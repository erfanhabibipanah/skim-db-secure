#ifndef SKIMDB_H
#define SKIMDB_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <generator>
#include <string>
#include <vector>

#include <bbhash/bbhash.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_encoding.h"
#include "detail/skimdb_util.h"


namespace skim {

namespace fs = std::filesystem;

class skimdb final {
public:
  using parameters_type = skimdb_parameters;

  struct kmer_index {
    bitmap_t kmers;
    bbh::bbhash<std::uint64_t> hash;
  }

  struct skimdb_components {
    std::vector<detail::encoding> data;
    std::vector<std::string> labels;
    phmap::parallel_flat_hash_map<std::uint32_t, std::uint64_t> index;
  };


  skimdb() = default;


  [[nodiscard]] auto parameters() const -> parameters_type { return parameters_type{.k = k_, .s = s_, .t = t_}; }

  [[nodiscard]] auto explode() && -> skimdb_components {
    return {.data = std::move(data_), .labels = std::move(labels_), .index = std::move(index_)};
  }


  // given a kmer, returns a generator over annotated labels
  [[nodiscard]] auto query(std::string s) const -> std::generator<const std::string&> {
    auto pos = m_find_kmer_pos_(s);

    if (!pos.has_value()) {
      co_return;
    }

    for (auto label_idx : m_traverse_kmer_(pos.value())) {
      if (label_idx >= labels_.size()) {
        break;
      }
      co_yield labels_[label_idx];
    }
  }


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

  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    std::ofstream os{path, std::ios::binary};
    if (!os) {
      return std::unexpected{"could not create file"};
    }

    try {
      cereal::BinaryOutputArchive archive{os};
      archive(*this);
    } catch (...) {
      return std::unexpected{"serialization failed"};
    }

    os.close();

    return fs::file_size(path);
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(k_, s_, t_, labels_, index_, data_);
  }

private:
  friend class builder;

  [[nodiscard]] auto m_find_kmer_pos_(const std::string& s) const -> std::optional<std::uint64_t> {
    if (!detail::is_valid(s, k_)) {
      return std::nullopt;
    }

    auto kmer = detail::kmer_to_uint32(s);
    auto kmer_idx = std::min(kmer, detail::reverse_complement(kmer, k_));

    auto it = index_.find(kmer_idx);

    if (it == index_.end()) {
      return std::nullopt;
    }

    return it->second;
  }

  [[nodiscard]] auto m_traverse_kmer_(std::uint64_t kmer_pos) const -> std::generator<std::uint64_t> {
    for (auto label_idx : data_[kmer_pos].select_idxs()) {
      co_yield label_idx;
    }
  }

  std::uint64_t k_{0};
  std::uint64_t s_{0};
  std::uint64_t t_{0};

  std::vector<detail::encoding> data_;
  std::vector<std::string> labels_;
  phmap::parallel_flat_hash_map<std::uint32_t, uint64_t> index_;
};

} // namespace skim

#endif // SKIMDB_H
