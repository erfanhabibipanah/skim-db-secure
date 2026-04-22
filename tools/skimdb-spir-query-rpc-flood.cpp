#include <iostream>
#include <string>
#include <thread>

#include <cxxopts.hpp>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/spir/net/gRPC/spirdb_client.h>


auto mlog = spdlog::stdout_color_mt("skimdb-spir-query-rpc-flood");


void run_query(skim::spir::rpc::SpirDBClient& client, unsigned int l) {
  skim::skimdb_parameters param = client.skim_parameters().value();

  mlog->info("running thread {} with l={}...", std::this_thread::get_id(), l);

  std::mt19937 rng(std::random_device{}());
  skim::kmer_distribution dist{param.k};

  std::vector<std::string> res;
  res.reserve(32);

  auto start = std::chrono::high_resolution_clock::now();

  for (unsigned int i = 0; i < l; ++i) {
    auto kmer = dist(rng);
    std::ranges::copy(client.query(kmer), std::back_inserter(res));
    res.clear();
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;

  mlog->info("thread {} done, throughput: {:.2f}", std::this_thread::get_id(), static_cast<double>(l) / elapsed.count());
}

auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  std::string cache_dir = "";
  unsigned int nt = 1;
  unsigned int l = 100000;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("c,cache-dir", "directory for client cached data", cxxopts::value<std::string>(cache_dir))
      ("t,threads", "number of query threads", cxxopts::value<unsigned int>(nt)->default_value(std::to_string(nt)))
      ("l", "sample size per thread", cxxopts::value<unsigned int>(l)->default_value(std::to_string(l)))
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
  skim::g_log = spdlog::stdout_color_mt("skimdb");
  skim::g_log->set_level(spdlog::level::warn);

  if (cache_dir.empty()) {
    mlog->info("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::spir::g_spir_config.client_hint_c_dir = cache_dir;
  skim::spir::g_spir_config.client_metadata_dir = cache_dir;

  mlog->info("connecting to {}...", addr);

  skim::spir::rpc::SpirDBClient client{addr};

  auto res = client.setup();

  if (!res) {
    mlog->error("rpc setup failed: {}", res.error());
    return -1;
  }

  {
    std::vector<std::jthread> threads(nt);

    auto start = std::chrono::high_resolution_clock::now();

    for (auto& t : threads) {
      t = std::jthread(run_query, std::ref(client), l);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    mlog->info("total throughput: {:.2f}", static_cast<double>(nt * l) / elapsed.count());
  }

  mlog->info("done!");

  return 0;
}
