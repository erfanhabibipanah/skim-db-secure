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
  LogFun(std::string&& name) : name_(std::move(name)) {
    g_log->info("running {}...", name_);
    tp_ = steady_clock::now();
  }
  ~LogFun() {
    auto t = duration<double>(steady_clock::now() - tp_);
    g_log->info("{} done in {:.2f}s!", name_, t.count());
  }

private:
  std::chrono::steady_clock::time_point tp_;
  std::string name_;

};

}

#endif // SKIMDB_LOGGER_H
