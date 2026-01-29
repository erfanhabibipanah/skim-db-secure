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

#include "proto/skimdb.grpc.pb.h"


namespace skim {
namespace rpc {

class SkimDBClient final {
public:
  explicit SkimDBClient(std::shared_ptr<grpc::Channel> channel) : stub_(SkimDB::NewStub(channel)) {
    g_log->debug("rpc client created!");
  }

  auto parameters() -> std::expected<skimdb::parameters_type, std::string> const {
    ParametersRequest req;
    ParametersReply ans;
    grpc::ClientContext ctx;

    grpc::Status status = stub_->GetParameters(&ctx, req, &ans);

    if (!status.ok()) {
      std::unexpected{status.error_message()};
    }

    return skimdb::parameters_type{ans.k(), ans.s(), ans.t()};
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
    std::unique_ptr<SkimDB::Stub> stub_;
};

} // namespace rpc
} // namespace skimdb

#endif // SKIMDB_CLIENT_H
