#ifndef SKIMDB_BUILDER_H
#define SKIMDB_BUILDER_H

#include <algorithm>
#include <cstddef>
#include <execution>
#include <expected>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_logger.h"
#include "detail/skimdb_util.h"

#include "skimdb.h"


namespace skim::solver {

namespace detail {

inline auto max_distance(const std::unordered_map<kmer_binary_t, std::size_t>& S, bitmap_t R, const bitmap_t& M)
    -> std::size_t {

  R ^= M;

  std::size_t res = 0;

  for (auto r : R) {
    auto pos = S.find(r);
    std::size_t val = (pos != S.end()) ? (pos->second) + 1 : 1;
    if (val > res) {
      res = val;
    }
  }

  return res;
}

} // namespace detail

inline void sort_bitmaps(std::vector<bitmap_t>& bitmaps, std::vector<std::string>& labels) {
  LogFun lf{"sort_bitmaps(...)"};

  std::size_t n = bitmaps.size();

  // all this to parallelize sort
  std::vector<std::size_t> sizes(n);

  for (std::size_t i = 0; i < n; ++i) {
    sizes[i] = bitmaps[i].cardinality();
  }

  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), 0);

  std::sort(std::execution::par, order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return sizes[a] > sizes[b];
  });

  std::vector<bitmap_t> sbitmaps(n);
  std::vector<std::string> slabels(n);

  for (std::size_t i = 0; i < n; ++i) {
    sbitmaps[i] = std::move(bitmaps[order[i]]);
    slabels[i] = std::move(labels[order[i]]);
  }

  bitmaps = std::move(sbitmaps);
  labels = std::move(slabels);
}

inline void
greedy_tsp_order_bitmaps(std::vector<bitmap_t>& bitmaps, std::vector<std::string>& labels, std::size_t win_size = 0) {
  LogFun lf{"greedy_tsp_order_bitmaps(...)"};

  std::size_t n = bitmaps.size();

  if (n < 3) {
    return;
  }

  if (win_size == 0) {
    win_size = n - 1;
  }

  sort_bitmaps(bitmaps, labels);

  auto w = std::min(win_size, n - 1);

  g_log->info("reordering bitmaps, window size w={}...", w);

  std::vector<std::size_t> indices(w);

  for (std::size_t i = 0, end = n - 2; i < end; ++i) {
    const auto& B = bitmaps[i];

    // we can't use views because TBB complains
    indices.resize(std::min(w, n - i - 1));
    std::iota(indices.begin(), indices.end(), i + 1);

    g_log->trace("iteration {}, range {}..{}", i, indices.front(), indices.back());

    auto [best_dist, best_pos] = std::transform_reduce(
        std::execution::par,
        indices.begin(),
        indices.end(),
        std::pair{std::size_t{0}, i + 1}, // identity
        [](auto a, auto b) {              // reduction
          return a.first >= b.first ? a : b;
        },
        [&](std::size_t j) -> std::pair<std::size_t, std::size_t> { // transform
          return {(B & bitmaps[j]).cardinality(), j};
        });

    g_log->trace("iteration {}, swapping {} with {}", i, i + 1, best_pos);

    bitmaps[i + 1].swap(bitmaps[best_pos]);
    labels[i + 1].swap(labels[best_pos]);
  }
}

inline void greedy_minmax_order_bitmaps(std::vector<bitmap_t>& bitmaps,
                                        std::vector<std::string>& labels,
                                        std::size_t win_size = 0) {
  LogFun lf{"greedy_minmax_order_bitmaps(...)"};

  std::size_t n = bitmaps.size();

  if (n < 3) {
    return;
  }

  if (win_size == 0) {
    win_size = n - 1;
  }

  sort_bitmaps(bitmaps, labels);

  auto w = std::min(win_size, n - 1);

  g_log->info("reordering bitmaps, window size w={}...", w);

  std::unordered_map<kmer_binary_t, std::size_t> S;
  std::vector<std::size_t> indices(w);

  for (std::size_t i = 0, end = n - 2; i < end; ++i) {
    const auto& B = bitmaps[i];

    indices.resize(std::min(w, n - i - 1));
    std::iota(indices.begin(), indices.end(), i + 1);

    g_log->trace("iteration {}, range {}..{}", i, indices.front(), indices.back());

    auto [min_dst, min_pos] = std::transform_reduce(
        std::execution::par,
        indices.begin(),
        indices.end(),
        std::pair{std::numeric_limits<std::size_t>::max(), i},
        [](auto a, auto b) {
          return a.first <= b.first ? a : b; // min reduction
        },
        [&](std::size_t j) -> std::pair<std::size_t, std::size_t> {
          return {detail::max_distance(S, B, bitmaps[j]), j};
        });

    g_log->trace("iteration {}, swapping {} with {}", i, i + 1, min_pos);

    std::swap(bitmaps[i + 1], bitmaps[min_pos]);
    std::swap(labels[i + 1], labels[min_pos]);

    auto R = (bitmaps[i] ^ bitmaps[i + 1]);

    for (auto r : R) {
      ++S[r];
    }
  }
}

} // namespace skim::solver


namespace skim {

namespace fs = std::filesystem;

class builder final {
public:
  [[nodiscard]] static auto build_index(const std::vector<bitmap_t>& bitmaps,
                                        std::vector<std::string> labels,
                                        std::size_t k,
                                        std::size_t s,
                                        std::size_t t) -> skimdb {
    LogFun lf{"build_index(...)"};

    g_log->info("packing kmers into data with (k={}, s={}, t={})...", k, s, t);

    skimdb db;

    db.k_ = k;
    db.s_ = s;
    db.t_ = t;

    auto& index = db.index_;
    auto& data = db.data_;

    db.labels_ = std::move(labels);

    g_log->debug("extracting kmers for hashing...");

    // we first build k-mer hash
    // pre-allocation is arbitrary
    std::vector<kmer_binary_t> kmers;
    kmers.reserve(1024 * 1024);

    for (auto& bmp : bitmaps) {
      for (kmer_binary_t kmer : bmp) {
        if (!index.kmers.contains(kmer)) {
          index.kmers.add(kmer);
          kmers.push_back(kmer);
        }
      }
    }

    index.kmers.runOptimize();

    g_log->debug("building hash...");

    index.hash = bbh::bbhash<kmer_binary_t>{std::ranges::subrange(kmers.begin(), kmers.end())};
    kmers = {};

    // now we prepeare data
    g_log->info("found {} unique kmers", index.kmers.cardinality());
    g_log->info("kmers processing now, please be patient...");

    auto m = bitmaps.size();

    g_log->info("mapping kmers to {} bitmaps...", m);

    // right now memory is an issue so we use locking
    std::vector<std::mutex> data_mtx(index.kmers.cardinality());
    data.resize(index.kmers.cardinality());

    auto indices = std::views::iota(std::size_t{0}, m);

    std::for_each(std::execution::par, indices.begin(), indices.end(), [&](std::size_t i) {
      for (auto kmer : bitmaps[i]) {
        std::size_t idx = index.hash.find(kmer).value();
        std::lock_guard<std::mutex> lock(data_mtx[idx]);
        data[idx].push(i);
      }
    });

    g_log->info("kmers packing done!");
    g_log->info("compressing data with {} kmers...", data.size());

    std::for_each(std::execution::par, data.begin(), data.end(), [](detail::encoding& rec) { rec.attempt_compress(); });

    g_log->info("compression done!");

    return db;
  }


  [[nodiscard]] static auto build_file_index(const fs::path& dir,
                                             const std::vector<std::string>& files,
                                             std::vector<std::string> labels,
                                             std::size_t k,
                                             std::size_t s,
                                             std::size_t t,
                                             skimdb_rle_ordering order = skimdb_rle_ordering::none,
                                             std::size_t w = 0) -> skimdb {
    LogFun lf{"build_file_index(dir, files, ...)"};

    std::vector<bitmap_t> bitmaps(files.size());
    auto zipped = std::views::zip(files, bitmaps);

    std::for_each(std::execution::par, zipped.begin(), zipped.end(), [&](auto&& fb) {
      auto& [file, bitmap] = fb;
      bitmap = detail::populate_bitmap(dir, file, k, s, t);
    });

    if (order == skimdb_rle_ordering::tsp) {
      solver::greedy_tsp_order_bitmaps(bitmaps, labels, w);
    } else if (order == skimdb_rle_ordering::minmax) {
      solver::greedy_minmax_order_bitmaps(bitmaps, labels, w);
    }

    return build_index(bitmaps, std::move(labels), k, s, t);
  }


  [[nodiscard]] static auto build_file_index(const fs::path& dir,
                                             const fs::path& f2l,
                                             std::size_t k,
                                             std::size_t s,
                                             std::size_t t,
                                             skimdb_rle_ordering order = skimdb_rle_ordering::none,
                                             std::size_t w = 0) -> skimdb {
    LogFun lf{"build_file_index(dir, f2l, ...)"};
    auto [files, labels] = detail::load_f2l(f2l);
    return build_file_index(dir, files, std::move(labels), k, s, t, order, w);
  }


  template <std::ranges::input_range Range>
  [[nodiscard]] static auto build_range_index(Range&& range,
                                              std::size_t k,
                                              std::size_t s,
                                              std::size_t t,
                                              skimdb_rle_ordering order = skimdb_rle_ordering::none,
                                              std::size_t w = 0) -> skimdb {
    LogFun lf{"build_range_index(...)"};

    std::vector<bitmap_t> bitmaps;
    std::vector<std::string> labels;

    g_log->info("extracting kmers with (k={}, s={}, t={})...", k, s, t);

    std::size_t kmer_count = 0;
    kmer_binary_t kmax = 0;

    for (auto&& seq : range) {
      labels.emplace_back(std::move(std::get<0>(seq)));
      const std::string& read = std::get<1>(seq);
      bitmap_t bitmap;
      auto [count, last] = detail::update_bitmap(read, k, s, t, bitmap);
      kmer_count += count;
      kmax = std::max(kmax, last);
      bitmaps.emplace_back(std::move(bitmap));
    }

    g_log->info("{} kmers extracted from {} sequences", kmer_count, labels.size());
    g_log->info("largest kmer: {}", kmax);

    if (order == skimdb_rle_ordering::tsp) {
      solver::greedy_tsp_order_bitmaps(bitmaps, labels, w);
    } else if (order == skimdb_rle_ordering::minmax) {
      solver::greedy_minmax_order_bitmaps(bitmaps, labels, w);
    }

    return build_index(bitmaps, std::move(labels), k, s, t);
  }


  [[nodiscard]] static auto build_dir_index(const fs::path& dir,
                                            std::size_t k,
                                            std::size_t s,
                                            std::size_t t,
                                            skimdb_rle_ordering order = skimdb_rle_ordering::none,
                                            std::size_t w = 0) -> skimdb {
    LogFun lf{"build_dir_index(...)"};
    fastx::fastx_files_reader<fastx::fasta_buffered_reader> ffr{dir};
    return build_range_index(ffr.sequences(), k, s, t, order, w);
  }


  // merges indexes with a disjoint set of labels
  template <std::ranges::input_range Range>
  [[nodiscard]] static auto merge_disjoint_indexes(Range&& range, std::size_t k, std::size_t s, std::size_t t)
      -> std::expected<skimdb, std::string> {
    LogFun lf{"merge_disjoint_indexes(...)"};

    std::vector<bitmap_t> bitmaps;
    std::vector<std::string> labels;

    std::size_t offset = 0;

    for (const skimdb& db : range) {
      if (db.parameters() != skimdb_parameters{.k = k, .s = s, .t = t}) {
        return std::unexpected{"parameter mismatch"};
      }

      labels.insert(labels.end(), db.labels_.begin(), db.labels_.end());
      bitmaps.resize(labels.size());

      for (const auto& kmer : db.index_.kmers) {
        auto pos = db.index_.hash.find(kmer).value();
        for (auto l : db.m_traverse_kmer_(pos)) {
          bitmaps[l + offset].add(kmer);
        }
      }

      offset = labels.size();
    }

    return build_index(bitmaps, labels, k, s, t);
  }
};

} // namespace skim

#endif // SKIMDB_BUILDER_H
