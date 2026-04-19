#include "skimdb/detail/skimdb_definitions.h"
#include <array>
#include <iostream>
#include <string>
#include <thread>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/spir/net/gRPC/spirdb_client.h>


void run_query(skim::spir::rpc::SpirDBClient& client) {
  auto state = client.ready();
  skim::skimdb_parameters param = client.skim_parameters().value();
  
}

auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  unsigned int nt = 1;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("t,threads", "number of query threads", cxxopts::value<unsigned int>(nt)->default_value(std::to_string(nt)))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-query-rpc-flood");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("connecting to {}...", addr);

  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(-1);
  args.SetMaxSendMessageSize(-1);

  auto channel = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
    log->error("unable to connect to {}!", addr);
    return -1;
  }

  skim::spir::rpc::SpirDBClient client{channel};
  log->info("connection established, preparing for queries...");

  auto res = client.setup();

  if (!res) {
    log->error("rpc setup failed: {}", res.error());
    return -1;
  }

  std::array<std::jthread, 8> threads;

  for (auto& t : threads) {
    t = std::jthread(run_query, std::ref(client));
  }

  log->info("done!");

  return 0;
}
