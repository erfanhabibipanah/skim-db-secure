/***
 *  $Id$
 **
 *  File: skimdb-index-query.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/net/gRPC/skimdb_client.h>


auto main(int argc, char* argv[]) -> int {
  std::string addr{};

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value("127.0.0.1:50051"))
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
  auto log = spdlog::stdout_color_mt("skimdb-index-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("connecting to {}...", addr);

  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());

  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
    log->error("unable to connect to {}!", addr);
    return -1;
  }

  skim::rpc::SkimDBClient client{channel};

  auto res = client.parameters();

  if (!res) {
    log->error("rpc failed, error: {}!", res.error());
    return -1;
  }

  auto [k, s, t] = res.value();

  log->info("connection established, [k={}, s={}, t={}]", k, s, t);
  log->info("ready for queries...");

  std::string q{};

  std::cout << ">";
  while (std::cin >> q && std::cin && !std::cin.eof()) {
    for (const auto& l : client.query(q)) {
      std::cout << "  " << l << std::endl;
    }
    std::cout << ">";
  }
  std::cout << "\n";

  log->info("done!");

  return 0;
}
