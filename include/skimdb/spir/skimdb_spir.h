#ifndef SKIMDB_SPIR_H
#define SKIMDB_SPIR_H

#include <cmath>
#include <cstdint>
#include <expected>
#include <generator>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <dgpp/uniform_rejection.hpp>

#include <skimdb/skimdb.h>

#include "skimdb_spir_matrix.h"
#include "skimdb_spir_definitions.h"


namespace skim {
namespace spir {

namespace fs = std::filesystem;

class spir_server_state {
public:
  explicit spir_server_state(skimdb_matrix&& DB,
                             skimdb_metadata&& metadata,
                             skimdb_parameters&& skim_config,
                             spirdb_parameters&& spir_config,
                             spir_matrix&& hint_c)
    : DB_{std::move(DB)},
      metadata_{std::move(metadata)},
      skim_config_{std::move(skim_config)},
      spir_config_{std::move(spir_config)},
      hint_c_{std::move(hint_c)} {}

  [[nodiscard]] auto skim_parameters() const -> const skimdb_parameters& { return skim_config_; }
  [[nodiscard]] auto skim_metadata() const -> const skimdb_metadata& { return metadata_; }

  [[nodiscard]] auto spir_parameters() const -> const spirdb_parameters& { return spir_config_; }
  [[nodiscard]] auto hint_c() const -> const spir_matrix& { return hint_c_; }

  [[nodiscard]] auto answer(const spir_matrix& query_vec) const -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = query_vec.dimensions();

    if (q_rows != spir_config_.sqrt_N || q_cols != 1) {
      return std::unexpected{"invalid query vector dimensions"};
    }

    return mat_vec(DB_, query_vec, spir_config_.log_p, spir_config_.log_q, spir_config_.rle_blocks);
  }

  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    std::ofstream os{path, std::ios::binary};
    if (!os) {
      return std::unexpected{"could not create file"};
    }

    try {
      cereal::BinaryOutputArchive archive{os};
      archive(
        DB_, metadata_.index, metadata_.labels, skim_config_.k, skim_config_.s, skim_config_.t,
        spir_config_.n, spir_config_.sigma, spir_config_.rle_blocks, spir_config_.sqrt_N, 
        spir_config_.log_p, spir_config_.log_q, spir_config_.seed, hint_c_
      );
    } catch (...) {
      return std::unexpected{"serialization failed"};
    }

    os.close();

    return fs::file_size(path);
  }

private:
  skimdb_matrix DB_;              // matrix representation of rle encodings
  skimdb_metadata metadata_;      // skimdb metadata (kmer index, labels)
  skimdb_parameters skim_config_; // skimdb index parameters

  spirdb_parameters spir_config_; // SPIR parameters
  spir_matrix hint_c_;            // precomputed D*A matrix for query processing
};


[[nodiscard]] auto make_server(skimdb&& db, unsigned int log_p, unsigned int log_q, std::size_t n, double sigma,
                               std::uint64_t seed = std::random_device{}())
    -> std::expected<spir_server_state, std::string> {
  LogFun lf{"make_server(...)"};

  if (log_p != 8 && log_p != 16) {
    return std::unexpected{"unsupported plaintext modulus"};
  }
  if (log_q > 64) {
    return std::unexpected{"unsupported ciphertext modulus"};
  }
  if (n == 0) {
    return std::unexpected{"invalid LWE dimension"};
  }
  if (sigma <= 0.0) {
    return std::unexpected{"invalid sigma parameter"};
  }

  auto [k, s, t] = db.parameters();
  auto db_parts = std::move(db).explode();

  std::uint64_t kmers = db_parts.data.size();
  std::uint64_t max_rle = 0;

  for (const auto& entry : db_parts.data) {
    max_rle = std::max(max_rle, entry.length());
  }

  if (kmers == 0 || max_rle == 0) {
    return std::unexpected{"empty skimdb index"};
  }

  // determine spir matrix dimension sqrt(N)
  std::uint64_t rle_blocks = (log_p == 8) ? 2 * max_rle : max_rle;
  std::uint64_t min_blocks = kmers * rle_blocks;
  double min_side = std::ceil(std::sqrt(static_cast<double>(min_blocks)));
  std::uint64_t rles_per_side = static_cast<std::uint64_t>(std::ceil(min_side / static_cast<double>(rle_blocks)));
  std::uint64_t sqrt_N = rles_per_side * rle_blocks;

  g_log->info("skimdb contains {} kmers, blocks per RLE {}", kmers, rle_blocks);
  g_log->info("spir matrix dimension sqrt(N) = {}", sqrt_N);

  // generate matrix A
  spir_common_rng_t rng{seed};

  spir_matrix A{sqrt_N, n, log_q};
  A.fill(rng);

  skimdb_parameters skim_conf{k, s, t};
  spirdb_parameters spir_conf{n, sigma, rle_blocks, sqrt_N, log_p, log_q, seed};

  skimdb_matrix DB{std::move(db_parts.data), log_p, rle_blocks, sqrt_N};
  skimdb_metadata metadata{std::move(db_parts.index), std::move(db_parts.labels)};

  // compute hint_c = DB * A
  auto hint_c = mat_mul(DB, A, log_q);

  return spir_server_state{std::move(DB),
                           std::move(metadata),
                           std::move(skim_conf),
                           std::move(spir_conf),
                           std::move(hint_c)};
}

[[nodiscard]] auto load_server(const fs::path& path) -> std::expected<spir_server_state, std::string> {
  std::ifstream is{path, std::ios::binary};
  if (!is) {
    return std::unexpected{"could not open file"};
  }

  try {
    cereal::BinaryInputArchive archive(is);

    skimdb_matrix DB;
    phmap::parallel_flat_hash_map<std::uint32_t, std::size_t> index;
    std::vector<std::string> labels;
    skimdb_parameters skim_conf;
    spirdb_parameters spir_conf;
    spir_matrix hint_c;

    archive(DB, index, labels, skim_conf.k, skim_conf.s, skim_conf.t,
            spir_conf.n, spir_conf.sigma, spir_conf.rle_blocks, spir_conf.sqrt_N,
            spir_conf.log_p, spir_conf.log_q, spir_conf.seed, hint_c);

    skimdb_metadata metadata{std::move(index), std::move(labels)};

    return spir_server_state{std::move(DB), std::move(metadata), std::move(skim_conf), std::move(spir_conf), std::move(hint_c)};
  } catch (...) {
    return std::unexpected{"deserialization failed"};
  }
}

class spir_client_state {
public:
  using rng_type = std::mt19937_64;

  explicit spir_client_state(skimdb_parameters skim_config, skimdb_metadata skim_metadata,
                             spirdb_parameters spir_config, spir_matrix hint_c,
                             std::uint64_t seed = std::random_device{}())
      : skim_config_{std::move(skim_config)},
        skim_metadata_{std::move(skim_metadata)},
        spir_config_{std::move(spir_config)},
        A_{spir_config_.sqrt_N, spir_config_.n, spir_config_.log_q},
        hint_c_{std::move(hint_c)},
        rng_{seed} {
    spir_common_rng_t rng{spir_config_.seed};
    A_.fill(rng);
  }

  auto get_sqrt_N() const -> std::uint64_t { return spir_config_.sqrt_N; }

  auto get_log_q() const -> std::uint64_t { return spir_config_.log_q; }


  [[nodiscard]] auto prepare_query(const std::string& str) -> std::expected<spirdb_query_state, std::string> {
    if (!detail::is_valid(str, skim_config_.k)) {
      return std::unexpected{"invalid kmer"};
    }

    auto kmer = detail::kmer_to_uint32(str);
    auto kmer_idx = std::min(kmer, detail::reverse_complement(kmer, skim_config_.k));

    auto it = skim_metadata_.index.find(kmer_idx);

    // TODO: is this correct way to reporting that kmer is missing?
    if (it == skim_metadata_.index.end()) {
      return std::unexpected{"kmer not found in index"};
    }

    std::uint64_t target_rle = static_cast<std::uint64_t>(it->second) * spir_config_.rle_blocks;

    std::uint64_t col_idx = target_rle / spir_config_.sqrt_N;
    std::uint64_t row_idx = target_rle % spir_config_.sqrt_N;

    spir_matrix s{spir_config_.n, spir_config_.log_q};
    s.fill(rng_);

    dgpp::uniform_rejection dist{spir_config_.sigma};

    spir_matrix e{spir_config_.sqrt_N, 1, spir_config_.log_q};
    e.fill(rng_, dist);

    std::uint64_t delta = 1ull << (spir_config_.log_q - spir_config_.log_p);

    // compute encrypted query vector
    auto res = mat_vec(A_, s, spir_config_.log_q);

    spir_matrix query = std::move(res);

    query.add(e);
    query.set(col_idx, query.get(col_idx) + delta);

    return spirdb_query_state{row_idx, std::move(s), std::move(query)};
  }


  [[nodiscard]] auto recover(const spir_matrix& ans, const spirdb_query_state& query)
      -> std::expected<spir_matrix, std::string> {
    auto [a_rows, a_cols] = ans.dimensions();
    auto [h_rows, h_cols] = hint_c_.dimensions();
    auto [s_rows, s_cols] = query.s_vec.dimensions();

    if (a_rows != h_rows || s_rows != h_cols || s_cols != 1 || a_cols != 1) {
      return std::unexpected{"dimension mismatch"};
    }

    if (query.i_row >= a_rows || spir_config_.rle_blocks > a_rows - query.i_row) {
      return std::unexpected{"invalid row range"};
    }

    spir_matrix out{spir_config_.rle_blocks, spir_config_.log_p};
    std::uint64_t shift = spir_config_.log_q - spir_config_.log_p;
    std::uint64_t half_delta = (shift == 0) ? 0ull : (1ull << (shift - 1));
    std::uint64_t q_mask = (spir_config_.log_q >= 64) ? ~0ull : ((1ull << spir_config_.log_q) - 1);

    for (std::uint64_t i = 0; i < spir_config_.rle_blocks; ++i) {
      std::uint64_t sum = 0;
      for (std::uint64_t j = 0; j < h_cols; ++j) {
        sum += hint_c_.get(i + query.i_row, j) * query.s_vec.get(j);
        sum &= q_mask;
      }

      std::uint64_t d = (ans.get(i + query.i_row) - sum) & q_mask;
      d = (d + half_delta) & q_mask;
      d >>= shift;
      out.set(i, d);
    }

    return out;
  }

  [[nodiscard]] auto result(const spir_matrix& mat) -> std::generator<const std::string&> {
    auto [rows, _] = mat.dimensions();

    if (rows != spir_config_.rle_blocks) {
      co_return;
    }

    std::vector<std::uint16_t> rle;

    switch (spir_config_.log_p) {
    case 8: {
      for (std::uint64_t i = 0; i < spir_config_.rle_blocks / 2; ++i) {
        std::uint16_t byte_pair = static_cast<std::uint16_t>(mat.get(i * 2)) << 8;
        byte_pair |= static_cast<std::uint16_t>(mat.get(i * 2 + 1));
        rle.push_back(byte_pair);
      }
      break;
    }
    case 16: {
      for (std::uint64_t i = 0; i < spir_config_.rle_blocks; ++i) {
        rle.push_back(static_cast<std::uint16_t>(mat.get(i)));
      }
      break;
    }
    default:
      co_return;
    }

    detail::encoding enc{std::move(rle)};

    for (auto idx : enc.select_idxs()) {
      if (idx >= skim_metadata_.labels.size()) {
        break;
      }
      co_yield skim_metadata_.labels[idx];
    }
  }

private:
  skimdb_parameters skim_config_; // skimdb index parameters
  skimdb_metadata skim_metadata_; // skimdb metadata (kmer index, labels)

  spirdb_parameters spir_config_; // SPIR parameters

  spir_matrix A_;      // matrix A
  spir_matrix hint_c_; // hint matrix from server

  rng_type rng_;
};

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_H
