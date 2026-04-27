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
#include "spirdb_client.h"


namespace skim::spir::rpc {

using labels_t = std::vector<std::string>;
using shared_result_type = std::shared_ptr<labels_t>;


struct kmer_req {
  std::uint32_t value;
  std::uint64_t row_idx;
  
  std::promise<shared_result_type> promise;
  std::shared_future<shared_result_type> future;
};


class batch_request {
public:
  explicit batch_request(std::size_t max) : count_{0}, max_{max}, occupied_(max, false), col_idxs_(max), created_at{std::chrono::steady_clock::now()} {}

  const std::uint64_t& operator[](std::size_t i) const { return col_idxs_[i]; }

  auto size() const -> std::size_t { return count_; }

  auto max() const -> std::size_t { return max_; }

  auto free(std::size_t p) const -> bool { return !occupied_[p]; }

  auto set(std::size_t p, std::size_t col_idx, kmer_req&& req) -> bool {
    if (!free(p)) { return false; }
    occupied_[p] = true;
    col_idxs_[p] = col_idx;
    requests_.push_back(std::move(req));
    ++count_;
    return true;
  }

  void add(kmer_req&& req) { requests_.push_back(std::move(req)); }

  auto created() const -> std::chrono::steady_clock::time_point { return created_at; }

  auto expired(std::chrono::milliseconds timeout) const -> bool {
    auto now = std::chrono::steady_clock::now();
    return now >= created_at + timeout;
  }

  auto requests() -> std::vector<kmer_req>& { return requests_; }

private:
  std::size_t count_;
  std::size_t max_;

  std::vector<bool> occupied_;
  std::vector<std::size_t> col_idxs_;
  
  std::vector<kmer_req> requests_;

  std::chrono::steady_clock::time_point created_at;
};


class BatchedSpirDBClient : public SpirDBClient {
public:
  explicit BatchedSpirDBClient(std::size_t max_threads,
                               double submit_threshold, 
                               std::uint64_t batch_timeout = 100,
                               const std::string& addr = "127.0.0.1:50051")
      : SpirDBClient(addr),
        max_threads_{max_threads},
        submit_threshold_{submit_threshold}, 
        batch_timeout_{std::chrono::milliseconds(batch_timeout)},
        leader_{&BatchedSpirDBClient::m_run_, this},
        threads_{} {
    auto labels = std::make_shared<labels_t>();  // empty vector<string>
    std::promise<shared_result_type> p;
    p.set_value(labels);
    empty_future_ = p.get_future().share();

    g_log->debug("batched rpc client created!");
  }

  ~BatchedSpirDBClient() {
    { std::lock_guard lk{mtx_}; done_ = true; }
    cv_.notify_all();
    leader_.join();
    
    for (auto &t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  auto query(const std::string& s) -> std::generator<const std::string&> = delete;

  [[nodiscard]] auto request(std::string kmer) -> std::shared_future<shared_result_type> {
    if (!ready()) {
      g_log->error("client not initialized! call setup() first...");
      return empty_future_;
    }

    auto pos = state_->kmer_to_position(kmer);
    if (!pos.has_value()) { return empty_future_; }
    auto [i_row, i_col] = pos.value();
    auto i_part = state_->row_to_partition(i_row);

    auto value = skim::detail::kmer_to_binary(kmer);

    std::unique_lock lock{mtx_};

    kmer_req request;
    request.value = value;
    request.row_idx = i_row;
    request.future = request.promise.get_future().share();

    for (auto &batch : queue_) {
      if (batch.free(i_part)) {
        batch.set(i_part, i_col, std::move(request));
        cv_.notify_one();
        return request.future;
      } else if (batch[i_part] == i_col) {
        batch.add(std::move(request));
        cv_.notify_one();
        return request.future;
      }
    }

    batch_request new_batch{state_->spir_parameters().batch_size};
    new_batch.set(i_part, i_col, std::move(request));
    queue_.push_back(std::move(new_batch));
    cv_.notify_one();
    return request.future;
  }

private:
  void m_run_() {
    auto next = std::chrono::steady_clock::time_point{};

    while (true) {
      std::unique_lock lock{mtx_};
      if (queue_.empty()) {
        cv_.wait(lock, [&]{ return !queue_.empty() || done_; });
        if (done_) {
          return;
        }
      }

      next = queue_.front().created() + batch_timeout_;
      cv_.wait_until(lock, next, [&]{ return (m_batch_ready_() || done_); });

      if (done_) {
        return;
      }

      while (m_batch_ready_()) {
        auto batch = std::move(queue_.front());
        queue_.pop_front();

        threads_.emplace_back(&BatchedSpirDBClient::m_submit_, this, std::move(batch));
      }
    }
  }

  auto m_batch_ready_() const -> bool {
    double ratio = static_cast<double>(queue_.front().size()) / queue_.front().max();
    return !queue_.empty() && (ratio >= submit_threshold_ || queue_.front().expired(batch_timeout_));
  }

  void m_submit_(batch_request batch) {
    g_log->trace("submitting batch of size {}...", batch.size());

    auto& client = *state_;
    const auto pir_params = client.spir_parameters();
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
      g_log->error("batch query failed: {}", status.error_message());
      for (auto& r : batch.requests()) {
        r.promise.set_value(
          std::make_shared<labels_t>()
        );
      }
      return;
    }

    std::vector<std::uint64_t> ans_data{
      reply.ans().begin(),
      reply.ans().end()
    };

    spir_matrix ans_mat{
      std::move(ans_data),
      pir_params.sqrt_N,
      pir_params.log_q
    };

    for (auto& r : batch.requests()) {
      labels_t labels;
      std::ranges::copy(client.result(ans_mat, batch_state, r.row_idx), std::back_inserter(labels));

      r.promise.set_value(
        std::make_shared<labels_t>(std::move(labels))
      );
    }
  }

  std::deque<batch_request> queue_;

  double submit_threshold_;
  std::chrono::milliseconds batch_timeout_;
  std::size_t max_threads_;

  std::shared_future<shared_result_type> empty_future_;

  std::mutex mtx_;
  std::condition_variable cv_;

  std::jthread leader_;
  std::vector<std::jthread> threads_;

  bool done_ = false;
};

} // namespace skim::spir::rpc

#endif // SPIRDB_BATCHED_CLIENT_H