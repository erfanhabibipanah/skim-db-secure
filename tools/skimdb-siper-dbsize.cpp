#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/fmt_extra.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_version.h>
#include <skimdb/siper/skimdb_siper.h>
#include <skimdb/siper/skimdb_siper_util.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in = "";
  unsigned int block_size = 1;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input database file", cxxopts::value<std::string>(in))
      ("b,block_size", "number of runs per block", cxxopts::value<unsigned int>(block_size)->default_value(std::to_string(block_size)))
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
  auto log = spdlog::stdout_color_mt("skimdb-siper-dbsize");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  if (block_size == 0) {
    log->error("block size must be at least 1");
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

  log->info("skimdb contains {} kmers, computing sqrt N...", kmer_count);

  std::vector<std::uint16_t> rle_lengths(kmer_count, 0);
  std::size_t max_len = 0;

#pragma omp parallel for schedule(static) reduction(max : max_len)
  for (std::size_t i = 0; i < kmer_count; ++i) {
    std::size_t rle_len = (data[i].length() + block_size - 1) / block_size;
    max_len = std::max(max_len, rle_len);
    rle_lengths[i] = static_cast<std::uint16_t>(rle_len);
  }

  if (max_len > (1 << skim::siper::index::g_len_bits) - 1) {
    log->error(
        "longest RLE {} exceeds maximum supported length {}", max_len, (1 << skim::siper::index::g_len_bits) - 1);
    return -1;
  }

  std::uint64_t run_sum = 0;
#pragma omp parallel for reduction(+ : run_sum)
  for (std::size_t i = 0; i < kmer_count; ++i) {
    run_sum += rle_lengths[i];
  }

  auto sqrt_N = skim::siper::min_sqrt_N(run_sum, block_size);

  log->info("skimdb contains {} total runs, requires matrix with sqrt(N) = {} for block size {}",
            kmer_count,
            run_sum,
            sqrt_N,
            block_size);

  return 0;
}
