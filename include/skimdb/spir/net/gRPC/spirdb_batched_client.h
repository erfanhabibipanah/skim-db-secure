#ifndef SPIRDB_BATCHED_CLIENT_H
#define SPIRDB_BATCHED_CLIENT_H

#include <expected>
#include <future>
#include <generator>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim::spir::rpc {

using shared_encoding_type = std::shared_ptr<skim::detail::encoding>;


struct kmer_req {
  std::uint32_t value;
  std::uint64_t row_idx;
  
  std::promise<shared_encoding_type> promise;
};


class batch_request {
public:
  explicit batch_request(std::uint32_t max) : count_{0}, max_{max}, occupied_(max, false), col_idxs_(max) {}

  const std::uint64_t& operator[](std::size_t i) const { return col_idxs_[i]; }

  auto count() const -> std::uint32_t { return count_; }

  auto full() const -> bool { return count_ >= max_; }

  auto free(std::uint32_t p) const -> bool { return !occupied_[p]; }

  auto set(std::uint32_t p, std::uint64_t col_idx, kmer_req req) -> bool {
    if (!free(p)) { return false; }
    occupied_[p] = true;
    col_idxs_[p] = col_idx;
    requests_.push_back(std::move(req));
    ++count_;
    return true;
  }

  void add(kmer_req req) { requests_.push_back(std::move(req)); }

private:
  std::uint32_t count_;
  std::uint32_t max_;

  std::vector<bool> occupied_;
  std::vector<std::uint64_t> col_idxs_;
  
  std::vector<kmer_req> requests_;
};


class BatchedSpirDBClient final {
public:
  explicit BatchedSpirDBClient(std::shared_ptr<grpc::Channel> channel) : 
      stub_{SpirDB::NewStub(channel)}, thread_{&BatchedSpirDBClient::m_run_, this} {
    // TODO: set up empty promise 
    g_log->debug("batched rpc client created!");
  }

  ~BatchedSpirDBClient() {
    { std::lock_guard lk{mtx_}; done_ = true; }
    cv_.notify_all();
    thread_.join();
  }

  auto setup() -> std::expected<void, std::string> {
    LogFun lf{"SpirDBClient::setup(...)"};

    g_log->debug("fetching db parameters from server...");

    grpc::ClientContext ctx1;
    DbParametersRequest db_req;
    DbParametersReply db_ans;
    grpc::Status db_status = stub_->GetDbParameters(&ctx1, db_req, &db_ans);

    if (!db_status.ok()) {
      return std::unexpected{db_status.error_message()};
    }

    g_log->debug("fetching db metadata from server...");

    grpc::ClientContext ctx2;
    DbMetadataRequest meta_req;
    DbMetadataReply meta_ans;
    grpc::Status meta_status = stub_->GetDbMetadata(&ctx2, meta_req, &meta_ans);

    if (!meta_status.ok()) {
      return std::unexpected{meta_status.error_message()};
    }

    phmap::parallel_flat_hash_map<std::uint32_t, std::uint64_t> index;

    for (const auto& kidx : meta_ans.index()) {
      index[kidx.first] = kidx.second;
    }

    std::vector<std::string> labels{meta_ans.labels().begin(), meta_ans.labels().end()};

    g_log->debug("fetching spir parameters from server...");

    grpc::ClientContext ctx3;
    SpirParametersRequest spir_req;
    SpirParametersReply spir_ans;
    grpc::Status spir_status = stub_->GetSpirParameters(&ctx3, spir_req, &spir_ans);

    if (!spir_status.ok()) {
      return std::unexpected{spir_status.error_message()};
    }

    g_log->debug("fetching spir hint from server...");

    grpc::ClientContext ctx4;
    SpirHintRequest hint_req;
    SpirHintReply hint_ans;
    grpc::Status hint_status = stub_->GetSpirHint(&ctx4, hint_req, &hint_ans);

    if (!hint_status.ok()) {
      return std::unexpected{hint_status.error_message()};
    }

    std::vector<std::uint64_t> hint_data{hint_ans.hint_c().begin(), hint_ans.hint_c().end()};

    g_log->debug("initializing client state...");

    skimdb_parameters skim_conf{.k = db_ans.k(), .s = db_ans.s(), .t = db_ans.t()};

    skimdb_metadata skim_meta{.index = std::move(index), .labels = std::move(labels)};

    spirdb_parameters spir_conf{.n = spir_ans.n(),
                                .sigma = spir_ans.sigma(),
                                .log_p = spir_ans.log_p(),
                                .log_q = spir_ans.log_q(),
                                .batch_size = spir_ans.batch_size(),
                                .block_size = spir_ans.block_size(),
                                .rle_blocks = spir_ans.rle_blocks(),
                                .sqrt_N = spir_ans.sqrt_n(),
                                .seed = spir_ans.seed()};

    spir_matrix hint_c{std::move(hint_data), spir_ans.sqrt_n(), spir_ans.n(), spir_ans.log_p()};

    state_.emplace(std::move(skim_conf), std::move(skim_meta), std::move(spir_conf), std::move(hint_c));

    return {};
  }

  [[nodiscard]] auto request(std::string kmer) -> std::shared_future<shared_encoding_type> {
    if (!state_.has_value()) {
      g_log->error("client not initialized! call setup() first...");
      return empty_promise_.get_future().share();
    }

    auto pos = client_state_->kmer_to_position(kmer);
    if (!pos) { return empty_promise_.get_future().share(); }
    auto [i_row, i_col] = pos->second;
    auto i_part = client_state_->row_to_partition(i_row);

    auto value = skim::detail::kmer_to_uint32(kmer);

    std::unique_lock lock{mtx_};

    auto pending_it = pending_.find(value);
    if (pending_it != pending_.end()) {
      return pending_it->second;
    }

    auto fulfilled_it = fulfilled_.find(value);
    if (fulfilled_it != fulfilled_.end()) {
      return fulfilled_it->second;
    }

    kmer_req request;
    request.value = value;
    request.row_idx = i_row;
    auto fut = request.promise.get_future().share();
    pending_.emplace(value, fut);

    for (auto &batch : queue_) {
      if (batch.free(i_part)) {
        batch.set(i_part, i_col, request);
        cv_.notify_one();
        return fut;
      } else if (batch[i_part] == i_col) {
        batch.add(request);
        cv_.notify_one();
        return fut;
      }
    }

    batch_request new_batch{client_state_->get_spir_parameters().batch_size};
    new_batch.set(i_part, i_col, request);
    queue_.push(std::move(new_batch));
    cv_.notify_one();
    return fut;
  }

private:
  void m_run_() {
    while (true) {
      std::unique_lock lock{mtx_};
      cv_.wait(lock, [&]{ return !queue_.empty() || done_; });

      while (!queue_.empty()) {
        auto task = std::move(queue_.front());
        queue_.pop();
        lock.unlock();

        // TODO: make rpc call and fulfill promises 

        lock.lock();
      }

      if (done_) return;
    }
  }

  void m_submit_(batch_request& batch) {
    
  }

  std::optional<spir_client_state> state_; // client state (initialized on setup)

  std::queue<batch_request> queue_;
  phmap::parallel_flat_hash_map<std::uint32_t, std::shared_future<shared_encoding_type>> pending_;
  phmap::parallel_flat_hash_map<std::uint32_t, std::shared_future<shared_encoding_type>> fulfilled_;

  std::promise<shared_encoding_type> empty_promise_;

  std::mutex mtx_;

  bool done_ = false;
  std::thread thread_;
  std::unique_ptr<SpirDB::Stub> stub_;
};

} // namespace skim::spir::rpc

#endif // SPIRDB_BATCHED_CLIENT_H