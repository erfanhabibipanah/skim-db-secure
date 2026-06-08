#ifndef SIPERDB_BATCHED_CLIENT_H
#define SIPERDB_BATCHED_CLIENT_H

#include <atomic>
#include <future>
#include <generator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/siper/skimdb_siper.h>

#include <tbb/global_control.h>
#include <tbb/task_arena.h>

#include "siperdb_client.h"


namespace skim::siper::rpc {

using result_t = std::vector<std::uint16_t>;
using shared_result_type = std::shared_ptr<result_t>;


struct kmer_request {
  skim::kmer_binary_t kmer;

  result_t result;
  std::atomic<int> wait; // number of fragments that have not yet returned results

  std::shared_ptr<std::promise<shared_result_type>> promise;
  std::shared_future<shared_result_type> future;

  kmer_request(skim::kmer_binary_t k, std::size_t len, std::size_t num_parts)
      : kmer{k}, result(len), wait{static_cast<int>(num_parts)},
        promise{std::make_shared<std::promise<shared_result_type>>()}, future{promise->get_future().share()} {}
};


struct kmer_fragment {
  std::shared_ptr<kmer_request> parent_req;

  std::size_t partition; // batch partition index
  std::size_t row_idx;   // starting index of the result with respect to the full column
  std::size_t col_idx;   // column index to request within the partition

  std::size_t offset; // starting index within the parent request's result vector to write to
  std::size_t len;    // length of the result fragment (in number of runs)

  kmer_fragment(
      std::shared_ptr<kmer_request> req, std::size_t p, std::size_t r, std::size_t c, std::size_t o, std::size_t l)
      : parent_req{std::move(req)}, partition{p}, row_idx{r}, col_idx{c}, offset{o}, len{l} {}
};


class batch_request {
public:
  explicit batch_request(std::size_t max)
      : max_{max}, occupied_(max, false), col_idxs_(max), created_at{std::chrono::steady_clock::now()} {
    requests_.reserve(max);
  }

  auto operator[](std::size_t i) const -> const std::uint64_t& { return col_idxs_[i]; }

  [[nodiscard]] auto size() const -> std::size_t { return count_; }

  [[nodiscard]] auto max() const -> std::size_t { return max_; }

  [[nodiscard]] auto free(std::size_t p) const -> bool { return !occupied_[p]; }

  auto set(kmer_fragment&& req) -> bool {
    if (!free(req.partition)) {
      return false;
    }
    occupied_[req.partition] = true;
    col_idxs_[req.partition] = req.col_idx;
    requests_.push_back(std::move(req));
    ++count_;
    return true;
  }

  void add(kmer_fragment&& req) { requests_.emplace_back(std::move(req)); }

  [[nodiscard]] auto created() const -> std::chrono::steady_clock::time_point { return created_at; }

  [[nodiscard]] auto expired(std::chrono::milliseconds timeout) const -> bool {
    auto now = std::chrono::steady_clock::now();
    return now >= created_at + timeout;
  }

  [[nodiscard]] auto requests() -> std::vector<kmer_fragment>& { return requests_; }

private:
  std::size_t count_{0};
  std::size_t max_{0};

  std::vector<bool> occupied_;
  std::vector<std::size_t> col_idxs_;

  std::vector<kmer_fragment> requests_;

  std::chrono::steady_clock::time_point created_at;
};


class BatchedSiperDBClient : public SiperDBClient {
public:
  explicit BatchedSiperDBClient(int max_threads,
                                double submit_threshold,
                                std::uint64_t batch_timeout_ms,
                                const std::string& addr = "127.0.0.1:50051")
      : SiperDBClient(addr), submit_threshold_{submit_threshold},
        batch_timeout_{std::chrono::milliseconds(batch_timeout_ms)}, leader_{&BatchedSiperDBClient::m_run_, this},
        arena_{max_threads} {
    auto null_res = std::make_shared<result_t>(); // empty vector<std::uint16_t>
    std::promise<shared_result_type> p;
    empty_future_ = p.get_future().share();
    p.set_value(null_res);

    g_log->debug("batched gRPC client created!");
  }

  ~BatchedSiperDBClient() {
    {
      std::lock_guard lck{mtx_};
      done_ = true;
    }

    cv_.notify_all();
    leader_.join();
    // arena waits for tasks to finish automatically
  }

  auto query(const std::string& s) -> std::generator<const std::string&> = delete;

  [[nodiscard]] auto request(std::string kmer) -> std::shared_future<shared_result_type> {
    if (!ready()) {
      throw std::runtime_error("request called before setup");
    }

    auto pos = state_->kmer_to_position(kmer);

    if (!pos.has_value()) {
      // valid kmer not found in index -> result is empty
      return empty_future_;
    }

    auto [i_row, i_col, len] = pos.value();
    auto [i_part, p_len] = state_->row_to_partition(i_row, len);

    if (p_len > 1) {
      // the RLE for this kmer spans multiple batch partitions, each of which must be queried separately
      // this harms performance and introduces risk of information leakage
      g_log->warn("kmer {} has RLE length {} and spans {} batch partitions, security risk!", kmer, len, p_len);
    }

    const auto siper_params = state_->siper_parameters();

    // TODO: cache results for recently requested kmers to avoid repeated requests
    auto req = std::make_shared<kmer_request>(detail::kmer_to_binary(kmer), len, p_len);

    std::size_t blocks_per_col = siper_params.sqrt_N / siper_params.block_size;
    std::size_t blocks_per_part = blocks_per_col / siper_params.batch_size;
    std::size_t remaining_blocks = blocks_per_col % siper_params.batch_size;

    std::size_t offset = 0;
    for (std::size_t i = 0; i < p_len; ++i) {
      std::size_t p =
          (i_part + i) % siper_params.batch_size; // wrap around to the beginning if partitions exceed batch size
      std::size_t r = (i_row + offset) % siper_params.sqrt_N;         // row index within the column for this fragment
      std::size_t c = i_col + (i_part + i) / siper_params.batch_size; // move to the next column if we wrap around

      std::size_t l = 0; // length of the fragment in number of runs

      if (p < remaining_blocks - 1) {
        l = std::min(len - offset, (blocks_per_part + 1) * (p + 1) * siper_params.block_size - r);
      } else {
        l = std::min(len - offset, ((blocks_per_part) * (p + 1) + remaining_blocks) * siper_params.block_size - r);
      }

      {
        std::unique_lock lock{mtx_};

        bool added = false;
        for (auto& batch : queue_) {
          if (batch.free(p)) {
            batch.set(kmer_fragment(req, p, r, c, offset, l));
            added = true;
            break;
          } else if (batch[p] == c) {
            batch.add(kmer_fragment(req, p, r, c, offset, l));
            added = true;
            break;
          }
        }

        if (!added) {
          batch_request new_batch{siper_params.batch_size};
          new_batch.set(kmer_fragment(req, p, r, c, offset, l));
          queue_.push_back(std::move(new_batch));
        }
      }

      offset += l;
    }

    cv_.notify_one();

    return req->future;
  }

  [[nodiscard]] auto interpret(const std::shared_future<shared_result_type>& ft) -> std::generator<const std::string&> {
    if (!ready()) {
      throw std::runtime_error("interpret called before setup");
    }

    const auto& res = ft.get(); // blocks if the result is not ready yet

    // TODO: want to remove this copy
    result_t rle(res->begin(), res->end());

    co_yield std::ranges::elements_of(state_->interpret(std::move(rle)));
  }

private:
  void m_run_() {
    auto next = std::chrono::steady_clock::time_point{};

    while (true) {
      std::unique_lock lock{mtx_};

      if (queue_.empty()) {
        cv_.wait(lock, [&] { return !queue_.empty() || done_; });
        if (done_) {
          return;
        }
      }

      next = queue_.front().created() + batch_timeout_;
      cv_.wait_until(lock, next, [&] { return (m_batch_ready_() || done_); });

      if (done_) {
        return;
      }

      while (m_batch_ready_()) {
        auto batch_ptr = std::make_unique<batch_request>(std::move(queue_.front()));
        queue_.pop_front();

        arena_.enqueue([this, ptr = std::move(batch_ptr)]() { m_submit_(ptr); });
      }
    }
  }

  [[nodiscard]] auto m_batch_ready_() const -> bool {
    if (queue_.empty()) {
      return false;
    }

    double ratio = static_cast<double>(queue_.front().size()) / static_cast<double>(queue_.front().max());
    return ratio >= submit_threshold_ || queue_.front().expired(batch_timeout_);
  }

  void m_submit_(const std::unique_ptr<batch_request>& batch_ptr) {
    auto& batch = *batch_ptr;

    g_log->trace("submitting batch of size {}...", batch.size());

    auto& client = *state_;
    const auto pir_params = client.siper_parameters();
    const auto batch_size = pir_params.batch_size;

    auto batch_state = client.new_batch();

    for (std::size_t i = 0; i < batch_size; ++i) {
      client.update_batch(batch_state, i, batch[i]);
    }

    auto qu_data = batch_state.qu_vec.span();

    grpc::ClientContext ctx;

    QueryRequest req;
    req.mutable_qu()->Assign(qu_data.begin(), qu_data.end());

    QueryReply reply;
    grpc::Status status = stub_->BatchQuery(&ctx, req, &reply);

    if (!status.ok()) {
      g_log->warn("batch query failed: {}", status.error_message());
      throw std::runtime_error(std::format("batch query failed: {}", status.error_message()));
    }

    std::vector<std::uint64_t> ans_data(reply.ans().begin(), reply.ans().end());
    siper_matrix ans_mat{std::move(ans_data), pir_params.sqrt_N, pir_params.log_q};

    for (auto& fragment : batch.requests()) {
      auto dst = std::span(fragment.parent_req->result).subspan(fragment.offset, fragment.len);
      client.recover(ans_mat, batch_state, dst, fragment.row_idx, fragment.len, fragment.partition);

      if (fragment.parent_req->wait.fetch_sub(1) == 1) {
        // this was the last fragment for this request, set the promise value
        fragment.parent_req->promise->set_value(std::make_shared<result_t>(std::move(fragment.parent_req->result)));
      }
    }
  }

  std::deque<batch_request> queue_;

  double submit_threshold_;
  std::chrono::milliseconds batch_timeout_;

  std::shared_future<shared_result_type> empty_future_;

  std::mutex mtx_;
  std::condition_variable cv_;

  std::jthread leader_;
  tbb::task_arena arena_;

  bool done_ = false;
};

} // namespace skim::siper::rpc

#endif // SIPERDB_BATCHED_CLIENT_H
