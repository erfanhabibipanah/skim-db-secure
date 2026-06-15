#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/siper/skimdb_siper.h>
#include <skimdb/skimdb.h>
#include <skimdb/skimdb_version.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-siper-query");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  std::string in = "";
  std::string cache_dir = "";
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()("i,input", "siper database to query", cxxopts::value<std::string>(in))(
        "c,cache-dir", "directory for client metadata", cxxopts::value<std::string>(cache_dir))(
        "v,verbose", "print recovered labels", cxxopts::value<bool>(verbose)->default_value(std::to_string(verbose)))(
        "h,help", "print this help");

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
    log->error("input database not specified!");
    return -1;
  }

  if (cache_dir.empty()) {
    log->debug("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.siper_client_hint_c_dir = cache_dir;
  skim::g_skim_config.siper_client_metadata_dir = cache_dir;

  log->info("loading siperdb from {}...", in);

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  auto setup = skim::siper::load_server(dir);

  if (!setup) {
    log->error("could not load {}, error: {}!", in, setup.error());
    return -1;
  }

  const auto& server_state = setup.value();

  auto [k, s, t] = server_state.skim_parameters();

  log->info("server ready, [k={}, s={}, t={}]", k, s, t);
  log->info("creating client...");

  auto siper_params = server_state.siper_parameters();
  auto client_setup = skim::siper::load_client(server_state.skim_parameters(), siper_params);

  if (!client_setup) {
    log->error("could not create client: {}", client_setup.error());
    return -1;
  }

  auto& client_state = client_setup.value();

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

    auto [row, col, len] = pos.value();
    auto n_queries = (row + len + siper_params.sqrt_N - 1) / siper_params.sqrt_N;
    log->debug("kmer at row {}, col {}, length {}, spans {} column(s)", row, col, len, n_queries);

    if (n_queries > 1) {
      log->warn("query {} spans multiple columns ({})...", q, n_queries);
    }

    std::vector<std::uint16_t> rle(len);
    auto rle_span = std::span(rle);

    std::size_t offset = 0;
    bool query_failed = false;

    for (std::size_t i = 0; i < n_queries; ++i) {
      log->debug("submitting query ({} of {})...", i + 1, n_queries);

      auto query_state = client_state.prepare_query(col + i);
      auto res = server_state.answer(query_state.qu_vec);

      if (!res) {
        log->warn("could not get answer: {}", res.error());
        query_failed = true;
        break;
      }

      const auto& ans = res.value();

      log->debug("recovering result ({} of {})...", i + 1, n_queries);

      std::size_t count = std::min(len - offset, siper_params.sqrt_N - row);
      client_state.recover(ans, query_state, rle_span.subspan(offset, count), row, count, 0);

      offset += count;
      row = 0; // subsequent queries (if any) will start from the top of the next column
    }

    if (query_failed) {
      continue;
    }

    if (verbose) {
      log->info("query results:");

      for (auto label : client_state.interpret(std::move(rle))) {
        log->info("  {}", label);
      }
    } else {
      log->info("got {} label(s)", std::ranges::distance(client_state.interpret(std::move(rle))));
    }
  }

  log->info("done!");

  _Exit(0);
}
