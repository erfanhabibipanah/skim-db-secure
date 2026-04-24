#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb_version.h>
#include <skimdb/spir/net/gRPC/spirdb_client.h>
#include <skimdb/spir/skimdb_spir.h>


auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  std::string cache_dir = "";
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("a,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
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

  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-spir-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  if (cache_dir.empty()) {
    log->debug("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::spir::g_spir_config.client_hint_c_dir = cache_dir;
  skim::spir::g_spir_config.client_metadata_dir = cache_dir;

  log->info("connecting to {}...", addr);

  skim::spir::rpc::SpirDBClient client{addr};

  auto res = client.setup();
  if (!res) {
    log->error("rpc setup failed: {}", res.error());
    return -1;
  }

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

  return 0;
}
