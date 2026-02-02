#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/spir/skimdb_spir.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  unsigned int logp = 16;
  unsigned int logq = 64;
  std::size_t n = 1000;
  double sigma = 6.4;
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input database file", cxxopts::value<std::string>(in))
      ("p,logp", "log of text modulus p", cxxopts::value<unsigned int>(logp)->default_value(std::to_string(logp)))
      ("q,logq", "log of cypher modulus q", cxxopts::value<unsigned int>(logq)->default_value(std::to_string(logq)))
      ("n", "secret size", cxxopts::value<std::size_t>(n)->default_value(std::to_string(n)))
      ("s,sigma", "variance of error distribution", cxxopts::value<double>(sigma)->default_value(std::to_string(sigma)))
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
    log->error("input not specified!");
    return -1;
  }

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  log->info("loading index from {}...", in);

  skim::skimdb db;
  auto res = db.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  auto [k, s, t] = db.parameters();
  log->info("index loaded, [k={}, s={}, t={}]", k, s, t);

  log->info("building spir server state...");

  auto setup = skim::spir::make_server(std::move(db), logp, logq, n, sigma);

  if (!setup) {
    log->error("could not setup server state: {}", setup.error());
    return -1;
  }

  auto server_state = setup.value();


  log->info("creating client...");

  skim::spir::spir_client_state client_state{server_state.skim_parameters(), server_state.skim_metadata(),
                                             server_state.spir_parameters(), server_state.hint_c()};

  log->info("client ready for queries...");

  std::string q{};

  while (std::cin >> q && std::cin && !std::cin.eof()) {
    auto query = client_state.prepare_query(q);

    if (!query) {
      log->warn("could not create query: {}", query.error());
      continue;
    }

    auto query_state = query.value();

    log->info("submitting query...");

    auto answer = server_state.answer(query_state.qu_vec);

    if (!answer) {
      log->warn("could not get answer: {}", answer.error());
      continue;
    }

    auto answer_vec = answer.value();

    log->info("recovering result...");

    auto recover_mat = client_state.recover(answer_vec, query_state);

    if (!recover_mat) {
      log->warn("could not recover result: {}", recover_mat.error());
      continue;
    }

    auto res_mat = recover_mat.value();

    if (verbose) {
      log->info("query results:");

      for (auto label : client_state.result(res_mat)) {
        log->info("  {}", label);
      }
    } else {
      log->info("got {} labels", std::ranges::distance(client_state.result(res_mat)));
    }
  }

  std::cout << "\n";

  log->info("done!");

  return 0;
}
