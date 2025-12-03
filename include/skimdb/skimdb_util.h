#ifndef SKIMDB_UTIL_H
#define SKIMDB_UTIL_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include <fastxrd/fasta_simple_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include <roaring.hh>


namespace skim {

namespace fs = std::filesystem;

// Parse file_to_labels file into pair of vectors (file_names, labels)
// Returns empty on failure, skips ill-formatted lines
std::pair<std::vector<std::string>, std::vector<std::string>> load_file_to_labels(const fs::path& path) {
  std::vector<std::string> names;
  std::vector<std::string> labels;

  std::ifstream f{path};

  if (!f) {
    return {names, labels};
  }

  std::string name;
  std::string id;

  while (!f.eof()) {
    f >> name;
    std::getline(f, id);
    names.push_back(name);
    labels.push_back(id);
  }

  return {names, labels};
} // load_file_to_labels

inline bool kmer_is_valid(std::size_t k, const std::string& kmer) {
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
} // kmer_is_valid

inline int to_base2(char c) {
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
} // to_base2

uint32_t kmer_to_uint32(const std::string& kmer) {
  if (kmer.length() > 16) {
    return 0; // k-mers longer than 16 not supported
  }

  uint32_t result = 0;

  for (char c : kmer) {
    int base = to_base2(c);
    if (base == -1) {
      return 0; // invalid character
    }
    result = (result << 2) | static_cast<uint32_t>(base);
  }

  return result;
} // kmer_to_uint32

uint32_t reverse_complement(std::size_t k, uint32_t kmer) {
  uint32_t rev_comp = 0;

  for (std::size_t i = 0; i < k; ++i) {
    rev_comp = (rev_comp << 2) | (3 - (kmer & 3));
    kmer >>= 2;
  }

  return rev_comp;
}

bool check_syncmer(std::size_t k, std::size_t s, std::size_t t, uint32_t kmer) {
  if (s == 0 || s >= k) {
    return true;
  }

  uint32_t smer_mask = (1ULL << (2 * s)) - 1;
  std::size_t num_smers = k - s + 1;

  std::size_t tmer_shift = 2 * (k - s - t);
  uint32_t tmer = (kmer >> tmer_shift) & smer_mask;

  for (std::size_t i = 0; i < num_smers; ++i) {
    uint32_t smer = kmer & smer_mask;
    if (smer < tmer) {
      return false;
    }
    kmer >>= 2;
  }

  return true;
}

// Opens fasta file and processes canonical syncmers into a roaring bitmap
void populate_bitmap(std::size_t k,
                     std::size_t s,
                     std::size_t t,
                     const fs::path& dir,
                     const std::string& filename,
                     roaring::Roaring& bitmap) {

  fs::path full_path = dir / filename;
  fastx::fastx_files_reader<fastx::fasta_simple_reader> ffr{full_path};

  for (auto seq : ffr.sequences()) {
    std::string read = std::get<1>(seq);

    uint32_t kmer = 0;
    uint32_t rev_comp = 0;

    uint32_t kmer_mask = (1ULL << (2 * k)) - 1;

    std::size_t base_count = 0;

    for (auto i = 0, end = read.length(); i < end; ++i) {
      auto base = to_base2(read[i]);

      if (base < 0) {
        base_count = 0;
        continue;
      }

      kmer = (kmer << 2) | base;
      rev_comp = (rev_comp >> 2) | (static_cast<uint32_t>(3 - base) << ((k - 1) * 2));
      base_count++;

      if (base_count >= k) {
        uint32_t canonical = std::min((kmer & kmer_mask), (rev_comp & kmer_mask));
        if (check_syncmer(k, s, t, canonical)) {
          bitmap.add(canonical);
        }
      }
    }
  }
}

std::size_t total_kmer_count(std::size_t k, std::size_t s, std::size_t t) {
  uint64_t num_kmers = 1ULL << (2 * k);

  if (s == 0 || s >= k) {
    uint64_t num_palindromes = (1ULL << (2 * (k / 2))) * ((k + 1) % 2);
    return static_cast<std::size_t>((num_kmers + num_palindromes) / 2);
  }

  std::size_t count = 0;

  for (uint32_t kmer = 0; kmer < num_kmers; ++kmer) {
    uint32_t canonical = std::min(kmer, reverse_complement(k, kmer));

    if (kmer == canonical && check_syncmer(k, s, t, kmer)) {
      count += 1;
    }
  }

  return count;
}

} // namespace skim

#endif // SKIMDB_UTIL_H
