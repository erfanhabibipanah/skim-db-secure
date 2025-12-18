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

#include <ranges>
#include <vector>

#include "detail/skimdb_definitions.h"


namespace skim {
namespace solver {

auto order_bitmaps(std::vector<bitmap_t>& bitmaps, std::vector<std::string>& labels) -> void {
  std::vector<std::size_t> sizes(bitmaps.size());
  auto bs_zip = std::views::zip(bitmaps, sizes);

  std::for_each(bs_zip.begin(), bs_zip.end(), [&](auto&& bs) {
    auto& [bitmap, size] = bs;
    size = bitmap.cardinality();
  });

  auto bls_zip = std::views::zip(bitmaps, labels, sizes);
  std::ranges::sort(bls_zip, std::ranges::greater{}, [](const auto& bls) { return std::get<2>(bls); });
}

} // namespace solver
} // namespace skim

#endif // SKIMDB_SOLVERS_H
