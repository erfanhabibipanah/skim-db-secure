#ifndef SKIMDB_H
#define SKIMDB_H

#include <expected>
#include <filesystem>
#include <fstream>
#include <generator>
#include <string>
#include <tuple>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/phmap_dump.h>

#include "skimdb/skimdb_encoding.h"
#include "skimdb/skimdb_util.h"


namespace skim {

namespace fs = std::filesystem;


class skimdb {
public:
  skimdb() = default;

  // given a kmer, returns a generator over annotated labels
  auto query(std::string kmer) -> std::generator<const std::string&> {
    if (!detail::is_valid(kmer, k_)) {
      co_return;
    }

    auto kmer_rec = index_.find(detail::kmer_to_uint32(kmer));
    if (kmer_rec == index_.end()) {
      co_return;
    }

    auto kmer_idx = kmer_rec->second;

    for (std::size_t label_idx : data_[kmer_idx].select_idxs()) {
      if (label_idx >= labels_.size()) {
        break;
      }
      co_yield labels_[label_idx];
    }
  }

  auto parameters() const { return std::make_tuple(k_, s_, t_); }

  auto load(const fs::path& path) -> std::expected<void, std::string> {
    std::ifstream is{path, std::ios::binary};
    if (!is) {
      return std::unexpected{"could not open file"};
    }

    try {
      cereal::BinaryInputArchive archive(is);
      archive(*this);
    } catch (...) {
      return std::unexpected{"deserialization failed"};
    }

    return {};
  }

  auto save(const fs::path& path) -> std::expected<void, std::string> {
    std::ofstream os{path, std::ios::binary};
    if (!os) {
      return std::unexpected{"could not create file"};
    }

    try {
      cereal::BinaryOutputArchive archive{os};
      archive(*this);  // Uses serialize() internally
    } catch (...) {
      return std::unexpected{"serialization failed"};
    }

    return {};
  }

  template <class Archive>
  void serialize(Archive& archive) {
    archive(k_, s_, t_, labels_, index_, data_);
  }

private:
  friend class builder;

  std::size_t k_;
  std::size_t s_;
  std::size_t t_;

  std::vector<std::string> labels_;
  phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index_;
  std::vector<skim::encoding> data_;
};

}

#endif // SKIMDB_H
