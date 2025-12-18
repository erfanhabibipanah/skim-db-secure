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

#include <memory>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace skim {
inline auto g_log = std::make_shared<spdlog::logger>("null", std::make_shared<spdlog::sinks::null_sink_mt>());
}

#endif // SKIMDB_LOGGER_H
