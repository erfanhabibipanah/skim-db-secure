#include <iostream>
#include <string>
#include <thread>

#include <cxxopts.hpp>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/skimdb_version.h>
#include <skimdb/siper/net/gRPC/siperdb_batched_client.h>


auto mlog = spdlog::stdout_color_mt("skimdb-siper-batched-rpc-flood");


void run_query(skim::siper::rpc::BatchedSiperDBClient& client, unsigned int l) {
  skim::skimdb_parameters param = client.skim_parameters().value();

  mlog->info("running thread {} with l={}...", std::this_thread::get_id(), l);

  std::mt19937 rng(std::random_device{}());
  skim::kmer_distribution dist{param.k};

  std::vector<std::shared_future<skim::siper::rpc::shared_result_type>> futures;
  futures.reserve(l);

  std::vector<std::string> res;
  res.reserve(32);

  auto start = std::chrono::high_resolution_clock::now();

  for (unsigned int i = 0; i < l; ++i) {
    auto kmer = dist(rng);
    futures.push_back(client.request(kmer));
  }

  for (auto& f : futures) {
    std::ranges::copy(client.interpret(f), std::back_inserter(res));
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
  int bt = std::thread::hardware_concurrency() * 2;
  double submit_threshold = 0.75;
  unsigned int timeout = 100;
  unsigned int l = 100000;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("a,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("c,cache-dir", "directory for client cached data", cxxopts::value<std::string>(cache_dir))
      ("T,threads", "number of query threads", cxxopts::value<unsigned int>(nt)->default_value(std::to_string(nt)))
      ("b,bthreads", "maximum number of batch threads to run concurrently", cxxopts::value<int>(bt)->default_value(std::to_string(bt)))
      ("W,wait", "batch timeout in milliseconds", cxxopts::value<unsigned int>(timeout)->default_value(std::to_string(timeout)))
      ("s,submit", "batch submit threshold (%)", cxxopts::value<double>(submit_threshold)->default_value(std::format("{:.2f}", submit_threshold)))
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

  mlog->info("SKiMdb ver. {}", skim::version);

  if (cache_dir.empty()) {
    mlog->debug("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.siper_client_hint_c_dir = cache_dir;
  skim::g_skim_config.siper_client_metadata_dir = cache_dir;

  mlog->info("connecting to {}...", addr);

  skim::siper::rpc::BatchedSiperDBClient client{bt, submit_threshold, timeout, addr};

  auto res = client.setup();

  if (!res) {
    mlog->error("connection failed: {}", res.error());
    return -1;
  }

  mlog->info("connection established!");

  auto start = std::chrono::high_resolution_clock::now();

  {
    std::vector<std::jthread> threads(nt);

    for (auto& t : threads) {
      t = std::jthread(run_query, std::ref(client), l);
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;

  mlog->info("total throughput: {:.2f}", static_cast<double>(nt * l) / elapsed.count());

  mlog->info("done!");

  return 0;
}
