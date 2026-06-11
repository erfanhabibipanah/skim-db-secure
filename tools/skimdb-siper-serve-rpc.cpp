#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <skimdb/skimdb_version.h>
#include <skimdb/siper/net/gRPC/siperdb_service.h>
#include <skimdb/siper/skimdb_siper.h>


namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-siper-serve-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  log->info("SKiMdb ver. {}", skim::version);

  std::string in{};
  std::string addr{"0.0.0.0:50051"};
  std::string cache_dir = "";
  unsigned int nthreads = 8;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "siper database to serve", cxxopts::value<std::string>(in))
      ("a,address", "address [network:port] to serve on", cxxopts::value<std::string>(addr)->default_value(addr))
      ("c,cache-dir", "server store directory", cxxopts::value<std::string>(cache_dir))
      ("T,threads", "number of server threads", cxxopts::value<unsigned int>(nthreads)->default_value(std::to_string(nthreads)))
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

  if (cache_dir.empty()) {
    log->debug("server store directory not specified! using local directory...");
    cache_dir = ".";
  }

  skim::g_skim_config.siper_server_store_dir = cache_dir;

  if (in.empty()) {
    log->error("input database not specified!");
    return -1;
  }

  log->info("loading siperdb from {}...", in);

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  auto server_state = skim::siper::load_server(dir);

  if (!server_state) {
    log->error("could not load {}, error: {}!", in, server_state.error());
    return -1;
  }

  skim::siper::rpc::SiperDBService service(std::move(*server_state));

  grpc::ServerBuilder builder;
  grpc::ResourceQuota quota;

  quota.SetMaxThreads(nthreads);

  builder.SetResourceQuota(quota);
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.SetMaxReceiveMessageSize(-1);
  builder.SetMaxSendMessageSize(-1);

  auto server = builder.BuildAndStart();

  if (!server) {
    log->error("could not create service on {}!", addr);
    return -1;
  }

  log->info("listening for queries on {}...", addr);

  server->Wait();

  log->info("done!");

}
