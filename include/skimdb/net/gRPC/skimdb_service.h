/***
 *  $Id$
 **
 *  File: skimdb_service.h
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#ifndef SKIMDB_SERVICE_H
#define SKIMDB_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <skimdb/skimdb.h>

#include "proto/generated/skimdb.grpc.pb.h"


namespace skim::rpc {

class SkimDBService final : public SkimDB::Service {
public:
  explicit SkimDBService(skimdb&& db) : db_(std::move(db)) { g_log->debug("rpc service created!"); }

  grpc::Status GetParameters(grpc::ServerContext* context, const ParametersRequest*, ParametersReply* reply) override {
    g_log->debug("serving parameters request from {}...", context->peer());

    auto [k, s, t] = db_.parameters();

    reply->set_k(k);
    reply->set_s(s);
    reply->set_t(t);

    return grpc::Status::OK;
  }

  grpc::Status Query(grpc::ServerContext* context, const QueryRequest* request,
                     grpc::ServerWriter<Label>* writer) override {
    g_log->debug("serving query request {} from {}...", request->query(), context->peer());

    Label label;

    for (const std::string& l : db_.query(request->query())) {
      if (context->IsCancelled()) {
        return grpc::Status::CANCELLED;
      }

      label.set_value(l);

      if (!writer->Write(label)) {
        break;
      }
    }

    return grpc::Status::OK;
  }

private:
  skimdb db_;
};

} // namespace skim::rpc

#endif // SKIMDB_SERVICE_H
