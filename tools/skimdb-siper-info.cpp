#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/fmt_extra.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_version.h>
#include <skimdb/siper/skimdb_siper.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-siper-dbsize");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  std::string in = "";

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input siper database file", cxxopts::value<std::string>(in))
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

  if (in.empty()) {
    log->error("input not specified!");
    return -1;
  }

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  auto res = skim::siper::siper_server_state::info(dir);

  if (!res) {
    log->error("could not load {}, error: {}", in, res.error());
    return -1;
  }

  auto [tp, ver, skim_config, siper_config] = res.value();

  auto zt = std::chrono::zoned_time{std::chrono::current_zone(), std::chrono::round<std::chrono::seconds>(tp)};
  log->info("database info:\ntimestamp={}version={}\n{}{}",
            std::format("{:%Y-%m-%dT%H:%M:%S%Ez}\n", zt),
            std::to_string(ver),
            skim_config,
            siper_config);

  _Exit(0);
}
