#ifndef SKIMDB_UTIL_H
#define SKIMDB_UTIL_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cereal/cereal.hpp>
#include <fastxrd/fasta_simple_reader.h>

#include "skimdb_logger.h"
#include "skimdb_definitions.h"


namespace cereal {

template <class Archive>
void save(Archive& ar, const skim::bitmap_t& bitmap) {
  auto size = bitmap.getSizeInBytes();
  std::vector<char> buf(size);
  bitmap.write(buf.data());
  ar(cereal::make_nvp("size", size));
  ar(cereal::make_nvp("data", buf));
}

template <class Archive>
void load(Archive& ar, skim::bitmap_t& bitmap) {
  std::size_t size{};
  ar(cereal::make_nvp("size", size));
  std::vector<char> buf(size);
  ar(cereal::make_nvp("data", buf));
  bitmap = skim::bitmap_t::readSafe(buf.data(), size);
}

} // namespace cereal


namespace skim {

class kmer_distribution {
public:
  using result_type = std::string;

  struct param_type {
    std::size_t k;
    friend auto operator==(const param_type&, const param_type&) -> bool = default;
  };

  kmer_distribution() = default;
  explicit kmer_distribution(std::size_t k) : params_{k} {}
  explicit kmer_distribution(const param_type& p) : params_{p} {}

  [[nodiscard]] auto param() const noexcept { return params_; }
  void param(const param_type& p) noexcept { params_ = p; }

  [[nodiscard]] auto k() const noexcept { return params_.k; }

  void reset() noexcept {}

  template <typename URBG>
  auto operator()(URBG& g) -> result_type {
    return (*this)(g, params_);
  }

  template <typename URBG>
  auto operator()(URBG& g, const param_type& p) -> result_type {
    std::string s;
    s.resize(p.k);

    std::uniform_int_distribution<int> dist(0, 3);

    for (std::size_t i = 0; i < p.k; ++i) {
      s[i] = alphabet_[dist(g)];
    }

    return s;
  }

  friend auto operator==(const kmer_distribution&, const kmer_distribution&) -> bool = default;

private:
  param_type params_{0};
  static constexpr std::array<char, 4> alphabet_{'A', 'C', 'G', 'T'};
};

} // namespace skim


namespace skim::detail {

namespace fs = std::filesystem;

// Parse file_to_labels file into pair of vectors (file_names, labels)
// Returns empty on failure, skips ill-formatted lines
inline auto load_f2l(const fs::path& path) {
  std::vector<std::string> names;
  std::vector<std::string> labels;

  std::ifstream f{path};

  if (!f) {
    return std::make_pair(names, labels);
  }

  std::string line;
  while (std::getline(f, line)) {
    std::istringstream iss(line);

    std::string name;
    if (!(iss >> name)) {
        continue;
    }

    std::string label;
    std::getline(iss >> std::ws, label);

    if (label.empty()) {
      continue;
    }

    names.push_back(name);
    labels.push_back(label);
  }

  return std::make_pair(names, labels);
}

inline auto is_valid(const std::string& kmer, std::size_t k) noexcept -> bool {
  if (kmer.length() != k) {
    return false;
  }

  for (char c : kmer) {
    switch (c) {
    case 'A':
    case 'a':
    case 'C':
    case 'c':
    case 'G':
    case 'g':
    case 'T':
    case 't':
      continue;
    default:
      return false;
    }
  }

  return true;
}

inline auto char_to_base2(char c) noexcept -> int {
  switch (c) {
  case 'A':
  case 'a':
    return 0;
  case 'C':
  case 'c':
    return 1;
  case 'G':
  case 'g':
    return 2;
  case 'T':
  case 't':
    return 3;
  default:
    return -1;
  }
}

inline auto kmer_to_binary(const std::string& kmer) noexcept -> kmer_binary_t {
  if (kmer.length() > g_kmer_limit) {
    return 0;
  }

  kmer_binary_t result = 0;

  for (char c : kmer) {
    int base = char_to_base2(c);
    if (base == -1) {
      return 0; // invalid character
    }
    result = (result << 2) | static_cast<kmer_binary_t>(base);
  }

  return result;
}

inline auto reverse_complement(kmer_binary_t kmer, std::size_t k) noexcept -> kmer_binary_t {
  kmer_binary_t rev_comp = 0;

  for (std::size_t i = 0; i < k; ++i) {
    rev_comp = (rev_comp << 2) | (3 - (kmer & 3));
    kmer >>= 2;
  }

  return rev_comp;
}

inline auto is_syncmer(kmer_binary_t kmer, std::size_t k, std::size_t s, std::size_t t) noexcept -> bool {
  if (s == 0 || s >= k) {
    return true;
  }

  kmer_binary_t smer_mask = (1U << (2 * s)) - 1;
  std::size_t num_smers = k - s + 1;

  std::size_t tmer_shift = 2 * (k - s - t);
  kmer_binary_t tmer = (kmer >> tmer_shift) & smer_mask;

  for (std::size_t i = 0; i < num_smers; ++i) {
    kmer_binary_t smer = kmer & smer_mask;
    if (smer < tmer) {
      return false;
    }
    kmer >>= 2;
  }

  return true;
}

inline auto update_bitmap(const std::string& read, std::size_t k, std::size_t s, std::size_t t, bitmap_t& bitmap) {
  kmer_binary_t kmer = 0;
  kmer_binary_t rev_comp = 0;

  std::size_t base_count = 0;
  kmer_binary_t kmer_mask = (1ULL << (2 * k)) - 1;

  kmer_binary_t tot_added = 0;
  kmer_binary_t last_added = 0;

  for (std::size_t i = 0, end = read.length(); i < end; ++i) {
    auto base = char_to_base2(read[i]);

    if (base < 0) {
      base_count = 0;
      continue;
    }

    kmer = (kmer << 2) | base;
    rev_comp = (rev_comp >> 2) | (static_cast<kmer_binary_t>(3 - base) << ((k - 1) * 2));
    base_count++;

    if (base_count >= k) {
      kmer_binary_t canonical = std::min((kmer & kmer_mask), (rev_comp & kmer_mask));
      if (is_syncmer(canonical, k, s, t)) {
        bitmap.add(canonical);
        last_added = canonical;
        tot_added++;
      }
    }
  }

  bitmap.runOptimize();

  return std::make_tuple(tot_added, last_added);
}

// Opens fasta file and processes canonical syncmers into a roaring bitmap
inline auto populate_bitmap(const fs::path& dir, const std::string& filename,
                            std::size_t k, std::size_t s, std::size_t t) -> bitmap_t {
  fs::path full_path = dir / filename;
  fastx::fasta_simple_reader fbr{full_path};

  bitmap_t bitmap;

  for (auto seq : fbr.sequences()) {
    const std::string& read = std::get<1>(seq);
    update_bitmap(read, k, s, t, bitmap);
  }

  return bitmap;
}

inline auto total_kmer_count(std::size_t k, std::size_t s, std::size_t t) -> std::size_t {
  LogFun lf{"total_kmer_count", spdlog::level::debug};
  std::size_t num_kmers = 1ULL << (2 * k);

  if (s == 0 || s >= k) {
    std::size_t num_palindromes = (1ULL << (2 * (k >> 1))) * ((k + 1) % 2);
    return static_cast<std::size_t>((num_kmers + num_palindromes) / 2);
  }

  std::size_t count = 0;

  for (kmer_binary_t kmer = 0; kmer < num_kmers; ++kmer) {
    kmer_binary_t canonical = std::min(kmer, reverse_complement(kmer, k));

    if (kmer == canonical && is_syncmer(kmer, k, s, t)) {
      count += 1;
    }
  }

  return count;
}

} // namespace skim::detail

#endif // SKIMDB_UTIL_H
