#ifndef SKIMDB_SPIR_PARAMETERS_H
#define SKIMDB_SPIR_PARAMETERS_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <parallel_hashmap/phmap.h>

#include "skimdb_spir_matrix.h"


namespace skim {
namespace spir {

// random number generator that SPIR client and server agree to use
using spir_common_rng = std::mt19937_64;


struct skimdb_metadata {
  phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index; // kmer to row index map
  std::vector<std::string> labels;                                 // annotated labels
};

struct spirdb_parameters {
  std::uint64_t n; // LWE dimension
  double sigma;    // LWE error distribution stddev

  std::uint64_t rle_blocks; // number of blocks needed per RLE encoding
  std::uint64_t sqrt_N;     // matrix side length (blocks of data)

  std::uint64_t log_p; // plaintext modulus
  std::uint64_t log_q; // ciphertext modulus

  std::uint64_t seed; // seed for matrix A
};

struct spirdb_query_state {
  std::uint64_t i_row; // target row index
  spir_matrix s_vec;   // secret vector
  spir_matrix qu_vec;  // encrypted query vector
};

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_PARAMETERS_H
