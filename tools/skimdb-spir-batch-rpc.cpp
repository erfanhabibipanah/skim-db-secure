#include <filesystem>
#include <iostream>

#include <cxxopts.hpp>
#include <fmt_extra/fmt_extra.h>

#include <parallel_hashmap/phmap.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/spir/skimdb_spir.h>


auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  unsigned int threads = 0;
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("t,threads", "number of threads submitting queries", cxxopts::value<unsigned int>(threads)->default_value(std::to_string(threads)))
      ("v,verbose", "print recovered labels", cxxopts::value<bool>(verbose)->default_value(std::to_string(verbose)))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("connecting to {}...", addr);

  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(-1);
  args.SetMaxSendMessageSize(-1);

  auto channel = grpc::CreateCustomChannel(
    addr,
    grpc::InsecureChannelCredentials(),
    args
  );

  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
    log->error("unable to connect to {}!", addr);
    return -1;
  }

  skim::spir::rpc::SpirDBClient client{channel};
  log->info("connection established, preparing for queries...");

  auto success = client.setup();
  if (!success) {
    log->error("rpc setup failed: {}", success.error());
    return -1;
  }

  if (client.get_spir_parameters().batch_size < 2) {
    log->error("server does not support batch queries, exiting...");
    return -1;
  }

  log->info("ready for queries...");

  // TODO

  log->info("done!");
  
  return 0;
}
