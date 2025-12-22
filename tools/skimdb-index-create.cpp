/***
 *  $Id$
 **
 *  File: skimdb-index-create.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2025 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_builder.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  std::string out = "";

  int k = 15;
  int s = 9;
  int t = 2;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input file or directory FASTA format", cxxopts::value<std::string>(in))
      ("o,output", "database file name", cxxopts::value<std::string>(out))
      ("k", "k-mer size", cxxopts::value<int>(k)->default_value(std::to_string(k)))
      ("s", "syncmer s size", cxxopts::value<int>(s)->default_value(std::to_string(s)))
      ("t", "syncmer t parameter", cxxopts::value<int>(t)->default_value(std::to_string(t)))
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
  auto log = spdlog::stdout_color_mt("skimdb-index-create");
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

  if (out.empty()) {
    log->error("output not specified!");
    return -1;
  }

  if ((k < 10) || (k > skim::g_kmer_limit)) {
    log->error("incorrect k, must be 10 < k <= {}", skim::g_kmer_limit);
    return -1;
  }

  if (s < 3) {
    log->error("incorrect s, must be s > 2");
    return -1;
  }

  if (t < 0) {
    log->error("incorrect t, must be t >= 0");
    return -1;
  }

  log->info("indexing {}...", in);

  auto db = skim::builder::build_sequence_index(dir, k, s, t);

  log->info("index ready!");
  log->info("saving index to {}...", out);

  auto res = db.save(out);

  if (!res) {
    log->error("could not save {}, error: {}!", out, res.error());
    return -1;
  }

  log->info("done!");

  return 0;
}
