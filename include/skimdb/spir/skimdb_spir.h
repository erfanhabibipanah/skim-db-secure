#ifndef SKIMDB_SPIR_H
#define SKIMDB_SPIR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <generator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <dgpp/uniform_rejection.hpp>

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/phmap_dump.h>

#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/skimdb.h>

#include "skimdb_spir_matrix.h"
#include "skimdb_spir_definitions.h"


namespace skim::spir {

namespace fs = std::filesystem;

class spir_server_state final {
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


  [[nodiscard]] auto answer(const spir_matrix& qu) const -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = qu.dimensions();

    if (q_rows != 1 || q_cols != spir_config_.sqrt_N) {
      return std::unexpected{"invalid query vector dimensions"};
    }

    return mat_vec(DB_, qu, spir_config_.log_q, spir_config_.rle_blocks);
  }

  [[nodiscard]] auto batch_answer(const spir_matrix& qu_mat) const -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = qu_mat.dimensions();

    if (q_rows != spir_config_.batch_size || q_cols != spir_config_.sqrt_N) {
      return std::unexpected{"invalid query matrix dimensions"};
    }

    spir_matrix ans{spir_config_.sqrt_N, spir_config_.log_q};

    std::uint32_t rles_per_col = spir_config_.sqrt_N / spir_config_.rle_blocks;
    std::uint32_t rles_per_batch = rles_per_col / spir_config_.batch_size;
    std::uint32_t remaining_rles = rles_per_col % spir_config_.batch_size;

    for (std::uint64_t i = 0; i < spir_config_.batch_size; ++i) {
      std::uint32_t start_idx = i * rles_per_batch;
      std::uint32_t count = rles_per_batch;

      if (i < remaining_rles) {
        start_idx += i;
        count += 1;
      } else {
        start_idx += remaining_rles;
      }

      partitioned_mat_vec(DB_, qu_mat.row(i), ans.span(), spir_config_.log_q, start_idx, count,
                          spir_config_.rle_blocks);
    }

    return ans;
  }


  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    std::ofstream os{path, std::ios::binary};
    if (!os) {
      return std::unexpected{"could not create file"};
    }

    try {
      cereal::BinaryOutputArchive archive{os};
      archive(DB_, metadata_.index, metadata_.labels, skim_config_.k, skim_config_.s, skim_config_.t,
              spir_config_.n, spir_config_.sigma, spir_config_.log_p, spir_config_.log_q, spir_config_.batch_size,
              spir_config_.block_len, spir_config_.rle_blocks, spir_config_.sqrt_N, spir_config_.seed, hint_c_);
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
                               std::uint32_t batch_size = 1, std::uint64_t seed = std::random_device{}())
    -> std::expected<spir_server_state, std::string> {
  LogFun lf{"make_server(...)"};

  if (log_p < 8 || log_p >= 32) {
    return std::unexpected{"unsupported plaintext modulus"};
  }
  if (log_q < 32 || log_q > 64) {
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

  std::uint32_t block_len = log_p / 8;
  std::uint64_t rle_blocks = 2 * max_rle / block_len + ((2 * max_rle % block_len) ? 1 : 0);
  std::uint64_t min_blocks = kmers * rle_blocks;

  g_log->info("skimdb contains {} kmers, require {} blocks per RLE", kmers, rle_blocks);

  double min_side = std::ceil(std::sqrt(static_cast<double>(min_blocks)));
  auto rles_per_side = static_cast<std::uint64_t>(std::ceil(min_side / static_cast<double>(rle_blocks)));
  std::uint64_t sqrt_N = rles_per_side * rle_blocks;

  g_log->info("spir matrix dimension sqrt(N) = {}", sqrt_N);

  spir_common_rng_t rng{seed};

  spir_matrix A{sqrt_N, n, log_q};
  A.fill(rng);

  skimdb_parameters skim_conf{.k = k, .s = s, .t = t};

  spirdb_parameters spir_conf{.n = n,
                              .sigma = sigma,
                              .log_p = log_p,
                              .log_q = log_q,
                              .batch_size = batch_size,
                              .block_len = block_len,
                              .rle_blocks = rle_blocks,
                              .sqrt_N = sqrt_N,
                              .seed = seed};

  skimdb_matrix DB{std::move(db_parts.data), block_len, rle_blocks, sqrt_N};
  skimdb_metadata metadata{.index = std::move(db_parts.index), .labels = std::move(db_parts.labels)};

  // compute hint_c = DB * A
  g_log->info("precomputing hint matrix DB * A...");
  auto hint_c = mat_mul(DB, A, log_q, rle_blocks);

  return spir_server_state{std::move(DB), std::move(metadata), std::move(skim_conf), std::move(spir_conf), std::move(hint_c)};
}


[[nodiscard]] auto load_server(const fs::path& path) -> std::expected<spir_server_state, std::string> {
  LogFun lf{"load_server(...)"};

  std::ifstream is{path, std::ios::binary};
  if (!is) {
    return std::unexpected{"could not open file"};
  }

  try {
    cereal::BinaryInputArchive archive(is);

    skimdb_matrix DB;
    phmap::parallel_flat_hash_map<std::uint32_t, std::uint64_t> index;
    std::vector<std::string> labels;
    skimdb_parameters skim_conf{};
    spirdb_parameters spir_conf{};
    spir_matrix hint_c;

    archive(DB, index, labels, skim_conf.k, skim_conf.s, skim_conf.t, spir_conf.n, spir_conf.sigma, spir_conf.log_p,
            spir_conf.log_q, spir_conf.batch_size, spir_conf.block_len, spir_conf.rle_blocks, spir_conf.sqrt_N,
            spir_conf.seed, hint_c);

    skimdb_metadata metadata{.index = std::move(index), .labels = std::move(labels)};

    g_log->info("serever state loaded with spir paramaters sqrt_N: {}, log_p: {}, log_q: {}, n: {}, and sigma: {}",
                spir_conf.sqrt_N, spir_conf.log_p, spir_conf.log_q, spir_conf.n, spir_conf.sigma);

    return spir_server_state{std::move(DB),
                             std::move(metadata),
                             std::move(skim_conf),
                             std::move(spir_conf),
                             std::move(hint_c)};
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


  [[nodiscard]] auto skim_parameters() const -> skimdb_parameters { return skim_config_; }

  [[nodiscard]] auto spir_parameters() const -> spirdb_parameters { return spir_config_; }


  [[nodiscard]] auto is_valid_kmer(const std::string& str) const -> bool {
    auto kmer = detail::kmer_to_uint32(str);
    return detail::is_valid(str, skim_config_.k) && detail::is_syncmer(kmer, skim_config_.k, skim_config_.s, skim_config_.t);
  }

  [[nodiscard]] auto kmer_to_position(const std::string& str) const -> std::expected<std::pair<std::uint64_t, std::uint64_t>, std::string> {
    LogFun lf{"spir_client_state::kmer_to_position(...)"};

    auto kmer = detail::kmer_to_uint32(str);
    auto iter = skim_metadata_.index.find(kmer);

    if (iter == skim_metadata_.index.end()) {
      return std::unexpected{
        // If the k‑mer is valid but absent from the skimdb index, the k‑mer has no associated labels
        std::format("kmer {} not found in skimdb index", str)
      };
    }

    std::uint64_t target_rle = static_cast<std::uint64_t>(iter->second) * spir_config_.rle_blocks;
    std::uint64_t i_col = target_rle / spir_config_.sqrt_N;
    std::uint64_t i_row = target_rle % spir_config_.sqrt_N;

    return std::make_pair(i_row, i_col);
  }

  [[nodiscard]] auto row_to_partition(std::uint64_t i_row) const -> std::uint64_t {
    std::uint64_t rle_idx = i_row / spir_config_.rle_blocks;
    std::uint64_t rles_per_col = spir_config_.sqrt_N / spir_config_.rle_blocks;
    std::uint64_t rles_per_batch = rles_per_col / spir_config_.batch_size;
    std::uint64_t remaining_rles = rles_per_col % spir_config_.batch_size;

    if (rle_idx < remaining_rles * (rles_per_batch + 1)) {
      return rle_idx / (rles_per_batch + 1);
    } else {
      return (rle_idx - remaining_rles * (rles_per_batch + 1)) / rles_per_batch + remaining_rles;
    }
  }


  [[nodiscard]] auto prepare_query(std::uint64_t i_col) -> spirdb_query_state {
    LogFun lf{"spir_client_state::prepare_query(...)"};

    spir_matrix s{spir_config_.n, spir_config_.log_q};
    s.fill(rng_);

    dgpp::uniform_rejection dist{spir_config_.sigma};
    spir_matrix e{spir_config_.sqrt_N, spir_config_.log_q};
    e.fill(rng_, dist);

    std::uint64_t delta = 1ull << (spir_config_.log_q - spir_config_.log_p);

    auto qu = mat_vec(A_, s, spir_config_.log_q);
    qu.add(e);
    qu.set(i_col, qu.get(i_col) + delta);

    return spirdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }


  [[nodiscard]] auto new_batch() -> spirdb_query_state {
    LogFun lf{"spir_client_state::new_batch(...)"};

    spir_matrix s{spir_config_.batch_size, spir_config_.n, spir_config_.log_q};
    s.fill(rng_);

    dgpp::uniform_rejection dist{spir_config_.sigma};
    spir_matrix e{spir_config_.batch_size, spir_config_.sqrt_N, spir_config_.log_q};
    e.fill(rng_, dist);

    spir_matrix qu{spir_config_.batch_size, spir_config_.sqrt_N, spir_config_.log_q};

    for (std::size_t i = 0; i < spir_config_.batch_size; ++i) {
      mat_vec(A_, s.row(i), qu.row(i), spir_config_.log_q);
    }
    qu.add(e);

    return spirdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }


  void update_batch(spirdb_query_state& batch_state, std::uint64_t i_batch, std::uint64_t i_col) {
    LogFun lf{"spir_client_state::update_batch(...)", spdlog::level::trace};

    std::uint64_t delta = 1ull << (spir_config_.log_q - spir_config_.log_p);
    batch_state.qu_vec.set(i_batch, i_col, batch_state.qu_vec.get(i_batch, i_col) + delta);
  }


  [[nodiscard]] auto result(const spir_matrix& ans, const spirdb_query_state& query, std::uint64_t i_row)
      -> std::generator<const std::string&> {
    LogFun lf{"spir_client_state::result(...)"};

    auto d = sub_mat_vec_rows(ans, hint_c_, query.s_vec.span(), spir_config_.log_q, i_row, spir_config_.rle_blocks);
    d.div_delta(spir_config_.log_q - spir_config_.log_p);
    auto d_data = d.span();

    auto rle = m_recover_(d_data);
    for (auto idx : rle.select_idxs()) {
      if (idx >= skim_metadata_.labels.size()) {
        break;
      }
      co_yield skim_metadata_.labels[idx];
    }
  }

private:
  auto m_recover_(std::span<const std::uint64_t> d_data) -> detail::encoding {
    std::vector<std::uint16_t> rle_data;

    switch (spir_config_.block_len) {
    case 1: {
      std::uint64_t out_len = spir_config_.rle_blocks / 2;
      rle_data.resize(out_len);
      auto* dst = reinterpret_cast<std::uint8_t*>(rle_data.data());

#pragma omp parallel for simd schedule(static)
      for (std::uint64_t i = 0; i < spir_config_.rle_blocks; ++i) {
        dst[i] = static_cast<std::uint8_t>(d_data[i] & 0xFFull);
      }

      break;
    }
    case 2: {
      std::uint64_t out_len = spir_config_.rle_blocks;
      rle_data.resize(out_len);
      std::uint16_t* dst = rle_data.data();

#pragma omp parallel for simd schedule(static)
      for (std::uint64_t i = 0; i < out_len; ++i) {
        dst[i] = static_cast<std::uint16_t>(d_data[i] & 0xFFFFull);
      }

      break;
    }
    case 3: {
      std::uint64_t out_len = spir_config_.rle_blocks * 3 / 2 + ((spir_config_.rle_blocks * 3 % 2) ? 1 : 0);
      rle_data.resize(out_len);
      auto* dst = reinterpret_cast<std::uint8_t*>(rle_data.data());

#pragma omp parallel for simd schedule(static)
      for (std::uint64_t i = 0; i < spir_config_.rle_blocks; ++i) {
        dst[i * 3] = static_cast<std::uint8_t>((d_data[i] >> 16) & 0xFFull);
        dst[i * 3 + 1] = static_cast<std::uint8_t>((d_data[i] >> 8) & 0xFFull);
        dst[i * 3 + 2] = static_cast<std::uint8_t>(d_data[i] & 0xFFull);
      }

      break;
    }
    default: {
      // should never reach here since prepare_query would fail for unsupported log_p
      g_log->error("unsupported log_p value in recover: {}", spir_config_.log_p);
      break;
    }
    }

    return detail::encoding{std::move(rle_data)};
  }

  skimdb_parameters skim_config_; // skimdb index parameters
  skimdb_metadata skim_metadata_; // skimdb metadata (kmer index, labels)

  spirdb_parameters spir_config_; // SPIR parameters

  spir_matrix A_;      // matrix A
  spir_matrix hint_c_; // hint matrix from server

  rng_type rng_;
};

} // namespace skim::spir

#endif // SKIMDB_SPIR_H
