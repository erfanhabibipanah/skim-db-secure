#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/fmt_extra.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/skimdb.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in{};
  
  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input skim database file", cxxopts::value<std::string>(in))
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

  log->info("index loaded successfully, computing statistics...");

  auto data = std::move(db).explode().data;
  auto kmer_count = data.size();

  std::vector<std::size_t> rle_lengths; 
  rle_lengths.reserve(kmer_count);

  std::transform(data.begin(), data.end(), std::back_inserter(rle_lengths), [](const auto& encoding) {
    return encoding.length();
  });

  auto [min_it, max_it] = std::minmax_element(rle_lengths.begin(), rle_lengths.end());
  double mean = std::accumulate(rle_lengths.begin(), rle_lengths.end(), 0.0) / kmer_count;

  auto q1_it = rle_lengths.begin() + kmer_count / 4;
  auto med_it = rle_lengths.begin() + kmer_count / 2;
  auto q3_it = rle_lengths.begin() + (3 * kmer_count) / 4;

  std::nth_element(rle_lengths.begin(), q1_it, rle_lengths.end());
  std::size_t q1 = *q1_it;

  std::nth_element(rle_lengths.begin(), med_it, rle_lengths.end());
  std::size_t median = *med_it;

  std::nth_element(rle_lengths.begin(), q3_it, rle_lengths.end());
  std::size_t q3 = *q3_it;

  log->info("RLE length statistics: mean {}, min {}, Q1 {}, median {}, Q3 {}, max {}", mean, *min_it, q1, median, q3, *max_it);

  return 0;
}
