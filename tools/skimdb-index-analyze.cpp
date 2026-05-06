#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/fmt_extra.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/skimdb.h>
#include <skimdb/skimdb_version.h>


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
  auto log = spdlog::stdout_color_mt("skimdb-index-analyze");
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

  log->info("index loaded successfully, computing statistics...");

  auto data = std::move(db).explode().data;

  struct rle_info {
    std::size_t length;
    std::size_t idx;
  };

  std::vector<rle_info> info;
  info.reserve(data.size());

  for (std::size_t i = 0; i < data.size(); ++i) {
    info.emplace_back(rle_info{.length = data[i].length(), .idx = i});
  }

  std::sort(info.begin(), info.end(), [](const auto& a, const auto& b) { return a.length < b.length; });

  std::size_t total_length =
      std::accumulate(info.begin(), info.end(), 0ULL, [](std::size_t sum, const auto& p) { return sum + p.length; });
  double mean = static_cast<double>(total_length) / info.size();

  log->info("RLE length statistics:");
  log->info("  mean={:.3f}, min={}, Q1={}, median={}, Q3={}, max={}",
            mean,
            info.front().length,
            info[info.size() / 4].length,
            info[info.size() / 2].length,
            info[3 * info.size() / 4].length,
            info.back().length);

  const std::size_t n = std::min<std::size_t>(10, info.size());

  log->info("largest RLE lengths:");

  for (std::size_t i = info.size() - n; i < info.size(); ++i) {
    std::size_t n_compressed = 0;
    std::size_t n_uncompressed = 0;

    std::size_t ones = 0;
    std::size_t zeros = 0;

    for (auto block : data[info[i].idx].span()) {
      switch (skim::detail::get_block_encoding(block)) {
      case skim::detail::g_encoding_uncompressed: {
        n_uncompressed++;
        std::size_t value = block & skim::detail::g_literal_mask;
        auto set = std::popcount(value);
        ones += set;
        zeros += 15 - set;
        break;
      }
      case skim::detail::g_encoding_one_run: {
        n_compressed++;
        ones += block & skim::detail::g_count_mask;
        break;
      }
      case skim::detail::g_encoding_zero_run: {
        n_compressed++;
        zeros += block & skim::detail::g_count_mask;
        break;
      }
      }
    }

    double entropy = 0.0;

    if (ones > 0) {
      double p1 = static_cast<double>(ones) / (ones + zeros);
      entropy -= p1 * std::log2(p1);
    }

    if (zeros > 0) {
      double p0 = static_cast<double>(zeros) / (ones + zeros);
      entropy -= p0 * std::log2(p0);
    }

    log->info("  {:02}: total={}, compressed={}, uncompressed={}, H={:.3f}",
              info.size() - i,
              info[i].length,
              n_compressed,
              n_uncompressed,
              entropy);
  }

  return 0;
}
