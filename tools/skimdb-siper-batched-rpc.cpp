#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/siper/net/gRPC/siperdb_batched_client.h>
#include <skimdb/siper/skimdb_siper.h>
#include <skimdb/skimdb_version.h>

#include <tbb/concurrent_queue.h>


auto mlog = spdlog::stdout_color_mt("skimdb-siper-batched-rpc");
std::atomic_flag run{true};

template <typename T>
using future_queue = tbb::concurrent_bounded_queue<T>;


struct query_item {
  std::string kmer;
  std::shared_future<skim::siper::rpc::shared_result_type> future;
};


void output_thread(future_queue<query_item>& fq,
                   skim::siper::rpc::BatchedSiperDBClient& client,
                   bool verbose) {
  query_item item;

  while (true) {
    if (!fq.empty()) {
      fq.pop(item);

      try {
        if (verbose) {
          mlog->info("query {} results:", item.kmer);
          for (auto label : client.interpret(item.future)) {
            mlog->info("  {}", label);
          }
        } else {
          mlog->info("got {} label(s)", std::ranges::distance(client.interpret(item.future)));
        }
      } catch (const std::exception& e) {
        mlog->error("query failed: {}", e.what());
      }
    } else {
      if (run.test(std::memory_order_acquire) == false) {
        break;
      }
    }
  }

  mlog->info("output thread done!");
}


auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  std::string cache_dir = "";
  int bt = std::thread::hardware_concurrency() * 2;
  unsigned int timeout = 1000;
  double submit_threshold = 0.5;
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("a,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("c,cache-dir", "directory for client cached data", cxxopts::value<std::string>(cache_dir))
      ("b,bthreads", "maximum number of batch threads to run concurrently", cxxopts::value<int>(bt)->default_value(std::to_string(bt)))
      ("t,timeout", "batch timeout in milliseconds", cxxopts::value<unsigned int>(timeout)->default_value(std::to_string(timeout)))
      ("s,submit", "batch submit threshold (%)", cxxopts::value<double>(submit_threshold)->default_value(std::format("{:.2f}", submit_threshold)))
      ("v,verbose", "print recovered labels", cxxopts::value<bool>(verbose)->default_value(std::to_string(verbose)))
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

  prompted_input prompt;
  std::string q{};

  if (prompt.interactive()) {
    mlog->error("interactive mode not supported, use batch mode!");
    return -1;
  }

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

  future_queue<query_item> kmer_queue;
  std::jthread consumer{output_thread, std::ref(kmer_queue), std::ref(client), verbose};

  mlog->info("ready for queries...");

  while (prompt.getline(q)) {
    auto ft = client.request(q);
    kmer_queue.push(query_item{.kmer = q, .future = std::move(ft)});
  }

  run.clear(std::memory_order_release);

  mlog->info("queries submitted, waiting for results...");
  consumer.join();

  mlog->info("done!");

  return 0;
}
