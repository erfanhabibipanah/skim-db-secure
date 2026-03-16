#ifndef SKIMDB_UTIL_H
#define SKIMDB_UTIL_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fastxrd/fasta_simple_reader.h>

#include "skimdb_logger.h"
#include "skimdb_definitions.h"


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

  std::string name;
  std::string id;

  while (!f.eof()) {
    f >> name;
    std::getline(f, id);
    names.push_back(name);
    labels.push_back(id);
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

inline auto kmer_to_uint32(const std::string& kmer) noexcept -> std::uint32_t {
  if (kmer.length() > g_kmer_limit) {
    return 0;
  }

  std::uint32_t result = 0;

  for (char c : kmer) {
    int base = char_to_base2(c);
    if (base == -1) {
      return 0; // invalid character
    }
    result = (result << 2) | static_cast<std::uint32_t>(base);
  }

  return result;
}

inline auto reverse_complement(std::uint32_t kmer, std::size_t k) noexcept -> std::uint32_t {
  std::uint32_t rev_comp = 0;

  for (std::size_t i = 0; i < k; ++i) {
    rev_comp = (rev_comp << 2) | (3 - (kmer & 3));
    kmer >>= 2;
  }

  return rev_comp;
}

inline auto is_syncmer(uint32_t kmer, std::size_t k, std::size_t s, std::size_t t) noexcept -> bool {
  if (s == 0 || s >= k) {
    return true;
  }

  std::uint32_t smer_mask = (1U << (2 * s)) - 1;
  std::size_t num_smers = k - s + 1;

  std::size_t tmer_shift = 2 * (k - s - t);
  std::uint32_t tmer = (kmer >> tmer_shift) & smer_mask;

  for (std::size_t i = 0; i < num_smers; ++i) {
    std::uint32_t smer = kmer & smer_mask;
    if (smer < tmer) {
      return false;
    }
    kmer >>= 2;
  }

  return true;
}

inline auto update_bitmap(const std::string& read, std::size_t k, std::size_t s, std::size_t t, bitmap_t& bitmap) {
  std::uint32_t kmer = 0;
  std::uint32_t rev_comp = 0;

  std::size_t base_count = 0;
  std::uint32_t kmer_mask = (1ULL << (2 * k)) - 1;

  std::uint32_t tot_added = 0;
  std::uint32_t last_added = 0;

  for (std::size_t i = 0, end = read.length(); i < end; ++i) {
    auto base = char_to_base2(read[i]);

    if (base < 0) {
      base_count = 0;
      continue;
    }

    kmer = (kmer << 2) | base;
    rev_comp = (rev_comp >> 2) | (static_cast<std::uint32_t>(3 - base) << ((k - 1) * 2));
    base_count++;

    if (base_count >= k) {
      std::uint32_t canonical = std::min((kmer & kmer_mask), (rev_comp & kmer_mask));
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
  std::uint64_t num_kmers = 1ULL << (2 * k);

  if (s == 0 || s >= k) {
    std::uint64_t num_palindromes = (1ULL << (2 * (k >> 1))) * ((k + 1) % 2);
    return static_cast<std::size_t>((num_kmers + num_palindromes) / 2);
  }

  std::size_t count = 0;

  for (std::uint32_t kmer = 0; kmer < num_kmers; ++kmer) {
    std::uint32_t canonical = std::min(kmer, reverse_complement(kmer, k));

    if (kmer == canonical && is_syncmer(kmer, k, s, t)) {
      count += 1;
    }
  }

  return count;
}

inline auto estimated_kmer_count(std::size_t k, std::size_t s, std::size_t) noexcept -> std::size_t {
  std::uint64_t num_kmers = 1ULL << (2 * k);

  if (s == 0 || s >= k) {
    std::uint64_t num_palindromes = (1ULL << (2 * (k >> 1))) * ((k + 1) % 2);
    return static_cast<std::size_t>((num_kmers + num_palindromes) / 2);
  }

  // we use compressiom factor from the syncmer paper
  return num_kmers / (k - s + 1);
}

} // namespace skim::detail

#endif // SKIMDB_UTIL_H
