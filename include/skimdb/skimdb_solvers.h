/***
 *  $Id$
 **
 *  File: skimdb_solvers.h
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2025 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#ifndef SKIMDB_SOLVERS_H
#define SKIMDB_SOLVERS_H

#include <cstdint>
#include <ranges>
#include <vector>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_logger.h"


namespace skim {
namespace solver {

inline void greedy_order_bitmaps(std::vector<bitmap_t>& bitmaps, std::vector<std::string>& labels) {
  LogFun lf{"greedy_order_bitmaps"};

  std::vector<std::size_t> sizes(bitmaps.size());
  auto bs_zip = std::views::zip(bitmaps, sizes);

  std::for_each(bs_zip.begin(), bs_zip.end(), [&](auto&& bs) {
    auto& [bitmap, size] = bs;
    size = bitmap.cardinality();
  });

  auto bls_zip = std::views::zip(bitmaps, labels, sizes);
  std::ranges::sort(bls_zip, std::ranges::greater{}, [](const auto& bls) { return std::get<2>(bls); });

  // selected somewhat arbitrarily
  constexpr int w = 16;

  for (std::size_t i = 0, end = bitmaps.size() - w - 1; i < end; ++i) {
    auto& B = bitmaps[i];

    // this could be better expressed with ranges, but would be slower :(
    std::uint64_t curr_dist{0};
    std::size_t curr_pos{0};

    for (std::size_t j = i + 1; j < i + w; ++j) {
      auto dist = B.and_cardinality(bitmaps[j]);
      if (curr_dist < dist) {
        curr_dist = dist;
        curr_pos = j;
      }
    }

    bitmaps[i + 1].swap(bitmaps[curr_pos]);
    labels[i + 1].swap(labels[curr_pos]);
  }
}

} // namespace solver
} // namespace skim

#endif // SKIMDB_SOLVERS_H
