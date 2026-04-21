#include <cstdlib>
#include <iostream>
#include <string>

#include <cxxopts.hpp>
#include <fmtextra/prompted_input.h>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <skimdb/spir/net/gRPC/spirdb_client.h>


auto main(int argc, char* argv[]) -> int {
  std::string addr{"127.0.0.1:50051"};
  std::string client_metadata_dir = "";
  bool verbose = false;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("s,address", "server to connect to", cxxopts::value<std::string>(addr)->default_value(addr))
      ("m,meta_dir", "directory for client metadata", cxxopts::value<std::string>(client_metadata_dir))
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
  auto log = spdlog::stdout_color_mt("skimdb-spir-query-rpc");
  skim::g_log = spdlog::stdout_color_mt("skimdb");

  if (client_metadata_dir.empty()) {
    log->info("client metadata directory not specified! using local directory as default...");
    client_metadata_dir = ".";
  }

  log->info("connecting to {}...", addr);

  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(-1);
  args.SetMaxSendMessageSize(-1);

  auto channel = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);

  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
    log->error("unable to connect to {}!", addr);
    return -1;
  }

  skim::spir::rpc::SpirDBClient client{channel};
  log->info("connection established, preparing for queries...");

  auto success = client.setup(client_metadata_dir);
  if (!success) {
    log->error("rpc setup failed: {}", success.error());
    return -1;
  }

  log->info("ready for queries...");

  prompted_input prompt;
  std::string q{};

  while (prompt.getline(q)) {
    if (!prompt.interactive()) {
      log->info("running query {}", q);
    }

    if (verbose) {
      for (const auto &l : client.query(q)) {
        log->info("  {}", l);
      }
    } else {
      log->info("got {} label(s)", std::ranges::distance(client.query(q)));
    }
  }

  log->info("done!");

  return 0;
}
