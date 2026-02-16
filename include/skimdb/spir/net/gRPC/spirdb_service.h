#ifndef SPIRDB_SERVICE_H
#define SPIRDB_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim {
namespace spir {
namespace rpc {

class SpirDBService final : public SpirDB::Service {
public:
  explicit SpirDBService(spir_server_state&& state) : state_(std::move(state)) { g_log->debug("rpc service created!"); }

  grpc::Status GetDbParameters(grpc::ServerContext* context, const DbParametersRequest*, DbParametersReply* reply) override {
    g_log->debug("serving DB parameters request from {}...", context->peer());

    auto [k, s, t] = state_.skim_parameters();

    reply->set_k(k);
    reply->set_s(s);
    reply->set_t(t);

    return grpc::Status::OK;
  }

  grpc::Status GetDbMetadata(grpc::ServerContext* context, const DbMetadataRequest*, DbMetadataReply* reply) override {
    g_log->debug("serving DB metadata request from {}...", context->peer());

    for (const auto& kidx : state_.skim_metadata().index) {
      (*reply->mutable_index())[kidx.first] = kidx.second;
    }

    *reply->mutable_labels() = {state_.skim_metadata().labels.begin(), state_.skim_metadata().labels.end()};
    return grpc::Status::OK;
  }

  grpc::Status GetSpirParameters(grpc::ServerContext* context, const SpirParametersRequest*, SpirParametersReply* reply) override {
    g_log->debug("serving SPIR parameters request from {}...", context->peer());

    auto spir_params = state_.spir_parameters();

    reply->set_n(spir_params.n);
    reply->set_sigma(spir_params.sigma);
    reply->set_rle_blocks(spir_params.rle_blocks);
    reply->set_sqrt_n(spir_params.sqrt_N);
    reply->set_log_p(spir_params.log_p);
    reply->set_log_q(spir_params.log_q);
    reply->set_seed(spir_params.seed);

    return grpc::Status::OK;
  }

  grpc::Status GetSpirHint(grpc::ServerContext* context, const SpirHintRequest*, SpirHintReply* reply) override {
    g_log->debug("serving SPIR hint request from {}...", context->peer());

    const auto& hint_c = state_.hint_c();
    auto data = hint_c.vec();

    *reply->mutable_hint_c() = {data.begin(), data.end()};
  
    return grpc::Status::OK;
  }

  grpc::Status Query(grpc::ServerContext* context, const QueryRequest* request,
                     QueryReply* reply) override {
    g_log->debug("serving SPIR query request from {}...", context->peer());
    
    std::vector<std::uint64_t> query_vec_data{request->qu().begin(), request->qu().end()};
    if (query_vec_data.size() != state_.spir_parameters().sqrt_N) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid query vector size");
    }

    spir_matrix query_vec{std::move(query_vec_data), state_.spir_parameters().sqrt_N, 1, state_.spir_parameters().log_q};
    auto ans = state_.answer(query_vec);

    if (!ans) {
      return grpc::Status(grpc::StatusCode::INTERNAL, ans.error());
    }

    auto ans_data = (*ans).vec();

    *reply->mutable_ans() = {ans_data.begin(), ans_data.end()};

    return grpc::Status::OK;
  }

private:
  spir_server_state state_;
};

} // namespace rpc
} // namespace spir
} // namespace skim

#endif // SPIRDB_SERVICE_H
