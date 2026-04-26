/***
 *  $Id$
 **
 *  File: skimdb-index-query.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

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


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in{};

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input database file", cxxopts::value<std::string>(in))
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
  auto log = spdlog::stdout_color_mt("skimdb-index-query");
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

  log->info("loading index from {}...", in);

  skim::skimdb db;
  auto res = db.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  auto [k, s, t] = db.parameters();

  log->info("index loaded, (k={}, s={}, t={})", k, s, t);
  log->info("ready for queries...");

  prompted_input prompt;
  std::string q = "";

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("running query {}", q);
    }

    for (const auto& l : db.query(q)) {
      std::cout << "  " << l << std::endl;
    }
  }

  log->info("done!");

  return 0;
}
