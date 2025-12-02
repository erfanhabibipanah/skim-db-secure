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
  static skim_db build(const std::size_t k,
                       const std::size_t s,
                       const std::size_t t,
                       const fs::path& f2t,
                       const fs::path& dir) {

    auto [file_names, labels] = load_file_to_labels(f2t);

    std::vector<roaring::Roaring> bitmaps(file_names.size());
    std::vector<std::size_t> idxs(bitmaps.size());

    std::iota(idxs.begin(), idxs.end(), 0);

    std::for_each(std::execution::par, idxs.begin(), idxs.end(),
                  [&](std::size_t i) {
                    populate_bitmap(k, s, t, dir, file_names[i], bitmaps[i]);
                  });

    std::size_t total_kmers = total_kmer_count(k, s, t);

    std::vector<skim::encoding> data(total_kmers);

    std::unordered_map<std::uint32_t, std::size_t> kmer_to_index;
    kmer_to_index.reserve(total_kmers);

    std::size_t free_idx = 0;

    for (size_t i = 0; i < bitmaps.size(); i++) {
      roaring::Roaring& bitmap = bitmaps[i];

      for (uint32_t kmer : bitmap) {
        auto [it, inserted] = kmer_to_index.try_emplace(kmer, free_idx);
        if (inserted) {
          free_idx++;
        }

        std::size_t idx = it->second;
        data[idx].push(i);
      }
    }

    std::for_each(std::execution::par, data.begin(), data.end(),
                  [](encoding& rec) { rec.attempt_compress(); });

    skim_db db;

    db.k_ = k;
    db.s_ = s;
    db.t_ = t;

    db.labels_ = std::move(labels);
    db.kmer_to_index_ = std::move(kmer_to_index);
    db.data_ = std::move(data);

    return db;
  } // build
};

} // namespace skim

#endif // SKIMDB_BUILDER_H
