#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb.h>
#include <skimdb/spir/skimdb_spir.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  std::string cache_dir = "";
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "spir database to query", cxxopts::value<std::string>(in))
      ("c,cache-dir", "directory for client metadata", cxxopts::value<std::string>(cache_dir))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-query");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  if (in.empty()) {
    log->error("input database not specified!");
    return -1;
  }

  if (cache_dir.empty()) {
    log->info("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::spir::g_spir_config.client_hint_c_dir = cache_dir;
  skim::spir::g_spir_config.client_metadata_dir = cache_dir;

  log->info("loading spir db from {}...", in);

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  auto setup = skim::spir::load_server(dir);

  if (!setup) {
    log->error("could not load {}, error: {}!", in, setup.error());
    return -1;
  }

  auto server_state = setup.value();

  log->info("creating client...");

  auto client_setup = skim::spir::load_client(server_state.skim_parameters(), server_state.spir_parameters());

  if (!client_setup) {
    log->error("could not create client: {}", client_setup.error());
    return -1;
  }

  auto client_state = client_setup.value();

  log->info("client ready for queries...");

  prompted_input prompt;
  std::string q{};

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("running query {}", q);
    }

    if (!client_state.is_valid_kmer(q)) {
      log->warn("invalid kmer: {}", q);
      continue;
    }

    auto pos = client_state.kmer_to_position(q);
    if (!pos) {
      log->info("kmer not found in DB: {}", q);
      continue;
    }

    auto query_state = client_state.prepare_query(pos->second);

    log->info("submitting query...");

    auto answer = server_state.answer(query_state.qu_vec);

    if (!answer) {
      log->warn("could not get answer: {}", answer.error());
      continue;
    }

    auto answer_vec = answer.value();

    log->info("recovering result...");

    if (verbose) {
      log->info("query results:");

      for (auto label : client_state.result(answer_vec, query_state, pos->first)) {
        log->info("  {}", label);
      }
    } else {
      log->info("got {} label(s)", std::ranges::distance(client_state.result(answer_vec, query_state, pos->first)));
    }
  }

  log->info("done!");

  return 0;
}
