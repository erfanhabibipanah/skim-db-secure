#ifndef SKIMDB_SIPER_DEFINITIONS_H
#define SKIMDB_SIPER_DEFINITIONS_H

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <skimdb/skimdb.h>

#include "skimdb_siper_matrix.h"


namespace skim::siper {

// random number generator that SiPeR client and server agree to use
using siper_common_rng_t = std::mt19937_64;


struct skimdb_metadata {
  skimdb::kmer_index index;                 // kmer to row index map
  std::vector<std::uint64_t> kmer_metadata; // packed starting position and length of each kmer's RLE in the matrix
  std::vector<std::string> labels;          // annotated labels
};

struct siperdb_parameters {
  std::size_t n; // LWE dimension
  double sigma;  // LWE error distribution stddev

  std::size_t log_p; // plaintext modulus
  std::size_t log_q; // ciphertext modulus

  std::size_t block_size; // number of runs that we logically treat as a single block when packing the matrix
  std::size_t batch_size; // number of qu vectors which can be processed in one batch
  std::size_t sqrt_N;     // matrix side length (runs per row and column), should be a multiple of block_size

  std::uint64_t seed; // seed for matrix A

  std::string metadata_hash; // hash of client metadata (for verification)
  std::string hint_c_hash;   // hash of hint_c (for verification)
};

struct siperdb_query_state {
  siper_matrix<std::uint64_t> s_vec;  // secret vector
  siper_matrix<std::uint64_t> qu_vec; // encrypted query vector
};

} // namespace skim::siper

namespace std {
template <>
struct formatter<skim::siper::siperdb_parameters> {
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const skim::siper::siperdb_parameters& p, FormatContext& ctx) const {
    return format_to(ctx.out(),
                     "n={}\nsigma={}\nlog_p={}\nlog_q={}\nblock_size={}\nbatch_size={}\nsqrt_N={}\nseed={}\n"
                     "metadata_hash={}\nhint_c_hash={}",
                     p.n,
                     p.sigma,
                     p.log_p,
                     p.log_q,
                     p.block_size,
                     p.batch_size,
                     p.sqrt_N,
                     p.seed,
                     p.metadata_hash,
                     p.hint_c_hash);
  }
};
} // namespace std

#endif // SKIMDB_SIPER_DEFINITIONS_H
