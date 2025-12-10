#ifndef SKIMDB_BUILDER_H
#define SKIMDB_BUILDER_H

#include <algorithm>
#include <execution>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <ranges>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include <roaring.hh>

#include "skimdb/skimdb_encoding.h"
#include "skimdb/skimdb_query.h"
#include "skimdb/skimdb_util.h"


namespace skim {

namespace fs = std::filesystem;

class skim_db_builder {
public:
  static skim_db build_file_index(const std::vector<roaring::Roaring>& bitmaps,
                                  const std::vector<std::string>& labels,
                                  std::size_t k,
                                  std::size_t s,
                                  std::size_t t) {
    std::size_t total_kmers = detail::total_kmer_count(k, s, t);

    std::vector<skim::encoding> data(total_kmers);

    std::unordered_map<std::uint32_t, std::size_t> kmer_to_index;
    kmer_to_index.reserve(total_kmers);

    std::size_t free_idx = 0;

    for (std::size_t i = 0, end = bitmaps.size(); i < end; i++) {
      auto& bitmap = bitmaps[i];

      for (std::uint32_t kmer : bitmap) {
        auto [it, inserted] = kmer_to_index.try_emplace(kmer, free_idx);
        if (inserted) {
          free_idx++;
        }

        std::size_t idx = it->second;
        data[idx].push(i);
      }
    }

    std::for_each(std::execution::par, data.begin(), data.end(), [](encoding& rec) { rec.attempt_compress(); });

    skim_db db;

    db.k_ = k;
    db.s_ = s;
    db.t_ = t;

    db.labels_ = labels;
    db.kmer_to_index_ = std::move(kmer_to_index);
    db.data_ = std::move(data);

    return db;
  }

  static skim_db build_file_index(const fs::path& dir,
                                  const std::vector<std::string>& files,
                                  const std::vector<std::string>& labels,
                                  std::size_t k,
                                  std::size_t s,
                                  std::size_t t) {
    std::vector<roaring::Roaring> bitmaps(files.size());
    auto zipped = std::views::zip(files, bitmaps);

    std::for_each(std::execution::par, zipped.begin(), zipped.end(),
                  [&](auto&& fb) {
                    auto& [file, bitmap] = fb;
                    bitmap = detail::populate_bitmap(dir, file, k, s, t); });

    return build_file_index(bitmaps, labels, k, s, t);
  }

  static skim_db build_file_index(const fs::path& dir, const fs::path& f2l,
                                  std::size_t k, std::size_t s, std::size_t t) {
    auto [files, labels] = detail::load_f2l(f2l);
    return build_file_index(dir, files, labels, k, s, t);
  }

  static skim_db build_sequence_index(const fs::path& dir, std::size_t k, std::size_t s, std::size_t t) {
    fastx::fastx_files_reader<fastx::fasta_buffered_reader> ffr{dir};

    std::vector<roaring::Roaring> bitmaps;
    std::vector<std::string> labels;

    for (auto seq : ffr.sequences()) {
      labels.emplace_back(std::move(std::get<0>(seq)));
      const std::string& read = std::get<1>(seq);
      roaring::Roaring bitmap;
      detail::update_bitmap(read, k, s, t, bitmap);
      bitmaps.emplace_back(bitmap);
    }

    detail::order_bitmaps(bitmaps, labels);

    return build_file_index(bitmaps, labels, k, s, t);
  }
};

}

#endif // SKIMDB_BUILDER_H
