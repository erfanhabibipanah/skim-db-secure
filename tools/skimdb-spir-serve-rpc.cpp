#include <filesystem>
#include <iostream>
#include <string>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/spir/skimdb_spir.h>
#include <skimdb/spir/net/gRPC/spirdb_service.h>

namespace fs = std::filesystem;


auto main(int argc, char* argv[]) -> int {
  std::string in{};
  std::string addr{"0.0.0.0:50051"};
  unsigned int logp = 16;
  unsigned int logq = 64;
  std::size_t n = 1000;
  double sigma = 6.4;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "database to serve", cxxopts::value<std::string>(in))
      ("a,addr", "serve on network:port", cxxopts::value<std::string>(addr)->default_value(addr))
      ("p,logp", "log of text modulus p", cxxopts::value<unsigned int>(logp)->default_value(std::to_string(logp)))
      ("q,logq", "log of cypher modulus q", cxxopts::value<unsigned int>(logq)->default_value(std::to_string(logq)))
      ("n", "secret size", cxxopts::value<std::size_t>(n)->default_value(std::to_string(n)))
      ("s,sigma", "variance of error distribution", cxxopts::value<double>(sigma)->default_value(std::to_string(sigma)))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-serve-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  if (in.empty()) {
    log->error("input database not specified!");
    return -1;
  }

  fs::path dir{in};

  if (!fs::exists(dir)) {
    log->error("path {} does not exist!", dir.string());
    return -1;
  }

  if (logp < 8) {
    log->error("logp must be at least 8");
    return -1;
  }

  if (logq < logp) {
    log->error("logq must be at least logp");
    return -1;
  }

  log->info("loading index from {}...", in);

  skim::skimdb db;
  auto res = db.load(dir);

  if (!res) {
    log->error("could not load {}, error: {}!", in, res.error());
    return -1;
  }

  auto [k, s, t] = db.parameters();

  log->info("index loaded, [k={}, s={}, t={}]", k, s, t);

  auto server_state = skim::spir::make_server(std::move(db), logp, logq, n, sigma);
  if (!server_state) {
    log->error("could not setup server state: {}", server_state.error());
    return -1;
  }

  skim::spir::rpc::SpirDBService service(std::move(*server_state));
  grpc::ServerBuilder builder;

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

  return 0;
}
