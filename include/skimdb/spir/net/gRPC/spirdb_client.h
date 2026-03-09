#ifndef SPIRDB_CLIENT_H
#define SPIRDB_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <generator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim {
namespace spir {
namespace rpc {

class SpirDBClient final {
public:
  explicit SpirDBClient(std::shared_ptr<grpc::Channel> channel) : stub_(SpirDB::NewStub(channel)) {
    g_log->debug("rpc client created!");
  }

  auto setup() -> std::expected<void, std::string> {
    LogFun lf{"SpirDBClient::setup(...)"};

    g_log->debug("fetching DB parameters from server...");
    grpc::ClientContext ctx1;
    DbParametersRequest db_req;
    DbParametersReply db_ans;
    grpc::Status db_status = stub_->GetDbParameters(&ctx1, db_req, &db_ans);
    if (!db_status.ok()) {
      return std::unexpected{db_status.error_message()};
    }

    g_log->debug("fetching DB metadata from server...");
    grpc::ClientContext ctx2;
    DbMetadataRequest meta_req;
    DbMetadataReply meta_ans;
    grpc::Status meta_status = stub_->GetDbMetadata(&ctx2, meta_req, &meta_ans);
    if (!meta_status.ok()) {
      return std::unexpected{meta_status.error_message()};
    }

    phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index;
    for (const auto& kidx : meta_ans.index()) {
      index[kidx.first] = kidx.second;
    }

    std::vector<std::string> labels{meta_ans.labels().begin(), meta_ans.labels().end()};

    g_log->debug("fetching SPIR parameters from server...");
    grpc::ClientContext ctx3;
    SpirParametersRequest spir_req;
    SpirParametersReply spir_ans;
    grpc::Status spir_status = stub_->GetSpirParameters(&ctx3, spir_req, &spir_ans);
    if (!spir_status.ok()) {
      return std::unexpected{spir_status.error_message()};
    }

    g_log->debug("fetching SPIR hint from server...");
    grpc::ClientContext ctx4;
    SpirHintRequest hint_req;
    SpirHintReply hint_ans;
    grpc::Status hint_status = stub_->GetSpirHint(&ctx4, hint_req, &hint_ans);
    if (!hint_status.ok()) {
      return std::unexpected{hint_status.error_message()};
    }

    std::vector<std::uint64_t> hint_data{hint_ans.hint_c().begin(), hint_ans.hint_c().end()};

    g_log->debug("initializing client state...");
    skimdb_parameters skim_conf{db_ans.k(), db_ans.s(), db_ans.t()};
    skimdb_metadata skim_meta{std::move(index), std::move(labels)};
    spirdb_parameters spir_conf{spir_ans.n(), spir_ans.sigma(), spir_ans.log_p(), spir_ans.log_q(), spir_ans.batch_size(), spir_ans.block_len(), spir_ans.rle_blocks(), spir_ans.sqrt_n(), spir_ans.seed()};
    spir_matrix hint_c{std::move(hint_data), spir_ans.sqrt_n(), spir_ans.n(), spir_ans.log_p()};

    state_.emplace(std::move(skim_conf), std::move(skim_meta), std::move(spir_conf), std::move(hint_c));

    return {};
  }

  auto query(const std::string& s) -> std::generator<const std::string&> {
    LogFun lf{"SpirDBClient::query(...)"};

    if (!state_.has_value()) {
      g_log->error("client not initialized! call setup() first.");
      co_return;
    }

    auto query = state_->prepare_query(s);
    if (!query) {
      g_log->error("failed to prepare query: {}", query.error());
      co_return;
    }

    auto query_state = query.value();
    auto qu_data = query_state.qu_vec.span();

    grpc::ClientContext ctx;

    QueryRequest req;
    *req.mutable_qu() = {qu_data.begin(), qu_data.end()};

    QueryReply reply;
    grpc::Status status = stub_->Query(&ctx, req, &reply);
    if (!status.ok()) {
      g_log->error("query failed: {}", status.error_message());
      co_return;
    }

    std::vector<std::uint64_t> ans_data{reply.ans().begin(), reply.ans().end()};

    auto pir_params = state_->get_spir_parameters();
    spir_matrix ans_mat{std::move(ans_data), pir_params.sqrt_N, pir_params.log_q};

    co_yield std::ranges::elements_of(state_->result(ans_mat, query_state));
  }

private:
    std::optional<spir_client_state> state_; // client state (initialized on setup)

    std::unique_ptr<SpirDB::Stub> stub_;
};

} // namespace rpc
} // namespace spir
} // namespace skim

#endif // SPIRDB_CLIENT_H
