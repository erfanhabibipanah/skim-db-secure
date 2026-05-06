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
#include <fmtextra/fmt_extra.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_builder.h>
#include <skimdb/skimdb_version.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  std::string out = "";
  std::string lbl = "";

  std::uint64_t k = 15;
  std::uint64_t s = 9;
  std::uint64_t t = 2;

  std::string S = "minmax";
  std::size_t w = 1024;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input file or directory, FASTA format", cxxopts::value<std::string>(in))
      ("o,output", "database file name", cxxopts::value<std::string>(out))
      ("l,labels", "label mapping file", cxxopts::value<std::string>(lbl))
      ("k", "k-mer size", cxxopts::value<std::uint64_t>(k)->default_value(std::to_string(k)))
      ("s", "syncmer s size", cxxopts::value<std::uint64_t>(s)->default_value(std::to_string(s)))
      ("t", "syncmer t parameter", cxxopts::value<std::uint64_t>(t)->default_value(std::to_string(t)))
      ("S,solver", "RLE optimization solver {none|tsp|minmax}", cxxopts::value<std::string>(S)->default_value(S))
      ("w,window", "RLE optimization window size", cxxopts::value<std::size_t>(w)->default_value(std::to_string(w)))
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

  if ((k < 10) || (k > skim::g_kmer_limit)) {
    log->error("incorrect k, must be 10 < k <= {}", skim::g_kmer_limit);
    return -1;
  }

  skim::skimdb_rle_ordering solver;

  try {
    solver = skim::parse_rle_ordering(S);
  } catch (...) {
    log->error("incorrect solver, must be none|tsp|minmax");
    return -1;
  }

  log->info("indexing {}...", in);

  skim::skimdb db;

  if (lbl.empty()) {
    db = skim::builder::build_dir_index(dir, k, s, t, solver, w);
  } else {
    db = skim::builder::build_file_index(dir, lbl, k, s, t, solver, w);
  }

    log->info("index ready!");
    log->info("saving index to {}...", out);

    auto res = db.save(out);

    if (!res) {
      log->error("could not save {}, error: {}!", out, res.error());
      return -1;
    } else {
      log->info("index saved, size: {}B ({})", res.value(), as_fsize{res.value()});
    }

    log->info("done!");

    return 0;
  }
