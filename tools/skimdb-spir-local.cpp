
#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmt_extra.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/spir/skimdb_spir.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "path to database file", cxxopts::value<std::string>(in))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-local");
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

  log->info("building server state...");

  auto setup = skim::spir::make_server(std::move(db), 16, 64, 1000, 6.4);

  if (!setup) {
    log->error("could not setup server state: {}", setup.error());
    return -1;
  }

  auto server_state = setup.value();

  log->info("creating client...");

  skim::spir::spir_client_state client_state{
    server_state.get_hint_c(),
    server_state.get_config(),
    server_state.get_metadata(),
    server_state.get_parameters()
  };

  log->info("creating query for kmer AACGGTCCTAAGGTA...");

  auto query = client_state.prepare_query("AACGGTCCTAAGGTA");

  if (!query) {
    log->error("could not create query: {}", query.error());
    return -1;
  }
  auto query_state = query.value();

  log->info("submitting query...");

  auto answer = server_state.answer(query_state.enc_vec);

  if (!answer) {
    log->error("could not get answer: {}", answer.error());
    return -1;
  }

  auto answer_vec = answer.value();

  log->info("recovering result...");

  auto recover_mat = client_state.recover(answer_vec, query_state);

  if (!recover_mat) {
    log->error("could not recover result: {}", recover_mat.error());
    return -1;
  }

  auto res_mat = recover_mat.value();

  log->info("query results:");

  for (auto label : client_state.result(res_mat)) {
    log->info("  {}", label);
  }

  return 0;
}
