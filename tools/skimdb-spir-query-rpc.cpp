#include <cstdlib>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/spir/net/gRPC/spirdb_client.h>


auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  bool verbose = false;
  bool use_rlwe = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("v,verbose", "print recovered labels", cxxopts::value<bool>(verbose)->default_value(std::to_string(verbose)))
#ifdef SKIMDB_USE_RLWE
      ("rlwe", "use Ring-LWE hybrid query mode", cxxopts::value<bool>(use_rlwe)->default_value("false"))
#endif
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

  auto channel = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

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

#ifdef SKIMDB_USE_RLWE
  if (use_rlwe) {
    client.init_rlwe();
  }
#endif

  log->info("ready for queries...");

  prompted_input prompt;
  std::string q{};

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("running query {}", q);
    }

    if (verbose) {
#ifdef SKIMDB_USE_RLWE
      auto results = use_rlwe ? client.query_hybrid(q) : client.query(q);
#else
      auto results = client.query(q);
#endif
      for (const auto &l : results) {
        log->info("  {}", l);
      }
    } else {
#ifdef SKIMDB_USE_RLWE
      if (use_rlwe) {
        log->info("got {} label(s)", std::ranges::distance(client.query_hybrid(q)));
      } else {
#endif
        log->info("got {} label(s)", std::ranges::distance(client.query(q)));
#ifdef SKIMDB_USE_RLWE
      }
#endif
    }
  }

  log->info("done!");

  return 0;
}
