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
  unsigned int logp = 16;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input database file", cxxopts::value<std::string>(in))
      ("p,logp", "log of text modulus p", cxxopts::value<unsigned int>(logp)->default_value(std::to_string(logp)))
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
  auto log = spdlog::stdout_color_mt("spirdb-precompute-size");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  if (logp < 8) {
    log->error("logp must be at least 8");
    return -1;
  }

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

  auto data = std::move(db).explode().data;
  auto kmer_count = data.size();

  std::uint64_t max_rle = 0;

  for (const auto& entry : data) {
    max_rle = std::max(max_rle, entry.length());
  }

  log->info("kmer count: {}, max RLE length: {}", kmer_count, max_rle);
  log->info("calculating minimum N for logp = {}...", logp);

  std::uint64_t bytes_per_rle = 2 * max_rle;
  std::uint64_t bytes_per_block = logp / 8;
  std::uint64_t blocks_per_rle = bytes_per_rle / bytes_per_block + ((bytes_per_rle % bytes_per_block) ? 1 : 0);

  log->info("blocks required per RLE: {}", blocks_per_rle);

  std::uint64_t min_blocks = kmer_count * blocks_per_rle;
  double min_side = std::ceil(std::sqrt(static_cast<double>(min_blocks)));
  std::uint64_t rles_per_side = static_cast<std::uint64_t>(std::ceil(min_side / static_cast<double>(blocks_per_rle)));
  std::uint64_t sqrt_N = rles_per_side * blocks_per_rle;

  log->info("sqrt(N) = {}, N = {}", sqrt_N, sqrt_N * sqrt_N);

  return 0;
}
