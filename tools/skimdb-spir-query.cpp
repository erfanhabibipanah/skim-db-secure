#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
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
  std::string cache_dir = "";
  bool verbose = false;
  bool use_rlwe = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "spir database to query", cxxopts::value<std::string>(in))
      ("c,cache-dir", "directory for client metadata", cxxopts::value<std::string>(cache_dir))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-query");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  if (in.empty()) {
    log->error("input database not specified!");
    return -1;
  }

  if (cache_dir.empty()) {
    log->debug("client cache directory not specified! using local directory...");
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

  auto client_state = std::move(client_setup.value());

#ifdef SKIMDB_USE_RLWE
  if (use_rlwe) {
    auto spir_conf = server_state.spir_parameters();
    std::uint64_t poly_degree = spir_conf.n;
    std::uint64_t p_mod = 1ULL << spir_conf.log_p;
    client_state.init_rlwe(poly_degree, p_mod);
  }
#endif

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

#ifdef SKIMDB_USE_RLWE
    if (use_rlwe) {
      auto hybrid = client_state.prepare_query_hybrid(q);
      if (!hybrid) {
        log->warn("hybrid query failed: {}", hybrid.error());
        continue;
      }
      auto& query_state = hybrid->second;

      log->info("submitting query...");
      auto answer = server_state.answer(query_state.qu_vec);
      if (!answer) { log->warn("could not get answer: {}", answer.error()); continue; }
      auto answer_vec = answer.value();
      log->info("recovering result...");

      if (verbose) {
        log->info("query results:");
        for (auto label : client_state.result_hybrid(answer_vec, query_state, pos->first)) {
          log->info("  {}", label);
        }
      } else {
        log->info("got {} label(s)", std::ranges::distance(client_state.result_hybrid(answer_vec, query_state, pos->first)));
      }
    } else {
#endif
    auto query_state = client_state.prepare_query(pos->second);
#ifdef SKIMDB_USE_RLWE
    // fall through to standard LWE path below
#endif

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
#ifdef SKIMDB_USE_RLWE
    } // close else from RLWE branch
#endif
  }

  log->info("done!");

  return 0;
}
