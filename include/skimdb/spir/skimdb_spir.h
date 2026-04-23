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
#include <system_error>
#include <utility>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <dgpp/uniform_rejection.hpp>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/detail/skimdb_util.h>
#include <skimdb/skimdb.h>

#include "skimdb_spir_matrix.h"
#include "skimdb_spir_definitions.h"
#include "skimdb_spir_util.h"


namespace skim::spir {

namespace fs = std::filesystem;


struct spir_runtime_config {
  unsigned int grpc_connect_timeout{5};             // gRPC connection timeout (seconds)
  unsigned int grpc_download_timeout{300};          // gRPC data download timeout (seconds)
  std::string server_store_dir{".skimdb-server"};   // path to directory where server stores hint data
  std::string client_metadata_dir{".skimdb-cache"}; // path to directory to store metadata on client's side
  std::string client_hint_c_dir{".skimdb-cache"};   // path to directory to store hint_c on client's side
};

spir_runtime_config g_spir_config;


class spir_server_state final {
public:
  explicit spir_server_state(skimdb_matrix&& DB, skimdb_parameters&& skim_config, spirdb_parameters&& spir_config)
      : DB_{std::move(DB)}, skim_config_{std::move(skim_config)}, spir_config_{std::move(spir_config)} {}


  [[nodiscard]] auto skim_parameters() const -> const skimdb_parameters& { return skim_config_; }

  [[nodiscard]] auto spir_parameters() const -> const spirdb_parameters& { return spir_config_; }


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

    std::size_t rles_per_col = spir_config_.sqrt_N / spir_config_.rle_blocks;
    std::size_t rles_per_batch = rles_per_col / spir_config_.batch_size;
    std::size_t remaining_rles = rles_per_col % spir_config_.batch_size;

    for (std::size_t i = 0; i < spir_config_.batch_size; ++i) {
      std::size_t start_idx = i * rles_per_batch;
      std::size_t count = rles_per_batch;

      if (i < remaining_rles) {
        start_idx += i;
        count += 1;
      } else {
        start_idx += remaining_rles;
      }

      partitioned_mat_vec(
          DB_, qu_mat.row(i), ans.span(), spir_config_.log_q, start_idx, count, spir_config_.rle_blocks);
    }

    return ans;
  }


  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    std::ofstream of{path, std::ios::binary};

    if (!of) {
      return std::unexpected{"could not create file"};
    }

    try {
      cereal::BinaryOutputArchive archive{of};
      archive(DB_,
              skim_config_.k,
              skim_config_.s,
              skim_config_.t,
              spir_config_.n,
              spir_config_.sigma,
              spir_config_.log_p,
              spir_config_.log_q,
              spir_config_.batch_size,
              spir_config_.block_size,
              spir_config_.rle_blocks,
              spir_config_.sqrt_N,
              spir_config_.seed,
              spir_config_.metadata_hash,
              spir_config_.hint_c_hash);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("serialization failed {}", e.what())};
    }

    of.close();

    return fs::file_size(path);
  }

private:
  skimdb_matrix DB_;              // matrix representation of rle encodings
  skimdb_parameters skim_config_; // skimdb index parameters
  spirdb_parameters spir_config_; // SPIR parameters
};


[[nodiscard]] auto make_server(skimdb&& db,
                               std::size_t log_p,
                               std::size_t log_q,
                               std::size_t n,
                               double sigma,
                               std::size_t batch_size = 1,
                               std::uint64_t seed = std::random_device{}())
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

  auto kmers = db_parts.data.size();
  std::size_t max_rle = 0;

  for (const auto& entry : db_parts.data) {
    max_rle = std::max(max_rle, entry.length());
  }

  if (kmers == 0 || max_rle == 0) {
    return std::unexpected{"empty skimdb index"};
  }

  std::size_t block_size = log_p / 8;
  std::size_t rle_blocks = 2 * max_rle / block_size + ((2 * max_rle % block_size) ? 1 : 0);
  std::size_t min_blocks = kmers * rle_blocks;

  g_log->info("skimdb contains {} kmers, requires {} block(s) per RLE", kmers, rle_blocks);

  double min_side = std::ceil(std::sqrt(static_cast<double>(min_blocks)));
  auto rles_per_side = static_cast<std::size_t>(std::ceil(min_side / static_cast<double>(rle_blocks)));

  std::size_t sqrt_N = rles_per_side * rle_blocks;

  skimdb_matrix DB{std::move(db_parts.data), block_size, rle_blocks, sqrt_N};

  g_log->info("spir matrix dimension sqrt(N) = {}", sqrt_N);

  spir_common_rng_t rng{seed};

  spir_matrix A{sqrt_N, n, log_q};
  A.fill(rng);

  // compute hint_c = DB * A
  g_log->info("precomputing hint matrix DB * A, be patient...");
  auto hint_c = mat_mul(DB, A, log_q, rle_blocks);

  g_log->info("saving client metadata...");
  std::string metadata_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_spir_config.server_store_dir) / rand_name;

    {
      std::ofstream os{temp_metadata_path, std::ios::binary};
      if (!os) {
        return std::unexpected{"could not create client metadata"};
      }

      cereal::BinaryOutputArchive archive{os};
      archive(db_parts.index, db_parts.labels);
    }

    auto hash_res = sha256_file(temp_metadata_path);

    if (!hash_res) {
      return std::unexpected{hash_res.error()};
    }

    metadata_hash = hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_spir_config.server_store_dir) / metadata_hash, ec);
    if (ec) {
      return std::unexpected{"could not rename client metadata"};
    }

    g_log->info("client metadata generated with hash {}", metadata_hash);
  }

  g_log->info("saving hint_c...");
  std::string hint_c_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_spir_config.server_store_dir) / rand_name;

    {
      std::ofstream os{temp_metadata_path, std::ios::binary};
      if (!os) {
        return std::unexpected{"could not create hint_c"};
      }

      cereal::BinaryOutputArchive archive{os};
      archive(hint_c);
    }

    auto hash_res = sha256_file(temp_metadata_path);

    if (!hash_res) {
      return std::unexpected{hash_res.error()};
    }

    hint_c_hash = hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_spir_config.server_store_dir) / hint_c_hash, ec);

    if (ec) {
      return std::unexpected{"could not rename hint_c"};
    }

    g_log->info("hint_c generated with hash {}", hint_c_hash);
  }

  g_log->info("constructing server state...");

  skimdb_parameters skim_conf{.k = k, .s = s, .t = t};

  spirdb_parameters spir_conf{.n = n,
                              .sigma = sigma,
                              .log_p = log_p,
                              .log_q = log_q,
                              .batch_size = batch_size,
                              .block_size = block_size,
                              .rle_blocks = rle_blocks,
                              .sqrt_N = sqrt_N,
                              .seed = seed,
                              .metadata_hash = metadata_hash,
                              .hint_c_hash = hint_c_hash};

  return spir_server_state{std::move(DB), std::move(skim_conf), std::move(spir_conf)};
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
    skimdb_parameters skim_conf{};
    spirdb_parameters spir_conf{};

    archive(DB,
            skim_conf.k,
            skim_conf.s,
            skim_conf.t,
            spir_conf.n,
            spir_conf.sigma,
            spir_conf.log_p,
            spir_conf.log_q,
            spir_conf.batch_size,
            spir_conf.block_size,
            spir_conf.rle_blocks,
            spir_conf.sqrt_N,
            spir_conf.seed,
            spir_conf.metadata_hash,
            spir_conf.hint_c_hash);

    g_log->info("server state loaded, (sqrt_N={}, log_p={}, log_q={}, n={}, sigma={})",
                spir_conf.sqrt_N,
                spir_conf.log_p,
                spir_conf.log_q,
                spir_conf.n,
                spir_conf.sigma);

    return spir_server_state{std::move(DB), std::move(skim_conf), std::move(spir_conf)};
  } catch (...) {
    return std::unexpected{"deserialization failed"};
  }
}


class spir_client_state {
public:
  using rng_type = spir_common_rng_t;

  explicit spir_client_state(skimdb_parameters skim_config, skimdb_metadata skim_metadata,
                             spirdb_parameters spir_config, spir_matrix hint_c,
                             std::uint64_t seed = std::random_device{}())
    : skim_config_{std::move(skim_config)},
      skim_metadata_{std::move(skim_metadata)},
      spir_config_{std::move(spir_config)},
      A_{spir_config_.sqrt_N, spir_config_.n, spir_config_.log_q},
      hint_c_{std::move(hint_c)},
      main_seed_{seed} {
    spir_common_rng_t rng{spir_config_.seed};
    A_.fill(rng);
  }


  [[nodiscard]] auto skim_parameters() const -> skimdb_parameters { return skim_config_; }

  [[nodiscard]] auto spir_parameters() const -> spirdb_parameters { return spir_config_; }


  [[nodiscard]] auto is_valid_kmer(const std::string& str) const -> bool {
    auto kmer = detail::kmer_to_binary(str);
    return detail::is_valid(str, skim_config_.k) &&
           detail::is_syncmer(kmer, skim_config_.k, skim_config_.s, skim_config_.t);
  }

  [[nodiscard]] auto kmer_to_position(const std::string& s) const
      -> std::optional<std::pair<std::size_t, std::size_t>> {
    LogFun lf{"spir_client_state::kmer_to_position(...)"};

    auto kmer = detail::kmer_to_binary(s);
    auto res = skim_metadata_.index.find(kmer);

    if (!res.has_value()) {
      // if the k‑mer is valid but absent from the skimdb index, the k‑mer has no associated labels
      return std::nullopt;
    }

    auto pos = res.value();

    std::size_t target_rle = pos * spir_config_.rle_blocks;
    std::size_t i_col = target_rle / spir_config_.sqrt_N;
    std::size_t i_row = target_rle % spir_config_.sqrt_N;

    return std::make_pair(i_row, i_col);
  }

  [[nodiscard]] auto row_to_partition(std::size_t i_row) const -> std::size_t {
    std::size_t rle_idx = i_row / spir_config_.rle_blocks;
    std::size_t rles_per_col = spir_config_.sqrt_N / spir_config_.rle_blocks;
    std::size_t rles_per_batch = rles_per_col / spir_config_.batch_size;
    std::size_t remaining_rles = rles_per_col % spir_config_.batch_size;

    if (rle_idx < remaining_rles * (rles_per_batch + 1)) {
      return rle_idx / (rles_per_batch + 1);
    } else {
      return (rle_idx - remaining_rles * (rles_per_batch + 1)) / rles_per_batch + remaining_rles;
    }
  }


  [[nodiscard]] auto prepare_query(std::size_t i_col) -> spirdb_query_state {
    LogFun lf{"spir_client_state::prepare_query(...)"};

    auto& rng = m_get_rng_();

    spir_matrix s{spir_config_.n, spir_config_.log_q};
    s.fill(rng);

    dgpp::uniform_rejection dist{spir_config_.sigma};
    spir_matrix e{spir_config_.sqrt_N, spir_config_.log_q};
    e.fill(rng, dist);

    std::size_t delta = 1ull << (spir_config_.log_q - spir_config_.log_p);

    auto qu = mat_vec(A_, s, spir_config_.log_q);
    qu.add(e);
    qu.set(i_col, qu.get(i_col) + delta);

    return spirdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }


  [[nodiscard]] auto new_batch() -> spirdb_query_state {
    LogFun lf{"spir_client_state::new_batch(...)"};

    auto& rng = m_get_rng_();

    spir_matrix s{spir_config_.batch_size, spir_config_.n, spir_config_.log_q};
    s.fill(rng);

    dgpp::uniform_rejection dist{spir_config_.sigma};
    spir_matrix e{spir_config_.batch_size, spir_config_.sqrt_N, spir_config_.log_q};
    e.fill(rng, dist);

    spir_matrix qu{spir_config_.batch_size, spir_config_.sqrt_N, spir_config_.log_q};

    for (std::size_t i = 0; i < spir_config_.batch_size; ++i) {
      mat_vec(A_, s.row(i), qu.row(i), spir_config_.log_q);
    }
    qu.add(e);

    return spirdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }


  void update_batch(spirdb_query_state& batch_state, std::size_t i_batch, std::size_t i_col) {
    LogFun lf{"spir_client_state::update_batch(...)", spdlog::level::trace};

    std::uint64_t delta = 1ull << (spir_config_.log_q - spir_config_.log_p);
    batch_state.qu_vec.set(i_batch, i_col, batch_state.qu_vec.get(i_batch, i_col) + delta);
  }


  [[nodiscard]] auto result(const spir_matrix& ans, const spirdb_query_state& query, std::size_t i_row)
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
  auto m_make_local_seed_() -> std::uint64_t {
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return main_seed_ ^ (tid * 0x9e3779b97f4a7c15ULL); // Fibonacci hashing
  }

  auto m_get_rng_() -> spir_common_rng_t& {
    thread_local spir_common_rng_t rng(m_make_local_seed_());
    return rng;
  }

  auto m_recover_(std::span<const std::uint64_t> d_data) -> detail::encoding {
    std::vector<std::uint16_t> rle_data;

    switch (spir_config_.block_size) {
    case 1: {
      std::size_t out_len = spir_config_.rle_blocks / 2;

      rle_data.resize(out_len);
      auto* dst = reinterpret_cast<std::uint8_t*>(rle_data.data());

      for (std::size_t i = 0; i < spir_config_.rle_blocks; ++i) {
        dst[i] = static_cast<std::uint8_t>(d_data[i] & 0xFFull);
      }

      break;
    }
    case 2: {
      std::size_t out_len = spir_config_.rle_blocks;

      rle_data.resize(out_len);
      std::uint16_t* dst = rle_data.data();

      for (std::size_t i = 0; i < out_len; ++i) {
        dst[i] = static_cast<std::uint16_t>(d_data[i] & 0xFFFFull);
      }

      break;
    }
    case 3: {
      std::size_t out_len = spir_config_.rle_blocks * 3 / 2 + ((spir_config_.rle_blocks * 3 % 2) ? 1 : 0);

      rle_data.resize(out_len);
      auto* dst = reinterpret_cast<std::uint8_t*>(rle_data.data());

      for (std::size_t i = 0; i < spir_config_.rle_blocks; ++i) {
        dst[i * 3 + 0] = static_cast<std::uint8_t>((d_data[i] >> 16) & 0xFFull);
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

  std::uint64_t main_seed_;
};

[[nodiscard]] auto load_client(skimdb_parameters skim_config,
                               spirdb_parameters spir_config,
                               std::uint64_t seed = std::random_device{}())
    -> std::expected<spir_client_state, std::string> {
  LogFun lf{"load_client(...)"};

  fs::path metadata_path = fs::path(g_spir_config.client_metadata_dir) / spir_config.metadata_hash;
  fs::path hint_c_path = fs::path(g_spir_config.client_hint_c_dir) / spir_config.hint_c_hash;

  g_log->debug("loading client metadata from {}...", metadata_path.string());

  skimdb_metadata skim_metadata;

  {
    std::ifstream is{metadata_path, std::ios::binary};
    if (!is) {
      return std::unexpected{"could not open metadata file"};
    }

    try {
      cereal::BinaryInputArchive archive(is);

      skimdb::kmer_index index;
      std::vector<std::string> labels;

      archive(index, labels);

      skim_metadata = skimdb_metadata{.index = std::move(index), .labels = std::move(labels)};
    } catch (...) {
      return std::unexpected{"deserialization failed"};
    }
  }

  g_log->debug("loading hint_c from {}...", hint_c_path.string());

  spir_matrix hint_c;

  {
    std::ifstream is{hint_c_path, std::ios::binary};
    if (!is) {
      return std::unexpected{"could not open hint_c file"};
    }

    try {
      cereal::BinaryInputArchive archive(is);
      archive(hint_c);
    } catch (...) {
      return std::unexpected{"deserialization failed"};
    }
  }

  g_log->debug("constructing client state...");

  return spir_client_state{
      std::move(skim_config), std::move(skim_metadata), std::move(spir_config), std::move(hint_c), seed};
}

} // namespace skim::spir

#endif // SKIMDB_SPIR_H
