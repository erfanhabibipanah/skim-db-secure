/***
 *  $Id$
 **
 *  File: skimdb_client.h
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#ifndef SKIMDB_CLIENT_H
#define SKIMDB_CLIENT_H

#include <expected>
#include <string>
#include <generator>

#include <grpcpp/grpcpp.h>
#include <skimdb/skimdb.h>

#include "proto/generated/skimdb.grpc.pb.h"


namespace skim::rpc {

class SkimDBClient final {
public:
  explicit SkimDBClient(const std::string& addr = "127.0.0.1:50051") {
    grpc::ChannelArguments args;

    args.SetMaxReceiveMessageSize(-1);
    args.SetMaxSendMessageSize(-1);

    channel_ = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

    if (channel_->WaitForConnected(std::chrono::system_clock::now() +
                                   std::chrono::seconds(g_skim_config.grpc_connect_timeout))) {
      stub_ = SkimDB::NewStub(channel_);
    }
  }


  auto parameters() -> std::expected<skimdb::parameters_type, std::string> const {
    ParametersRequest req;
    ParametersReply ans;
    grpc::ClientContext ctx;

    grpc::Status status = stub_->GetParameters(&ctx, req, &ans);

    if (!status.ok()) {
      std::unexpected{status.error_message()};
    }

    return skimdb::parameters_type{.k = ans.k(), .s = ans.s(), .t = ans.t()};
  }

  auto query(const std::string& q) const -> std::generator<std::string> {
    QueryRequest req;
    req.set_query(q);

    grpc::ClientContext ctx;
    auto reader = stub_->Query(&ctx, req);

    if (!reader) {
      g_log->error("failed to create reader!");
      co_return;
    }

    Label label;
    while (reader->Read(&label)) {
      co_yield label.value();
    }

    reader->Finish();
  }

private:
  std::shared_ptr<grpc::Channel> channel_{nullptr};
  std::unique_ptr<SkimDB::Stub> stub_{nullptr};
};

} // namespace skim::rpc

#endif // SKIMDB_CLIENT_H
