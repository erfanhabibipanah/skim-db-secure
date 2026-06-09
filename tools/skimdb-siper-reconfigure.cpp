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
  std::string in = "";
  bool update_batch_size = false;
  unsigned int batch_size = 0;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input siper database file", cxxopts::value<std::string>(in))
      ("b,batch-size","new batch size for siper database", cxxopts::value<unsigned int>(batch_size))
      ("h,help", "print this help");

    auto opt_res = options.parse(argc, argv);

    if ((opt_res.unmatched().size() != 0) || opt_res.count("help")) {
      std::cout << options.help() << std::endl;
      return 0;
    }

    update_batch_size = opt_res.count("batch-size") > 0;
  } catch (const cxxopts::exceptions::exception& e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-siper-dbsize");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  if (in.empty()) {
    log->error("input not specified!");
    return -1;
  }

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  log->info("loading siperdb from {}...", in);

  skim::siper::siper_server_state state;
  auto res = state.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  if (update_batch_size) {
    log->info("previous batch size is {}", state.siper_parameters().batch_size);
    state.update_batch_size(batch_size);
    log->info("batch size set to {}", state.siper_parameters().batch_size);

    log->info("saving siperdb to {}...", in);

    auto save_res = state.save(dir);

    if (!save_res) {
      log->error("could not save {}, error: {}!", in, save_res.error());
      return -1;
    }
  }

  log->info("done!");

  _Exit(0);
}
