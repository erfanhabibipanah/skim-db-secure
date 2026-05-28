#include <iostream>
#include <mutex>
#include <queue>
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


template <typename T>
class future_queue {
public:
  void push(T value) {
    {
      std::lock_guard lock(m_);
      q_.push(std::move(value));
    }
    cv_.notify_one();
  }

  auto pop(T& out) -> bool {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return done_ || !q_.empty(); });

    if (q_.empty()) {
      return false;
    }

    out = std::move(q_.front());
    q_.pop();

    return true;
  }

  void done() {
    {
      std::lock_guard lock(m_);
      done_ = true;
    }
    cv_.notify_all();
  }

private:
  std::queue<T> q_;
  std::mutex m_;
  std::condition_variable cv_;
  bool done_ = false;
};


struct query_item {
  std::string kmer;
  std::shared_future<skim::siper::rpc::shared_result_type> future;
};


void output_thread(future_queue<query_item>& fq,
                   skim::siper::rpc::BatchedSiperDBClient& client,
                   std::shared_ptr<spdlog::logger> log,
                   bool verbose) {
  query_item item;

  while (fq.pop(item)) {
    try {
      if (verbose) {
        log->info("query results:");
        for (auto label : client.interpret(item.future)) {
          log->info("  {}", label);
        }
      } else {
        log->info("got {} label(s)", std::ranges::distance(client.interpret(item.future)));
      }
    } catch (const std::exception& e) {
      log->error("query failed: {}", e.what());
    }
  }

  log->info("consumer thread exiting");
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
      ("b,b-threads", "maximum number of batch threads to run concurrently", cxxopts::value<int>(bt)->default_value(std::to_string(bt)))
      ("t,timeout", "batch timeout in milliseconds", cxxopts::value<unsigned int>(timeout)->default_value(std::to_string(timeout)))
      ("s,submit", "batch submit threshold (%)", cxxopts::value<double>(submit_threshold)->default_value(std::to_string(submit_threshold)))
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
  auto log = spdlog::stdout_color_mt("skimdb-siper-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  if (cache_dir.empty()) {
    log->debug("client cache directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.siper_client_hint_c_dir = cache_dir;
  skim::g_skim_config.siper_client_metadata_dir = cache_dir;

  log->info("connecting to {}...", addr);

  skim::siper::rpc::BatchedSiperDBClient client{bt, submit_threshold, timeout, addr};

  auto res = client.setup();

  if (!res) {
    log->error("rpc setup failed: {}", res.error());
    return -1;
  }

  future_queue<query_item> kmer_queue;
  std::jthread consumer{output_thread, std::ref(kmer_queue), std::ref(client), log, verbose};

  log->info("ready for queries...");

  prompted_input prompt;
  std::string q{};

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("submitting query {}", q);
    }

    auto ft = client.request(q);
    kmer_queue.push(query_item{q, std::move(ft)});
  }

  log->info("all queries submitted, waiting for results...");

  kmer_queue.done();
  consumer.join();

  log->info("done!");

  return 0;
}
