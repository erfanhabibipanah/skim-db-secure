#ifndef SPIRDB_SERVICE_H
#define SPIRDB_SERVICE_H

#include <cstdint>
#include <utility>
#include <vector>

#include <google/protobuf/empty.pb.h>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim::spir::rpc {

class SpirDBService final : public SpirDB::Service {
public:
  explicit SpirDBService(spir_server_state&& state) : state_(std::move(state)) { g_log->debug("rpc service created!"); }

  grpc::Status GetDbParameters(grpc::ServerContext* context, const google::protobuf::Empty*,
                               DbParametersReply* reply) override {
    LogFun lf{"SpirDBService::GetDbParameters(...)", spdlog::level::debug};
    g_log->trace("serving db parameters request from {}...", context->peer());

    auto [k, s, t] = state_.skim_parameters();

    reply->set_k(k);
    reply->set_s(s);
    reply->set_t(t);

    return grpc::Status::OK;
  }

  grpc::Status GetSpirParameters(grpc::ServerContext* context, const google::protobuf::Empty*,
                                 SpirParametersReply* reply) override {
    LogFun lf{"SpirDBService::GetSpirParameters(...)", spdlog::level::debug};
    g_log->trace("serving spir parameters request from {}...", context->peer());

    auto spir_params = state_.spir_parameters();

    reply->set_n(spir_params.n);
    reply->set_sigma(spir_params.sigma);
    reply->set_log_p(spir_params.log_p);
    reply->set_log_q(spir_params.log_q);
    reply->set_batch_size(spir_params.batch_size);
    reply->set_block_size(spir_params.block_size);
    reply->set_rle_blocks(spir_params.rle_blocks);
    reply->set_sqrt_n(spir_params.sqrt_N);
    reply->set_seed(spir_params.seed);
    reply->set_metadata_hash(spir_params.metadata_hash);
    reply->set_hint_c_hash(spir_params.hint_c_hash);

    return grpc::Status::OK;
  }

  // TODO: currently we do not have data consistency check (we may have collision on hash)
  grpc::Status DownloadData(grpc::ServerContext* context,
                            const DataRequest* request,
                            grpc::ServerWriter<DataChunk>* writer) override {
    LogFun lf{"SpirDBService::DownloadData(...)"};

    auto hash = request->hash();
    fs::path path = fs::path{g_spir_config.server_store_dir} / fs::path{hash}.filename();

    g_log->trace("serving {} to {}...", path.string(), context->peer());

    std::ifstream f{path, std::ios::binary};

    if (!f) {
      return {grpc::StatusCode::NOT_FOUND, "requested file not found"};
    }

    constexpr std::size_t buf_size = 1 << 20; // 1MB

    std::array<char, buf_size> buff{};
    std::uint64_t offset{0};

    while (f) {
      f.read(buff.data(), sizeof(buff));
      auto n = f.gcount();

      if (n <= 0) {
        break;
      }

      DataChunk chunk;

      chunk.set_data(buff.data(), n);
      chunk.set_offset(offset);

      if (!writer->Write(chunk)) {
        return {grpc::StatusCode::CANCELLED, "client disconnected"};
      }

      offset += n;
    }

    return grpc::Status::OK;
  }

  grpc::Status Query(grpc::ServerContext* context, const QueryRequest* request, QueryReply* reply) override {
    LogFun lf{"SpirDBService::Query(...)"};
    g_log->trace("serving spir query request from {}...", context->peer());

    // TODO: can we eliminate this copy?
    std::vector<std::uint64_t> query_vec_data{request->qu().begin(), request->qu().end()};

    if (query_vec_data.size() != state_.spir_parameters().sqrt_N) {
      return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "invalid query vector size"};
    }

    spir_matrix query_vec{std::move(query_vec_data), state_.spir_parameters().sqrt_N, state_.spir_parameters().log_q};
    auto ans = state_.answer(query_vec);

    if (!ans) {
      return grpc::Status(grpc::StatusCode::INTERNAL, ans.error());
    }

    auto ans_data = (*ans).span();
    reply->mutable_ans()->Assign(ans_data.begin(), ans_data.end());

    return grpc::Status::OK;
  }

  grpc::Status BatchQuery(grpc::ServerContext* context, const QueryRequest* request, QueryReply* reply) override {
    LogFun lf{"SpirDBService::BatchQuery(...)"};
    g_log->trace("serving spir batch query request from {}...", context->peer());

    auto spir_params = state_.spir_parameters();

    // TODO: can we eliminate this copy?
    std::vector<std::uint64_t> query_vec_data{request->qu().begin(), request->qu().end()};

    if (query_vec_data.size() != spir_params.sqrt_N * spir_params.batch_size) {
      return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "invalid query vector size"};
    }

    spir_matrix query_vec{std::move(query_vec_data), spir_params.batch_size, spir_params.sqrt_N, spir_params.log_q};
    auto ans = state_.batch_answer(query_vec);

    if (!ans) {
      return grpc::Status(grpc::StatusCode::INTERNAL, ans.error());
    }

    auto ans_data = (*ans).span();
    reply->mutable_ans()->Assign(ans_data.begin(), ans_data.end());

    return grpc::Status::OK;
  }

private:
  spir_server_state state_;
};

} // namespace skim::spir::rpc

#endif // SPIRDB_SERVICE_H
