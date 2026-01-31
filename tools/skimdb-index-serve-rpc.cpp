/***
 *  $Id$
 **
 *  File: skimdb-index-serve.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/net/gRPC/skimdb_service.h>

namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in{};
  std::string addr{"0.0.0.0:50051"};

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "database to serve", cxxopts::value<std::string>(in))
      ("s,server", "serve on network:port", cxxopts::value<std::string>(addr)->default_value(addr))
      ("h,help", "print this help");

    auto opt_res = options.parse(argc, argv);

    if ((opt_res.unmatched().size() != 0) || opt_res.count("help")) {
      std::cout << options.help() << std::endl;
      return 0;
    }
  } catch (const cxxopts::exceptions::exception& e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-index-serve-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  if (in.empty()) {
    log->error("input database not specified!");
    return -1;
  }

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  log->info("loading index from {}...", in);

  skim::skimdb db;
  auto res = db.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  auto [k, s, t] = db.parameters();

  log->info("index loaded, [k={}, s={}, t={}]", k, s, t);

  skim::rpc::SkimDBService service(std::move(db));
  grpc::ServerBuilder builder;

  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  auto server = builder.BuildAndStart();

  if (!server) {
    log->error("could not create service on {}!", addr);
    return -1;
  }

  log->info("listening for queries on {}...", addr);

  server->Wait();

  log->info("done!");

  return 0;
}
