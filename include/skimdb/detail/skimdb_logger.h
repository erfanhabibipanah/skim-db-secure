/***
 *  $Id$
 **
 *  File: skimdb_logger.h
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2025 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#ifndef SKIMDB_LOGGER_H
#define SKIMDB_LOGGER_H

#include <chrono>
#include <memory>
#include <string>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace skim {

using namespace std::chrono;


inline auto g_log = std::make_shared<spdlog::logger>("null", std::make_shared<spdlog::sinks::null_sink_mt>());

class LogFun {
public:
  explicit LogFun(std::string&& name, spdlog::level::level_enum level = spdlog::level::info)
      : name_{std::move(name)}, level_{level} {
    g_log->log(level_, "running {}...", name_);
    tp_ = steady_clock::now();
  }
  ~LogFun() {
    auto t = duration<double>(steady_clock::now() - tp_);
    g_log->log(level_, "{} done in {:.2f}s!", name_, t.count());
  }

private:
  std::chrono::steady_clock::time_point tp_;
  std::string name_{""};
  spdlog::level::level_enum level_{spdlog::level::info};
};

}

#endif // SKIMDB_LOGGER_H
