#ifndef SKIMDB_SPIR_DEFINITIONS_H
#define SKIMDB_SPIR_DEFINITIONS_H

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <skimdb/skimdb.h>

#include "skimdb_spir_matrix.h"


namespace skim::spir {

// random number generator that SPIR client and server agree to use
using spir_common_rng_t = std::mt19937_64;


struct skimdb_metadata {
  skimdb::kmer_index index;        // kmer to row index map
  std::vector<std::string> labels; // annotated labels
};

struct spirdb_parameters {
  std::size_t n;  // LWE dimension
  double sigma;   // LWE error distribution stddev

  std::size_t log_p; // plaintext modulus
  std::size_t log_q; // ciphertext modulus

  std::size_t batch_size; // number of qu vectors which can be processed in one batch

  std::size_t block_size; // bytes of plaintext data we can pack into one block
  std::size_t rle_blocks; // number of blocks needed per RLE encoding
  std::size_t sqrt_N;     // matrix side length (blocks of data)

  std::uint64_t seed; // seed for matrix A
};

struct spirdb_query_state {
  spir_matrix s_vec;  // secret vector
  spir_matrix qu_vec; // encrypted query vector
};

} // namespace skim::spir

#endif // SKIMDB_SPIR_DEFINITIONS_H
