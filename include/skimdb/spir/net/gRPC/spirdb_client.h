#ifndef SPIRDB_CLIENT_H
#define SPIRDB_CLIENT_H

#include <expected>
#include <generator>
#include <memory>
#include <optional>
#include <utility>

#include <google/protobuf/empty.pb.h>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim::spir::rpc {

class SpirDBClient final {
public:
  explicit SpirDBClient(std::shared_ptr<grpc::Channel> channel) : stub_(SpirDB::NewStub(channel)) {
    g_log->debug("rpc client created!");
  }

  auto setup() -> std::expected<void, std::string> {
    LogFun lf{"SpirDBClient::setup(...)"};

    g_log->debug("fetching db parameters from server...");

    DbParametersReply db_ans;
    if (auto res = m_unary_rpc_(
          [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, DbParametersReply* reply) {
            return stub_->GetDbParameters(ctx, req, reply);
          }, google::protobuf::Empty{}, db_ans);
        !res) {
      return std::unexpected{res.error()};
    }

    g_log->debug("fetching db metadata from server...");

    DbMetadataReply meta_ans;
    if (auto res = m_unary_rpc_(
          [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, DbMetadataReply* reply) {
            return stub_->GetDbMetadata(ctx, req, reply);
          }, google::protobuf::Empty{}, meta_ans);
        !res) {
      return std::unexpected{res.error()};
    }

    skimdb::kmer_index index;
    const std::string& buffer = meta_ans.index();
    std::istringstream is(buffer, std::ios::binary);
    cereal::BinaryInputArchive ar(is);
    ar(index);

    std::vector<std::string> labels{meta_ans.labels().begin(), meta_ans.labels().end()};

    g_log->debug("fetching spir parameters from server...");

    SpirParametersReply spir_ans;
    if (auto res = m_unary_rpc_(
          [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, SpirParametersReply* reply) {
            return stub_->GetSpirParameters(ctx, req, reply);
          }, google::protobuf::Empty{}, spir_ans);
        !res) {
      return std::unexpected{res.error()};
    }

    g_log->debug("fetching spir hint from server...");

    SpirHintReply hint_ans;
    if (auto res = m_unary_rpc_(
          [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, SpirHintReply* reply) {
            return stub_->GetSpirHint(ctx, req, reply);
          }, google::protobuf::Empty{}, hint_ans);
        !res) {
      return std::unexpected{res.error()};
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

  auto ready() const -> bool {
    return state_.has_value();
  }

  auto query(const std::string& s) -> std::generator<const std::string&> {
    LogFun lf{"SpirDBClient::query(...)"};

    if (!ready()) {
      g_log->error("client not initialized! call setup() first...");
      co_return;
    }

    if (!state_->is_valid_kmer(s)) {
      g_log->error("invalid kmer: {}", s);
      co_return;
    }

    auto res = state_->kmer_to_position(s);
    if (!res.has_value()) {
      // valid kmer not found in index -> result is empty
      co_return;
    }

    auto& pos = res.value();

    auto query_state = state_->prepare_query(pos.second);
    auto qu_data = query_state.qu_vec.span();

    grpc::ClientContext ctx;

    QueryRequest req;
    req.mutable_qu()->Assign(qu_data.begin(), qu_data.end());

    QueryReply reply;
    grpc::Status status = stub_->Query(&ctx, req, &reply);
    if (!status.ok()) {
      g_log->error("query failed: {}", status.error_message());
      co_return;
    }

    std::vector<std::uint64_t> ans_data{reply.ans().begin(), reply.ans().end()};

    auto pir_params = state_->spir_parameters();
    spir_matrix ans_mat{std::move(ans_data), pir_params.sqrt_N, pir_params.log_q};

    co_yield std::ranges::elements_of(state_->result(ans_mat, query_state, pos.first));
  }

private:
  template <typename RpcFn, typename Request, typename Reply>
  auto m_unary_rpc_(RpcFn&& rpc, const Request& req, Reply& reply) -> std::expected<void, std::string> {
    grpc::ClientContext ctx;
    grpc::Status status = rpc(&ctx, req, &reply);
    if (!status.ok()) {
      return std::unexpected{status.error_message()};
    }
    return {};
  };

  std::optional<spir_client_state> state_; // client state (initialized on setup)
  std::unique_ptr<SpirDB::Stub> stub_;
};

} // namespace skim::spir::rpc

#endif // SPIRDB_CLIENT_H
