#ifndef SKIMDB_BUILDER_H
#define SKIMDB_BUILDER_H

#include <algorithm>
#include <execution>
#include <filesystem>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include "detail/skimdb_encoding.h"
#include "detail/skimdb_logger.h"
#include "detail/skimdb_util.h"
#include "skimdb.h"
#include "skimdb_solvers.h"


namespace skim {

namespace fs = std::filesystem;

class builder {
public:

  static auto build_index(const std::vector<bitmap_t>& bitmaps, const std::vector<std::string>& labels,
                          std::size_t k, std::size_t s, std::size_t t) -> skimdb {
    g_log->info("build_index start...");

    g_log->info("creating kmer index, k={}, s={}, t={}...", k, s, t);

    // TODO: can we optimize this shit: counting k-mers is quite expensive...
    std::size_t total_kmers = detail::total_kmer_count(k, s, t);

    g_log->info("found {} total kmers", total_kmers);

    std::vector<detail::encoding> data(total_kmers);

    phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index;
    index.reserve(total_kmers);

    std::size_t free_idx = 0;

    for (std::size_t i = 0, end = bitmaps.size(); i < end; i++) {
      auto& bitmap = bitmaps[i];

      for (std::uint32_t kmer : bitmap) {
        auto [it, inserted] = index.try_emplace(kmer, free_idx);
        if (inserted) {
          free_idx++;
        }

        std::size_t idx = it->second;
        data[idx].push(i);
      }
    }

    g_log->info("kmer index done!");

    g_log->info("compressing kmers...");
    std::for_each(std::execution::par, data.begin(), data.end(), [](detail::encoding& rec) { rec.attempt_compress(); });
    g_log->info("compression done!");

    skimdb db;

    db.k_ = k;
    db.s_ = s;
    db.t_ = t;

    db.labels_ = labels;
    db.index_ = std::move(index);
    db.data_ = std::move(data);

    g_log->info("build_index done!");

    return db;
  }

  static auto build_file_index(const fs::path& dir, const std::vector<std::string>& files,
                               const std::vector<std::string>& labels, std::size_t k, std::size_t s, std::size_t t) -> skimdb {
    std::vector<bitmap_t> bitmaps(files.size());
    auto zipped = std::views::zip(files, bitmaps);

    std::for_each(std::execution::par, zipped.begin(), zipped.end(), [&](auto&& fb) {
      auto& [file, bitmap] = fb;
      bitmap = detail::populate_bitmap(dir, file, k, s, t);
    });

    return build_index(bitmaps, labels, k, s, t);
  }

  static auto build_file_index(const fs::path& dir, const fs::path& f2l,
                               std::size_t k, std::size_t s, std::size_t t) -> skimdb {
    auto [files, labels] = detail::load_f2l(f2l);
    return build_file_index(dir, files, labels, k, s, t);
  }

  static auto build_sequence_index(const fs::path& dir, std::size_t k, std::size_t s, std::size_t t) -> skimdb {
    g_log->info("build_sequence_index start...");

    fastx::fastx_files_reader<fastx::fasta_buffered_reader> ffr{dir};

    std::vector<bitmap_t> bitmaps;
    std::vector<std::string> labels;

    g_log->info("extracting kmers from {}", dir.string());
    g_log->info("using k={}, s={}, t={}...", k, s, t);

    std::uint64_t kmer_count = 0;

    for (auto seq : ffr.sequences()) {
      labels.emplace_back(std::move(std::get<0>(seq)));
      const std::string& read = std::get<1>(seq);
      bitmap_t bitmap;
      kmer_count += detail::update_bitmap(read, k, s, t, bitmap);
      bitmaps.emplace_back(bitmap);
    }

    g_log->info("{} kmers extracted from {} sequences", kmer_count, labels.size());

    auto res = build_index(bitmaps, labels, k, s, t);

    g_log->info("build_sequence_index done!");

    return res;
  }
};

} // namespace skim

#endif // SKIMDB_BUILDER_H
