#ifndef SIPERDB_SERVICE_H
#define SIPERDB_SERVICE_H

#include <cstdint>
#include <utility>
#include <vector>

#include <google/protobuf/empty.pb.h>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/siper/skimdb_siper.h>

#include "proto/generated/siperdb.grpc.pb.h"


namespace skim::siper::rpc {

class SiperDBService final : public SiperDB::Service {
public:
  explicit SiperDBService(siper_server_state&& state) : state_{std::move(state)} {
    g_log->debug("rpc service created!");
  }

  grpc::Status
  GetDbParameters(grpc::ServerContext* context, const google::protobuf::Empty*, DbParametersReply* reply) override {
    LogFun lf{"SiperDBService::GetDbParameters(...)", spdlog::level::debug};
    g_log->trace("serving db parameters request from {}...", context->peer());

    auto [k, s, t] = state_.skim_parameters();

    reply->set_k(k);
    reply->set_s(s);
    reply->set_t(t);

    return grpc::Status::OK;
  }

  grpc::Status GetSiperParameters(grpc::ServerContext* context,
                                  const google::protobuf::Empty*,
                                  SiperParametersReply* reply) override {
    LogFun lf{"SiperDBService::GetSiperParameters(...)", spdlog::level::debug};
    g_log->trace("serving siper parameters request from {}...", context->peer());

    auto siper_params = state_.siper_parameters();

    reply->set_n(siper_params.n);
    reply->set_sigma(siper_params.sigma);
    reply->set_log_p(siper_params.log_p);
    reply->set_log_q(siper_params.log_q);
    reply->set_block_size(siper_params.block_size);
    reply->set_batch_size(siper_params.batch_size);
    reply->set_sqrt_n(siper_params.sqrt_N);
    reply->set_seed(siper_params.seed);
    reply->set_metadata_hash(siper_params.metadata_hash);
    reply->set_hint_c_hash(siper_params.hint_c_hash);

    return grpc::Status::OK;
  }

  // TODO: currently we do not have data consistency check (we may have collision on hash)
  grpc::Status DownloadData(grpc::ServerContext* context,
                            const DataRequest* request,
                            grpc::ServerWriter<DataChunk>* writer) override {
    LogFun lf{"SiperDBService::DownloadData(...)"};

    auto hash = request->hash();
    fs::path path = fs::path{g_skim_config.siper_server_store_dir} / fs::path{hash}.filename();

    g_log->trace("serving {} to {}...", path.string(), context->peer());

    std::ifstream f{path, std::ios::binary};

    if (!f) {
      return {grpc::StatusCode::NOT_FOUND, "requested file not found"};
    }

    constexpr std::size_t buf_size = 1 << 20; // 1MB

    std::array<char, buf_size> buff{};
    std::uint64_t offset{0};

    DataChunk chunk;

    while (f) {
      f.read(buff.data(), sizeof(buff));
      auto n = f.gcount();

      if (n <= 0) {
        break;
      }

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
    LogFun lf{"SiperDBService::Query(...)"};
    g_log->trace("serving siper query request from {}...", context->peer());

    // TODO: we should consider making siper_matrix non-owning :-)
    //       this way we could eliminate construction of query_vec
    //       and operate on request->qu.data() directly
    std::vector<std::uint64_t> query_vec_data{request->qu().begin(), request->qu().end()};

    if (query_vec_data.size() != state_.siper_parameters().sqrt_N) {
      return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "invalid query vector size"};
    }

    siper_matrix query_vec{
        std::move(query_vec_data), state_.siper_parameters().sqrt_N, state_.siper_parameters().log_q};
    auto ans = state_.answer(query_vec);

    if (!ans) {
      return grpc::Status(grpc::StatusCode::INTERNAL, ans.error());
    }

    auto ans_data = (*ans).span();
    reply->mutable_ans()->Assign(ans_data.begin(), ans_data.end());

    return grpc::Status::OK;
  }

  grpc::Status BatchQuery(grpc::ServerContext* context, const QueryRequest* request, QueryReply* reply) override {
    LogFun lf{"SiperDBService::BatchQuery(...)"};
    g_log->trace("serving siper batch query request from {}...", context->peer());

    auto siper_params = state_.siper_parameters();

    std::vector<std::uint64_t> query_vec_data{request->qu().begin(), request->qu().end()};

    if (query_vec_data.size() != siper_params.sqrt_N * siper_params.batch_size) {
      return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "invalid query vector size"};
    }

    siper_matrix query_vec{std::move(query_vec_data), siper_params.batch_size, siper_params.sqrt_N, siper_params.log_q};
    auto ans = state_.batch_answer(query_vec);

    if (!ans) {
      return grpc::Status(grpc::StatusCode::INTERNAL, ans.error());
    }

    auto ans_data = (*ans).span();
    reply->mutable_ans()->Assign(ans_data.begin(), ans_data.end());

    return grpc::Status::OK;
  }

private:
  siper_server_state state_;
};

} // namespace skim::siper::rpc

#endif // SIPERDB_SERVICE_H
