#ifndef SKIMDB_BUILDER_H
#define SKIMDB_BUILDER_H

#include <algorithm>
#include <execution>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <roaring.hh>

#include "skimdb/skimdb_encoding.h"
#include "skimdb/skimdb_query.h"
#include "skimdb/skimdb_util.h"


namespace skim {

namespace fs = std::filesystem;

class skim_db_builder {
public:

  static skim_db build_index(const fs::path& dir,
                             const std::vector<std::string>& files,
                             const std::vector<std::string>& labels,
                             std::size_t k,
                             std::size_t s,
                             std::size_t t) {
    std::vector<roaring::Roaring> bitmaps(files.size());
    std::vector<std::size_t> idxs(bitmaps.size());

    std::iota(idxs.begin(), idxs.end(), 0);

    std::for_each(std::execution::par, idxs.begin(), idxs.end(),
                  [&](std::size_t i) { detail::populate_bitmap(dir, files[i], bitmaps[i], k, s, t); });

    std::size_t total_kmers = detail::total_kmer_count(k, s, t);

    std::vector<skim::encoding> data(total_kmers);

    std::unordered_map<std::uint32_t, std::size_t> kmer_to_index;
    kmer_to_index.reserve(total_kmers);

    std::size_t free_idx = 0;

    for (std::size_t i = 0, end = bitmaps.size(); i < end; i++) {
      roaring::Roaring& bitmap = bitmaps[i];

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

    db.labels_ = std::move(labels);
    db.kmer_to_index_ = std::move(kmer_to_index);
    db.data_ = std::move(data);

    return db;
  }

  static skim_db build_index(const fs::path& dir, const fs::path& f2l, std::size_t k, std::size_t s, std::size_t t) {
    auto [files, labels] = detail::load_f2l(f2l);
    return build_index(dir, files, labels, k, s, t);
  }
};

}

#endif // SKIMDB_BUILDER_H
