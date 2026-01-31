#ifndef SKIMDB_SPIR_PARAMS_H
#define SKIMDB_SPIR_PARAMS_H

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include <parallel_hashmap/phmap.h>

#include "skimdb_spir_matrix.h"


namespace skim {
namespace spir {

struct db_config {
  std::uint64_t k;
  std::uint64_t s;
  std::uint64_t t;
};

struct db_metadata {
  phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index; // kmer to row index map
  std::vector<std::string> labels;  // annotated labels
};

struct spir_params {
  std::uint64_t n;            // LWE dimension
  double sigma;               // LWE error distribution stddev

  std::uint64_t rle_blocks;   // number of blocks needed per RLE encoding
  std::uint64_t sqrt_N;       // matrix side length (blocks of data)

  std::uint64_t log_p;        // plaintext modulus
  std::uint64_t log_q;        // ciphertext modulus
  std::uint64_t mat_seed;     // seed for matrix A
};

struct query_state {
  std::uint64_t i_row;        // target row index
  spir_matrix secret_vec;     // secret vector
  spir_matrix enc_vec;        // encrypted query vector
};

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_PARAMS_H
