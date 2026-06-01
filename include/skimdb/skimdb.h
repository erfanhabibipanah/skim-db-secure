#ifndef SKIMDB_H
#define SKIMDB_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <execution>
#include <expected>
#include <filesystem>
#include <generator>
#include <optional>
#include <string>
#include <vector>

#include <bbhash/bbhash.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "detail/skimdb_definitions.h"
#include "detail/skimdb_encoding.h"
#include "detail/skimdb_util.h"

#include "skimdb_config.h"


namespace skim {

namespace fs = std::filesystem;


struct skim_runtime_config {
  unsigned int grpc_connect_timeout{5};                   // gRPC connection timeout (seconds)
  unsigned int grpc_download_timeout{300};                // gRPC data download timeout (seconds)
  std::string siper_server_store_dir{".skimdb-server"};   // path to directory where server stores hint data
  std::string siper_client_metadata_dir{".skimdb-cache"}; // path to directory to store metadata on client's side
  std::string siper_client_hint_c_dir{".skimdb-cache"};   // path to directory to store hint_c on client's side
};

skim_runtime_config g_skim_config;


class skimdb final {
public:
  using parameters_type = skimdb_parameters;

  static_assert(sizeof(std::size_t) == sizeof(bbh::bbhash<kmer_binary_t>::size_type) &&
                    std::is_unsigned_v<bbh::bbhash<kmer_binary_t>::size_type>,
                "size_type must be equivalent to std::size_t");

  struct kmer_index {
    kmer_index() = default;

    bitmap_t kmers{};
    bbh::bbhash<kmer_binary_t> hash;

    [[nodiscard]] auto find(kmer_binary_t kmer) const -> std::optional<std::size_t> {
      if (kmers.contains(kmer)) {
        return hash.find(kmer);
      }
      return std::nullopt;
    }

    template <class Archive>
    void serialize(Archive& ar) {
      ar(CEREAL_NVP(kmers));
      ar(CEREAL_NVP(hash));
    }
  };

  struct skimdb_components {
    std::vector<detail::encoding> data;
    std::vector<std::string> labels;
    kmer_index index;
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
      skimdb_version_t ver;
      archive(ver);
      archive(*this);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("deserialization failed {}", e.what())};
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
      skimdb_version_t ver;
      archive(ver);
      archive(*this);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("serialization failed {}", e.what())};
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

  [[nodiscard]] auto m_find_kmer_pos_(const std::string& s) const -> std::optional<std::size_t> {
    if (!detail::is_valid(s, k_)) {
      return std::nullopt;
    }

    auto kmer = detail::kmer_to_binary(s);
    auto kmer_idx = std::min(kmer, detail::reverse_complement(kmer, k_));

    return index_.find(kmer_idx);
  }

  [[nodiscard]] auto m_traverse_kmer_(std::size_t kmer_pos) const -> std::generator<std::size_t> {
    for (auto label_idx : data_[kmer_pos].select_idxs()) {
      co_yield label_idx;
    }
  }

  std::size_t k_{0};
  std::size_t s_{0};
  std::size_t t_{0};

  std::vector<detail::encoding> data_;
  std::vector<std::string> labels_;
  kmer_index index_;
};

} // namespace skim

#endif // SKIMDB_H
