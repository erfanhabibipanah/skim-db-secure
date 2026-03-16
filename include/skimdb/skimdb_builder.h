#ifndef SKIMDB_BUILDER_H
#define SKIMDB_BUILDER_H

#include <algorithm>
#include <cstdint>
#include <execution>
#include <expected>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_encoding.h"
#include "detail/skimdb_logger.h"
#include "detail/skimdb_util.h"
#include "skimdb.h"
#include "skimdb_solvers.h"


namespace skim {

namespace fs = std::filesystem;

class builder final {
public:
  // labels become owned by the resulting skimdb index, hence move semantics
  // bitmaps are always post-processed so passing by const reference
  [[nodiscard]] static auto build_index(const std::vector<bitmap_t>& bitmaps, std::vector<std::string> labels,
                                        std::uint64_t k, std::uint64_t s, std::uint64_t t) -> skimdb {
    LogFun lf{"build_index(...)"};

    g_log->info("packing kmers into data with (k={}, s={}, t={})...", k, s, t);

    std::size_t total_kmers = detail::estimated_kmer_count(k, s, t);
    g_log->info("estimated {} total kmers", total_kmers);

    skimdb db;

    db.k_ = k;
    db.s_ = s;
    db.t_ = t;

    auto& index = db.index_;
    auto& data = db.data_;

    db.labels_ = std::move(labels);

    // our estimate is probably off hence we divide
    // this still should give good amortization
    // without overblowing memory
    data.reserve(total_kmers >> 2);

    std::size_t free_idx = 0;

    for (std::size_t i = 0, end = bitmaps.size(); i < end; i++) {
      auto& bitmap = bitmaps[i];

      for (std::uint32_t kmer : bitmap) {
        auto [it, inserted] = index.try_emplace(kmer, free_idx);
        if (inserted) {
          free_idx++;
        }

        std::size_t idx = it->second;

        if (idx >= data.size()) {
          data.resize(idx + 1);
        }
        data[idx].push(i);
      }
    }

    g_log->info("kmers packing done!");
    g_log->info("compressing data with {} kmers...", data.size());

    std::for_each(std::execution::par, data.begin(), data.end(), [](detail::encoding& rec) { rec.attempt_compress(); });

    g_log->info("compression done!");

    return db;
  }

  [[nodiscard]] static auto build_file_index(const fs::path& dir,
                                             const std::vector<std::string>& files,
                                             std::vector<std::string> labels,
                                             std::uint64_t k, std::uint64_t s, std::uint64_t t) -> skimdb {
    LogFun lf{"build_file_index(dir, files, ...)"};

    std::vector<bitmap_t> bitmaps(files.size());
    auto zipped = std::views::zip(files, bitmaps);

    std::for_each(std::execution::par, zipped.begin(), zipped.end(), [&](auto&& fb) {
      auto& [file, bitmap] = fb;
      bitmap = detail::populate_bitmap(dir, file, k, s, t);
    });

    return build_index(bitmaps, std::move(labels), k, s, t);
  }

  [[nodiscard]] static auto build_file_index(const fs::path& dir, const fs::path& f2l,
                                             std::uint64_t k, std::uint64_t s, std::uint64_t t) -> skimdb {
    LogFun lf{"build_file_index(dir, f2l, ...)"};
    auto [files, labels] = detail::load_f2l(f2l);
    return build_file_index(dir, files, std::move(labels), k, s, t);
  }

  template <std::ranges::input_range Range>
  [[nodiscard]] static auto build_range_index(Range&& range, std::uint64_t k, std::uint64_t s, std::uint64_t t)
      -> skimdb {
    LogFun lf{"build_range_index(...)"};

    std::vector<bitmap_t> bitmaps;
    std::vector<std::string> labels;

    g_log->info("extracting kmers with (k={}, s={}, t={})...", k, s, t);

    std::uint64_t kmer_count = 0;
    std::uint32_t d = 0;

    for (auto&& seq : range) {
      labels.emplace_back(std::move(std::get<0>(seq)));
      const std::string& read = std::get<1>(seq);
      bitmap_t bitmap;
      auto [count, last] = detail::update_bitmap(read, k, s, t, bitmap);
      kmer_count += count;
      d = std::max(d, last);
      bitmaps.emplace_back(std::move(bitmap));
    }

    g_log->info("{} kmers extracted from {} sequences", kmer_count, labels.size());
    g_log->info("largest kmer: {}", d);

    solver::greedy_order_bitmaps(bitmaps, labels);

    return build_index(bitmaps, std::move(labels), k, s, t);
  }

  [[nodiscard]] static auto build_dir_index(const fs::path& dir, std::uint64_t k, std::uint64_t s, std::uint64_t t)
      -> skimdb {
    LogFun lf{"build_dir_index(...)"};
    fastx::fastx_files_reader<fastx::fasta_buffered_reader> ffr{dir};
    return build_range_index(ffr.sequences(), k, s, t);
  }

  // merges indexes with a disjoint set of labels
  template <std::ranges::input_range Range>
  [[nodiscard]] static auto merge_disjoint_indexes(Range&& range, std::uint64_t k, std::uint64_t s, std::uint64_t t)
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

      for (const auto& [kmer, pos] : db.index_) {
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
