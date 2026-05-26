#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/fmt_extra.h>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_version.h>
#include <skimdb/spir/skimdb_spir.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  std::string out = "";
  std::string cache_dir = "";
  unsigned int logp = 22;
  unsigned int logq = 64;
  unsigned int block_size = 1;
  unsigned int batch_size = 1;
  std::size_t n = 1923;
  double sigma = 271.65;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input skimdb database file", cxxopts::value<std::string>(in))
      ("o,output", "output file for server state", cxxopts::value<std::string>(out))
      ("c,cache-dir", "server store directory", cxxopts::value<std::string>(cache_dir))
      ("p,logp", "log of text modulus p", cxxopts::value<unsigned int>(logp)->default_value(std::to_string(logp)))
      ("q,logq", "log of cypher modulus q", cxxopts::value<unsigned int>(logq)->default_value(std::to_string(logq)))
      ("b,batch-size", "batch size", cxxopts::value<unsigned int>(batch_size)->default_value(std::to_string(batch_size)))
      ("n,secret-size", "secret size", cxxopts::value<std::size_t>(n)->default_value(std::to_string(n)))
      ("s,sigma", "variance of error distribution", cxxopts::value<double>(sigma)->default_value(std::to_string(sigma)))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-server-create");
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

  if (out.empty()) {
    log->error("output not specified!");
    return -1;
  }

  if (cache_dir.empty()) {
    log->debug("server store directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.spir_server_store_dir = cache_dir;

  log->info("loading index from {}...", in);

  skim::skimdb db;
  auto res = db.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  log->info("building spir server state...");

  auto setup = skim::spir::make_server(std::move(db), logp, logq, n, sigma, block_size, batch_size);

  if (!setup) {
    log->error("could not setup server state: {}", setup.error());
    return -1;
  }

  auto server_state = setup.value();

  log->info("saving server state to {}...", out);
  auto save_res = server_state.save(out);

  if (!save_res) {
    log->error("could not save server state: {}", save_res.error());
    return -1;
  } else {
    log->info("server state saved, size: {}B ({})", save_res.value(), as_fsize{save_res.value()});
  }

  log->info("done!");

  return 0;
}
