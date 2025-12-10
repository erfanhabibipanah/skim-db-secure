#ifndef SKIMDB_H
#define SKIMDB_H

#include <filesystem>
#include <fstream>
#include <generator>
#include <string>
#include <unordered_map>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>

#include "skimdb/skimdb_encoding.h"
#include "skimdb/skimdb_util.h"


namespace skim {

namespace fs = std::filesystem;


class skim_db {
public:
  skim_db() = default;

  // given a kmer, returns a generator over annotated labels
  std::generator<std::string> query(std::string kmer) {
    if (!detail::is_valid(kmer, k_)) {
      co_return;
    }

    auto kmer_rec = index_.find(detail::kmer_to_uint32(kmer));
    if (kmer_rec == index_.end()) {
      co_return;
    }

    auto kmer_idx = kmer_rec->second;
    if (kmer_idx >= data_.size()) {
      co_return;
    }

    for (std::size_t label_idx : data_[kmer_idx].select_idxs()) {
      if (label_idx >= labels_.size()) {
        break;
      }
      co_yield labels_[label_idx];
    }
  }

  // save data to skim_db file
  bool save(const fs::path& path) {
    try {
      std::ofstream os{path, std::ios::binary};
      if (!os) {
        return false;
      }
      cereal::BinaryOutputArchive archive{os};
      archive(*this);  // Uses serialize() internally
    } catch (...) {
      return false;
    }

    return true;
  }

  // load data from skim_db file
  bool load(const fs::path& path) {
    try {
      std::ifstream is{path, std::ios::binary};
      if (!is) {
        return false;
      }

      cereal::BinaryInputArchive archive(is);
      archive(*this);
    } catch (...) {
      return false;
    }

    return true;
  }

  template<class Archive>
  void serialize(Archive& archive) {
    archive(k_, s_, t_, labels_, index_, data_);
  }

private:
  friend class skim_db_builder;

  std::size_t k_;
  std::size_t s_;
  std::size_t t_;

  std::vector<std::string> labels_;
  std::unordered_map<std::uint32_t, std::size_t> index_;
  std::vector<skim::encoding> data_;
};

}

#endif // SKIMDB_H
