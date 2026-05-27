#ifndef SIPERDB_CLIENT_H
#define SIPERDB_CLIENT_H

#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <generator>
#include <memory>
#include <optional>
#include <utility>

#include <google/protobuf/empty.pb.h>

#include <grpcpp/grpcpp.h>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/siper/skimdb_siper.h>

#include "proto/siperdb.grpc.pb.h"


namespace skim::siper::rpc {

namespace fs = std::filesystem;

class SiperDBClient {
public:
  explicit SiperDBClient(const std::string& addr = "127.0.0.1:50051") {
    grpc::ChannelArguments args;

    args.SetMaxReceiveMessageSize(-1);
    args.SetMaxSendMessageSize(-1);

    channel_ = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

    if (channel_->WaitForConnected(std::chrono::system_clock::now() +
                                   std::chrono::seconds(g_skim_config.grpc_connect_timeout))) {
      stub_ = SiperDB::NewStub(channel_);
    }
  }

  auto setup() -> std::expected<void, std::string> {
    LogFun lf{"SiperDBClient::setup(...)"};

    if (channel_->GetState(false) != GRPC_CHANNEL_READY) {
      return std::unexpected{"connection not established"};
    }

    g_log->info("fetching db parameters from server...");

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

    g_log->info("fetching siper parameters from server...");

    SiperParametersReply siper_ans;

    if (auto res = m_unary_rpc_(
            [this](grpc::ClientContext* ctx, const google::protobuf::Empty& req, SiperParametersReply* reply) {
              return stub_->GetSiperParameters(ctx, req, reply);
            },
            google::protobuf::Empty{},
            siper_ans);
        !res) {
      return std::unexpected{res.error().error_message()};
    }

    siperdb_parameters siper_conf{.n = siper_ans.n(),
                                  .sigma = siper_ans.sigma(),
                                  .log_p = siper_ans.log_p(),
                                  .log_q = siper_ans.log_q(),
                                  .block_size = siper_ans.block_size(),
                                  .batch_size = siper_ans.batch_size(),
                                  .sqrt_N = siper_ans.sqrt_n(),
                                  .seed = siper_ans.seed(),
                                  .metadata_hash = siper_ans.metadata_hash(),
                                  .hint_c_hash = siper_ans.hint_c_hash()};

    fs::path metadata_path = fs::path(g_skim_config.siper_client_metadata_dir) / siper_conf.metadata_hash;
    fs::path hint_c_path = fs::path(g_skim_config.siper_client_metadata_dir) / siper_conf.hint_c_hash;

    if (fs::exists(metadata_path) && fs::exists(hint_c_path)) {
      g_log->info("client metadata and hint_c found locally!");
    } else {
      g_log->info("downloading client metadata and hint_c from server...");

      auto res = m_get_data_(g_skim_config.siper_client_metadata_dir, siper_conf.metadata_hash);

      if (!res) {
        return std::unexpected{res.error()};
      }

      res = m_get_data_(g_skim_config.siper_client_hint_c_dir, siper_conf.hint_c_hash);

      if (!res) {
        return std::unexpected{res.error()};
      }
    }

    g_log->info("loading client state...");

    auto res = load_client(skim_conf, siper_conf);

    if (!res) {
      return std::unexpected{res.error()};
    }

    state_ = std::move(res.value());

    return {};
  }

  auto ready() const -> bool { return state_.has_value(); }

  auto query(const std::string& s) -> std::generator<const std::string&> {
    LogFun lf{"SiperDBClient::query(...)"};

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

    auto [row, col, len] = res.value();

    // determine how many queries need to be made
    const auto siper_parameters = state_->siper_parameters();
    auto num_queries = (row + len + siper_parameters.sqrt_N - 1) / siper_parameters.sqrt_N;

    if (num_queries > 1) {
      g_log->warn("query {} spans multiple columns ({})...", s, num_queries);
    }

    std::vector<std::uint16_t> rle(len);
    auto rle_span = std::span(rle);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < num_queries; ++i) {
      auto query_state = state_->prepare_query(col + i);
      auto qu_data = query_state.qu_vec.span();

      grpc::ClientContext ctx;
      QueryReply reply;

      {
        LogFun lf_sub{"gRPC query request", spdlog::level::debug};

        QueryRequest req;
        req.mutable_qu()->Assign(qu_data.begin(), qu_data.end());

        grpc::Status status = stub_->Query(&ctx, req, &reply);
        if (!status.ok()) {
          g_log->error("query failed: {}", status.error_message());
          co_return;
        }
      }

      std::vector<std::uint64_t> ans_data{reply.ans().begin(), reply.ans().end()};
      siper_matrix ans_mat{std::move(ans_data), siper_parameters.sqrt_N, siper_parameters.log_q};

      std::size_t count = std::min(len - offset, siper_parameters.sqrt_N - row);
      state_->recover(ans_mat, query_state, rle_span.subspan(offset, count), row, count, i);

      offset += count;
      row = 0; // subsequent queries (if any) will start from the top of the column
    }

    co_yield std::ranges::elements_of(state_->interpret(std::move(rle)));
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

  auto m_get_data_(const std::string& dest, const std::string& hash) -> std::expected<void, std::string> {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(g_skim_config.grpc_download_timeout));

    DataRequest req;
    req.set_hash(hash);

    auto reader = stub_->DownloadData(&ctx, req);

    fs::path name = fs::path{dest} / fs::path{hash}.filename();
    std::ofstream of{name, std::ios::binary};

    if (!of) {
      return std::unexpected{std::format("unable to create {}", name.string())};
    }

    DataChunk chunk;

    while (reader->Read(&chunk)) {
      of.write(chunk.data().data(), chunk.data().size());

      if (!of) {
        return std::unexpected{std::format("unable to create {}", name.string())};
      }
    }

    grpc::Status status = reader->Finish();

    if (!status.ok()) {
      return std::unexpected{status.error_message()};
    }

    return {};
  }

  std::optional<siper_client_state> state_; // client state (initialized on setup)

  std::shared_ptr<grpc::Channel> channel_{nullptr};
  std::unique_ptr<SiperDB::Stub> stub_{nullptr};
};

} // namespace skim::siper::rpc

#endif // SIPERDB_CLIENT
