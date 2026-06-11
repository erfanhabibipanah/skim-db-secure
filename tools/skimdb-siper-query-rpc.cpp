#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb_version.h>
#include <skimdb/siper/net/gRPC/siperdb_client.h>
#include <skimdb/siper/skimdb_siper.h>


auto main(int argc, char* argv[]) -> int {
  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-siper-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  std::string addr{"127.0.0.1:50051"};
  std::string cache_dir = "";
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("a,address", "server address to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("c,cache-dir", "directory for client cached data", cxxopts::value<std::string>(cache_dir))
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

  if (cache_dir.empty()) {
    log->debug("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.siper_client_hint_c_dir = cache_dir;
  skim::g_skim_config.siper_client_metadata_dir = cache_dir;

  log->info("connecting to {}...", addr);

  skim::siper::rpc::SiperDBClient client{addr};

  auto res = client.setup();

  if (!res) {
    log->error("connection failed: {}", res.error());
    return -1;
  }

  auto [k, s, t] = client.skim_parameters().value();

  log->info("connection established, [k={}, s={}, t={}]", k, s, t);
  log->info("ready for queries...");

  prompted_input prompt;
  std::string q{};

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("running query {}", q);
    }

    if (verbose) {
      for (const auto &l : client.query(q)) {
        log->info("  {}", l);
      }
    } else {
      log->info("got {} label(s)", std::ranges::distance(client.query(q)));
    }
  }

  log->info("done!");

  _Exit(0);
}
