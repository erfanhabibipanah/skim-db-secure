#ifndef SPIRDB_CLIENT_H
#define SPIRDB_CLIENT_H

#include <expected>
#include <generator>
#include <memory>
#include <optional>
#include <utility>

#include <google/protobuf/empty.pb.h>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/spir/skimdb_spir.h>

#include "proto/spirdb.grpc.pb.h"


namespace skim::spir::rpc {

namespace fs = std::filesystem;

class SpirDBClient {
public:
  explicit SpirDBClient(const std::string& addr = "127.0.0.1:50051") {
    grpc::ChannelArguments args;

    args.SetMaxReceiveMessageSize(-1);
    args.SetMaxSendMessageSize(-1);

    channel_ = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

    if (!channel_->WaitForConnected(std::chrono::system_clock::now() +
                                    std::chrono::seconds(g_spir_config.grpc_timeout))) {
      stub_ = SpirDB::NewStub(channel_);
    }
  }

  auto setup() -> std::expected<void, std::string> {
    LogFun lf{"SpirDBClient::setup(...)"};

    if ((!channel_) || (channel_->GetState(false) != GRPC_CHANNEL_READY)) {
      return std::unexpected{"connection not established"};
    }

    g_log->debug("fetching db parameters from server...");

    DbParametersReply db_ans;

    if (auto res = m_unary_rpc_([this](grpc::ClientContext* ctx,
                                       const google::protobuf::Empty& req,
                                       DbParametersReply* reply) { return stub_->GetDbParameters(ctx, req, reply); },
                                google::protobuf::Empty{},
                                db_ans);
        !res) {
      return std::unexpected{res.error().error_message()};
    }

    skimdb_parameters skim_conf{.k = db_ans.k(), .s = db_ans.s(), .t = db_ans.t()};

    g_log->debug("fetching spir parameters from server...");

    SpirParametersReply spir_ans;

    if (auto res = m_unary_rpc_(
            [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, SpirParametersReply* reply) {
              return stub_->GetSpirParameters(ctx, req, reply);
            },
            google::protobuf::Empty{},
            spir_ans);
        !res) {
      return std::unexpected{res.error().error_message()};
    }

    spirdb_parameters spir_conf{.n = spir_ans.n(),
                                .sigma = spir_ans.sigma(),
                                .log_p = spir_ans.log_p(),
                                .log_q = spir_ans.log_q(),
                                .batch_size = spir_ans.batch_size(),
                                .block_size = spir_ans.block_size(),
                                .rle_blocks = spir_ans.rle_blocks(),
                                .sqrt_N = spir_ans.sqrt_n(),
                                .seed = spir_ans.seed(),
                                .metadata_hash = spir_ans.metadata_hash(),
                                .hint_c_hash = spir_ans.hint_c_hash()};

    g_log->debug("searching for client metadata...");

    fs::path metadata_path = fs::path(g_spir_config.client_metadata_dir) / spir_conf.metadata_hash;
    fs::path hint_c_path = fs::path(g_spir_config.client_metadata_dir) / spir_conf.hint_c_hash;

    if (fs::exists(metadata_path) && fs::exists(hint_c_path)) {
      g_log->debug("client metadata and hint_c found locally, initializing client state...");

      auto res = load_client(skim_conf, spir_conf, metadata_path, hint_c_path);
      if (!res) {
        return std::unexpected{res.error()};
      }

      state_ = std::move(res.value());
      return {};
    } else {
      // TODO: implement metadata fetching (e.g. via FTP) if not found locally
      return std::unexpected{"client metadata or hint_c not found ... FTP download not implemented yet"};
    }
  }

  auto ready() const -> bool { return state_.has_value(); }

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

  auto skim_parameters() -> std::optional<skimdb_parameters> {
    if (state_.has_value()) {
      return state_.value().skim_parameters();
    }
    return std::nullopt;
  }

protected:
  template <typename RpcFn, typename Request, typename Reply>
  auto m_unary_rpc_(RpcFn&& rpc, const Request& req, Reply& reply) -> std::expected<void, grpc::Status> {
    grpc::ClientContext ctx;
    grpc::Status status = rpc(&ctx, req, &reply);

    if (!status.ok()) {
      return std::unexpected{status};
    }

    return {};
  };

  std::optional<spir_client_state> state_; // client state (initialized on setup)

  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<SpirDB::Stub> stub_;
};

} // namespace skim::spir::rpc

#endif // SPIRDB_CLIENT_H
