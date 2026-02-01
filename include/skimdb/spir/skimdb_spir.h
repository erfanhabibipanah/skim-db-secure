#ifndef SKIMDB_SPIR_H
#define SKIMDB_SPIR_H

#include <cmath>
#include <cstdint>
#include <expected>
#include <generator>
#include <string>
#include <tuple>
#include <vector>

#ifndef DGPP_UNIFORM_REJECTION_HPP
#define DGPP_UNIFORM_REJECTION_HPP
#include <dgpp/uniform_rejection.hpp>
#endif

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include <parallel_hashmap/phmap.h>

#include "skimdb/detail/skimdb_encoding.h"
#include "skimdb/detail/skimdb_logger.h"
#include "skimdb/detail/skimdb_util.h"
#include "skimdb/skimdb.h"
#include "skimdb_spir_matrix.h"
#include "skimdb_spir_params.h"
#include "skimdb_spir_random.h"

namespace skim {
namespace spir {

class spir_server_state {
public:
  explicit spir_server_state(skimdb_matrix db_mat,
                              spir_matrix hint_c,
                              db_config config,
                              db_metadata metadata,
                              spir_params params)
    : db_mat_{std::move(db_mat)},
      hint_c_{std::move(hint_c)},
      config_{std::move(config)},
      metadata_{std::move(metadata)},
      params_{std::move(params)} {}
  
  [[nodiscard]] auto get_hint_c() const -> const spir_matrix& { return hint_c_; }
  [[nodiscard]] auto get_config() const -> const db_config&   { return config_; }
  [[nodiscard]] auto get_metadata() const -> const db_metadata& { return metadata_; }
  [[nodiscard]] auto get_params() const -> const spir_params& { return params_; }

  [[nodiscard]] auto answer(const spir_matrix& query_vec) const 
      -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = query_vec.dimensions();
    if (q_rows != params_.sqrt_N || q_cols != 1) {
      return std::unexpected{"invalid query vector dimensions"};
    }

    return mat_vec(db_mat_, query_vec, params_.log_q);
  }
    
private:
  skimdb_matrix db_mat_;      // matrix representation of rle encodings 
  spir_matrix hint_c_;        // precomputed D*A matrix for query processing

  db_config config_;          // skimdb index parameters
  db_metadata metadata_;      // skimdb metadata (kmer index, labels)
  spir_params params_;        // SPIR parameters
};

[[nodiscard]] auto setup_server(skimdb&& db, std::uint64_t log_p, std::uint64_t log_q, std::uint64_t n, double sigma)
    -> std::expected<spir_server_state, std::string> {
  LogFun lf{"setup_server(...)"};
  if (log_p != 8 && log_p != 16) { return std::unexpected{"unsupported plaintext modulus"}; }
  if (log_q > 64) { return std::unexpected{"unsupported ciphertext modulus"}; }
  if (n == 0) { return std::unexpected{"invalid LWE dimension"}; }
  if (sigma <= 0.0) { return std::unexpected{"invalid sigma parameter"}; }

  auto [k, s, t] = db.parameters();
  auto db_parts = std::move(db).take_parts();

  std::uint64_t kmers = db_parts.data.size();
  std::uint64_t max_rle = 0;
  for (const auto& entry : db_parts.data) {
    max_rle = std::max(max_rle, entry.length());
  }

  g_log->info("skimdb contains {} kmers, max RLE length {}", kmers, max_rle);

  if (kmers == 0 || max_rle == 0) { return std::unexpected{"empty skimdb index"}; }
  
  // determine spir matrix dimension sqrt(N)
  std::uint64_t rle_blocks = (log_p == 8) ?  2 * max_rle : max_rle;
  std::uint64_t min_blocks = kmers * rle_blocks;
  double min_side = std::ceil(std::sqrt(static_cast<double>(min_blocks)));
  std::uint64_t rles_per_side = static_cast<std::uint64_t>(std::ceil(min_side / static_cast<double>(rle_blocks)));
  std::uint64_t sqrt_N = rles_per_side * rle_blocks;

  g_log->info("SPIR matrix dimension sqrt(N) = {}", sqrt_N);

  // generate matrix A
  std::uint64_t mat_seed = new_seed();
  std::mt19937_64 rng{mat_seed};

  spir_matrix mat_a{sqrt_N, n, log_q};
  mat_a.fill_random(rng); 

  db_config config{k, s, t};
  spir_params params{n, sigma, rle_blocks, sqrt_N, log_p, log_q, mat_seed};
  skimdb_matrix db_mat{std::move(db_parts.data), log_p, rle_blocks, sqrt_N};
  db_metadata metadata{std::move(db_parts.index), std::move(db_parts.labels)};

  // compute hint_c = D * A
  auto hint_c = mat_mul(db_mat, mat_a, log_q);
  if (!hint_c) {
    return std::unexpected{hint_c.error()};
  }

  return spir_server_state{std::move(db_mat), std::move(*hint_c),
                          std::move(config), std::move(metadata),
                          std::move(params)};
}

class spir_client_state {
public:
  explicit spir_client_state(spir_matrix hint_c,
                              db_config config,
                              db_metadata metadata,
                              spir_params params)
    : 
      hint_c_{std::move(hint_c)},
      config_{std::move(config)},
      metadata_{std::move(metadata)},
      params_{std::move(params)},
      mat_a_{params_.sqrt_N, params_.n, params_.log_q} {
        // populate matrix A
        std::mt19937_64 rng{params_.mat_seed};
        mat_a_.fill_random(rng);
      }

  [[nodiscard]] auto new_query(const std::string& str) -> std::expected<query_state, std::string> {
    if (!detail::is_valid(str, config_.k)) {
      return std::unexpected{"invalid kmer"};
    }

    auto kmer = detail::kmer_to_uint32(str);
    auto kmer_idx = std::min(kmer, detail::reverse_complement(kmer, config_.k));

    auto it = metadata_.index.find(kmer_idx);
    if (it == metadata_.index.end()) {
      return std::unexpected{"kmer not found in index"};
    }

    std::uint64_t target_rle = static_cast<std::uint64_t>(it->second) * params_.rle_blocks;
    std::uint64_t col_idx = target_rle / params_.sqrt_N;
    std::uint64_t row_idx = target_rle % params_.sqrt_N;

    std::mt19937_64 rng{new_seed()};
    spir_matrix s{params_.n, params_.log_q};
    s.fill_random(rng);
    
    std::mt19937_64 erng{new_seed()};
    dgpp::uniform_rejection dist{params_.sigma};
    spir_matrix e{params_.sqrt_N, 1, params_.log_q};
    e.fill_random(erng, dist);

    std::uint64_t delta = 1ull << (params_.log_q - params_.log_p);
    
    // compute encrypted query vector
    auto res = mat_mul(mat_a_, s, params_.log_q);
    if (!res) {
      return std::unexpected{res.error()};
    }

    spir_matrix query = std::move(*res);
    query.add(e);
    query.set(col_idx, query.get(col_idx) + delta);

    return query_state{row_idx, std::move(s), std::move(query)};
  }

  [[nodiscard]] auto recover(const spir_matrix& ans, const query_state& query) 
      -> std::expected<spir_matrix, std::string> {
    auto [a_rows, a_cols] = ans.dimensions();
    auto [h_rows, h_cols] = hint_c_.dimensions();
    auto [s_rows, s_cols] = query.secret_vec.dimensions();

    if (a_rows != h_rows || s_rows != h_cols || s_cols != 1 || a_cols != 1) {
      return std::unexpected{"dimension mismatch"};
    }

    if (query.i_row >= a_rows || params_.rle_blocks > a_rows - query.i_row) {
      return std::unexpected{"invalid row range"};
    }

    spir_matrix out{params_.rle_blocks, params_.log_p};
    std::uint64_t shift = params_.log_q - params_.log_p;
    std::uint64_t half_delta = (shift == 0) ? 0ull : (1ull << (shift - 1));
    std::uint64_t q_mask = (params_.log_q >= 64) ? ~0ull : ((1ull << params_.log_q) - 1);
    for (std::uint64_t i = 0; i < params_.rle_blocks; ++i) {
      std::uint64_t sum = 0;
      for (std::uint64_t j = 0; j < h_cols; ++j) {
        sum += hint_c_.get(i + query.i_row, j) * query.secret_vec.get(j);
        sum &= q_mask;
      }

      std::uint64_t d = (ans.get(i + query.i_row) - sum) & q_mask;
      d = (d + half_delta) & q_mask;
      d >>= shift;
      out.set(i, d);
    }

    return out;
  }

  [[nodiscard]] auto result(spir_matrix mat) -> std::generator<const std::string&> {
    auto [rows, _] = mat.dimensions();
    if (rows != params_.rle_blocks) {
      co_return;
    }

    std::vector<std::uint16_t> rle;
    
    switch (params_.log_p) {
      case 8: {
        for (std::uint64_t i = 0; i < params_.rle_blocks / 2; ++i) {
          std::uint16_t byte_pair = static_cast<std::uint16_t>(mat.get(i * 2)) << 8;
          byte_pair |= static_cast<std::uint16_t>(mat.get(i * 2 + 1));
          rle.push_back(byte_pair);
        }
        break;
      }
      case 16: {
        for (std::uint64_t i = 0; i < params_.rle_blocks; ++i) {
          rle.push_back(static_cast<std::uint16_t>(mat.get(i)));
        }
        break;
      }
      default:
        co_return;
    }

    detail::encoding enc{std::move(rle)};
    for (auto idx : enc.select_idxs()) {
      if (idx >= metadata_.labels.size()) { break; }
      co_yield metadata_.labels[idx];
    }
  }

private:
  db_config config_;          // skimdb index parameters
  db_metadata metadata_;      // skimdb metadata (kmer index, labels)
  spir_params params_;        // SPIR parameters

  spir_matrix mat_a_;         // matrix A
  spir_matrix hint_c_;        // hint matrix from server
};

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_H
