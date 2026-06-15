#ifndef SKIMDB_SIPER_H
#define SKIMDB_SIPER_H

#include <algorithm>
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

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <dgpp/uniform_rejection.hpp>

#include <skimdb/detail/skimdb_definitions.h>
#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/detail/skimdb_logger.h>
#include <skimdb/detail/skimdb_util.h>
#include <skimdb/skimdb.h>

#include "siper_config.h"
#include "skimdb_siper_definitions.h"
#include "skimdb_siper_matrix.h"
#include "skimdb_siper_util.h"

#ifdef SKIMDB_USE_RLWE
#include "skimdb_siper_rlwe.h"
#endif


namespace skim::siper {

namespace fs = std::filesystem;


class siper_server_state final {
public:
  // TODO: check that this does not blow up things...
  siper_server_state() = default;

  explicit siper_server_state(skimdb_matrix&& DB, skimdb_parameters&& skim_config, siperdb_parameters&& siper_config)
      : DB_{std::move(DB)}, skim_config_{std::move(skim_config)}, siper_config_{std::move(siper_config)} {}


  [[nodiscard]] auto skim_parameters() const -> const skimdb_parameters& { return skim_config_; }

  [[nodiscard]] auto siper_parameters() const -> const siperdb_parameters& { return siper_config_; }


  void update_batch_size(std::size_t new_batch_size) {
    if (new_batch_size == 0 || siper_config_.sqrt_N / siper_config_.block_size < new_batch_size) {
      g_log->error("invalid batch size {}, max supported batch size is {}",
                   new_batch_size,
                   siper_config_.sqrt_N / siper_config_.block_size);
      return;
    }

    g_log->debug("updating batch size from {} to {}...", siper_config_.batch_size, new_batch_size);

    siper_config_.batch_size = new_batch_size;
  }


  template <typename T>
  [[nodiscard]] auto answer(const siper_matrix<T>& qu) const
      -> std::expected<siper_matrix<std::uint64_t>, std::string> {
    auto [q_rows, q_cols] = qu.dimensions();

    if (q_rows != 1 || q_cols != siper_config_.sqrt_N) {
      return std::unexpected{"invalid query vector dimensions"};
    }

    return mat_vec(DB_, qu.span(), siper_config_.log_q);
  }

  [[nodiscard]] auto batch_answer(siper_matrix<const std::uint64_t>& qu_mat) const
      -> std::expected<siper_matrix<std::uint64_t>, std::string> {
    auto [q_rows, q_cols] = qu_mat.dimensions();

    if (q_rows != siper_config_.batch_size || q_cols != siper_config_.sqrt_N) {
      return std::unexpected{"invalid query matrix dimensions"};
    }

    siper_matrix<std::uint64_t> ans{siper_config_.sqrt_N, siper_config_.log_q};
    auto dst = ans.span();

    std::size_t blocks_per_col = siper_config_.sqrt_N / siper_config_.block_size;
    std::size_t blocks_per_batch = blocks_per_col / siper_config_.batch_size;
    std::size_t remaining_blocks = blocks_per_col % siper_config_.batch_size;

    for (std::size_t i = 0; i < siper_config_.batch_size; ++i) {
      std::size_t start_idx = 0;
      std::size_t count = 0;

      if (i < remaining_blocks) {
        start_idx = i * (blocks_per_batch + 1) * siper_config_.block_size;
        count = (blocks_per_batch + 1) * siper_config_.block_size;
      } else {
        start_idx = (i * blocks_per_batch + remaining_blocks) * siper_config_.block_size;
        count = blocks_per_batch * siper_config_.block_size;
      }

      partitioned_mat_vec(DB_, qu_mat.row(i), dst.subspan(start_idx, count), siper_config_.log_q, start_idx, count);
    }

    return ans;
  }


  [[nodiscard]] static auto info(const fs::path& path) -> std::expected<
      std::tuple<std::chrono::system_clock::time_point, siper_version_t, skimdb_parameters, siperdb_parameters>,
      std::string> {
    skimdb_parameters skim_config{};
    siperdb_parameters siper_config{};

    std::int64_t stamp{0};
    siper_version_t ver;

    std::ifstream is{path, std::ios::binary};

    if (!is) {
      return std::unexpected{"could not open file"};
    }

    try {
      cereal::BinaryInputArchive ar(is);

      ar(ver,
         stamp,
         skim_config.k,
         skim_config.s,
         skim_config.t,
         siper_config.n,
         siper_config.sigma,
         siper_config.log_p,
         siper_config.log_q,
         siper_config.block_size,
         siper_config.batch_size,
         siper_config.sqrt_N,
         siper_config.seed,
         siper_config.metadata_hash,
         siper_config.hint_c_hash);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("deserialization failed {}", e.what())};
    }

    auto tp = std::chrono::system_clock::time_point{std::chrono::seconds{stamp}};

    return std::make_tuple(tp, ver, skim_config, siper_config);
  }

  auto load(const fs::path& path) -> std::expected<void, std::string> {
    std::ifstream is{path, std::ios::binary};

    if (!is) {
      return std::unexpected{"could not open file"};
    }

    try {
      cereal::BinaryInputArchive ar(is);
      siper_version_t ver;
      std::int64_t stamp{0};

      ar(ver,
         stamp,
         skim_config_.k,
         skim_config_.s,
         skim_config_.t,
         siper_config_.n,
         siper_config_.sigma,
         siper_config_.log_p,
         siper_config_.log_q,
         siper_config_.block_size,
         siper_config_.batch_size,
         siper_config_.sqrt_N,
         siper_config_.seed,
         siper_config_.metadata_hash,
         siper_config_.hint_c_hash,
         DB_);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("deserialization failed {}", e.what())};
    }

    return {};
  }

  auto save(const fs::path& path) const -> std::expected<std::uintmax_t, std::string> {
    std::ofstream of{path, std::ios::binary};

    if (!of) {
      return std::unexpected{"could not create file"};
    }

    try {

      std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
      std::int64_t stamp{std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()};

      cereal::BinaryOutputArchive ar{of};
      siper_version_t ver;

      ar(ver,
         stamp,
         skim_config_.k,
         skim_config_.s,
         skim_config_.t,
         siper_config_.n,
         siper_config_.sigma,
         siper_config_.log_p,
         siper_config_.log_q,
         siper_config_.block_size,
         siper_config_.batch_size,
         siper_config_.sqrt_N,
         siper_config_.seed,
         siper_config_.metadata_hash,
         siper_config_.hint_c_hash,
         DB_);
    } catch (const std::exception& e) {
      return std::unexpected{std::format("serialization failed {}", e.what())};
    }

    of.close();

    return fs::file_size(path);
  }

private:
  skimdb_matrix DB_{};                // matrix representation of rle encodings
  skimdb_parameters skim_config_{};   // skimdb index parameters
  siperdb_parameters siper_config_{}; // SIPER parameters
};


[[nodiscard]] auto load_server(const fs::path& path) -> std::expected<siper_server_state, std::string> {
  siper_server_state state;
  auto res = state.load(path);

  if (!res) {
    return std::unexpected{std::format("failed to load server: {}", res.error())};
  }

  return state;
}

[[nodiscard]] auto make_server(skimdb&& db,
                               std::size_t log_p,
                               std::size_t log_q,
                               std::size_t n,
                               double sigma,
                               std::size_t block_size = 1,
                               std::size_t batch_size = 1,
                               std::uint64_t seed = std::random_device{}())
    -> std::expected<siper_server_state, std::string> {
  LogFun lf{"make_server(...)"};

  if (log_p < 16 || log_p >= 32) {
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

  if (kmers == 0) {
    return std::unexpected{"empty skimdb index"};
  }

  // need to store the starting position and length of each RLE in the matrix (in terms of blocks)
  // encoding using uint64_t with lower 12 bits for length and upper 52 bits for starting positions
  // allows for up to 4095 blocks per RLE and a matrix with up to 2^52 blocks, which should be
  // sufficient for our use case.

  g_log->info("skimdb contains {} kmers, computing sqrt N...", kmers);

  std::vector<std::uint16_t> rle_lengths(kmers, 0);
  std::size_t max_len = 0;

#pragma omp parallel for schedule(static) reduction(max : max_len)
  for (std::size_t i = 0; i < kmers; ++i) {
    std::size_t rle_len = (db_parts.data[i].length() + block_size - 1) / block_size;

    max_len = std::max(max_len, rle_len);
    rle_lengths[i] = static_cast<std::uint16_t>(rle_len);
  }

  if (max_len > (1 << index::g_len_bits) - 1) {
    return std::unexpected{"RLE length exceeds maximum supported length"};
  }

  std::vector<std::uint64_t> kmer_metadata(kmers, 0);
  std::uint64_t run_sum = 0;

  std::exclusive_scan(
      std::execution::par, rle_lengths.begin(), rle_lengths.end(), kmer_metadata.begin(), std::uint64_t{0});
  run_sum = kmer_metadata.back() + rle_lengths.back();

#pragma omp parallel for schedule(guided)
  for (std::size_t i = 0; i < kmers; ++i) {
    kmer_metadata[i] = index::pack(kmer_metadata[i], rle_lengths[i]);
  }

  auto sqrt_N = min_sqrt_N(run_sum, block_size);

  g_log->info("skimdb contains {} total runs, requires matrix with sqrt(N) = {} for block size {}",
              run_sum * block_size,
              sqrt_N,
              block_size);

  g_log->info("packing RLE encodings into matrix format...");

  auto DB = populate_skimdb_matrix(db_parts.data, kmer_metadata, sqrt_N, block_size);

  siper_common_rng_t rng{seed};

  siper_matrix<std::uint64_t> A{sqrt_N, n, log_q};
  A.fill(rng);

  // compute hint_c = DB * A
  g_log->info("precomputing hint matrix DB * A, be patient...");
  auto hint_c = mat_mul(DB, A, log_q);

  g_log->info("saving client metadata...");
  std::string metadata_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.siper_server_store_dir) / rand_name;

    {
      std::ofstream os{temp_metadata_path, std::ios::binary};
      if (!os) {
        return std::unexpected{"could not create client metadata"};
      }

      cereal::BinaryOutputArchive archive{os};
      archive(db_parts.index, kmer_metadata, db_parts.labels);
    }

    auto hash_res = sha256_file(temp_metadata_path);

    if (!hash_res) {
      return std::unexpected{hash_res.error()};
    }

    metadata_hash = "meta." + hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_skim_config.siper_server_store_dir) / metadata_hash, ec);
    if (ec) {
      return std::unexpected{"could not rename client metadata"};
    }

    g_log->info("client metadata generated with hash {}", metadata_hash);
  }

  g_log->info("saving hint_c...");
  std::string hint_c_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.siper_server_store_dir) / rand_name;

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

    hint_c_hash = "hint." + hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_skim_config.siper_server_store_dir) / hint_c_hash, ec);

    if (ec) {
      return std::unexpected{"could not rename hint_c"};
    }

    g_log->info("hint_c generated with hash {}", hint_c_hash);
  }

  g_log->info("constructing server state...");

  skimdb_parameters skim_conf{.k = k, .s = s, .t = t};

  siperdb_parameters siper_conf{.n = n,
                                .sigma = sigma,
                                .log_p = log_p,
                                .log_q = log_q,
                                .block_size = block_size,
                                .batch_size = batch_size,
                                .sqrt_N = sqrt_N,
                                .seed = seed,
                                .metadata_hash = metadata_hash,
                                .hint_c_hash = hint_c_hash};

  return siper_server_state{std::move(DB), std::move(skim_conf), std::move(siper_conf)};
}

#ifdef SKIMDB_USE_RLWE
[[nodiscard]] auto make_server_rlwe(skimdb&& db,
                               std::size_t log_p,
                               std::size_t log_q,
                               std::uint64_t poly_degree,
                               std::size_t block_size = 1,
                               std::size_t batch_size = 1,
                               std::uint64_t seed = std::random_device{}())
    -> std::expected<siper_server_state, std::string> {
  LogFun lf{"make_server(...)"};

  if (log_p < 16 || log_p >= 32) {
    return std::unexpected{"unsupported plaintext modulus"};
  }
  if (log_q < 32 || log_q > 64) {
    return std::unexpected{"unsupported ciphertext modulus"};
  }
  if (poly_degree == 0 || (poly_degree & (poly_degree - 1)) != 0) {
    return std::unexpected{"poly_degree must be a power of 2"};
  }

  auto [k, s, t] = db.parameters();
  auto db_parts = std::move(db).explode();

  auto kmers = db_parts.data.size();

  if (kmers == 0) {
    return std::unexpected{"empty skimdb index"};
  }

  // need to store the starting position and length of each RLE in the matrix (in terms of blocks)
  // encoding using uint64_t with lower 12 bits for length and upper 52 bits for starting positions
  // allows for up to 4095 blocks per RLE and a matrix with up to 2^52 blocks, which should be
  // sufficient for our use case.

  g_log->info("skimdb contains {} kmers, computing sqrt N...", kmers);

  std::vector<std::uint16_t> rle_lengths(kmers, 0);
  std::size_t max_len = 0;

#pragma omp parallel for schedule(static) reduction(max : max_len)
  for (std::size_t i = 0; i < kmers; ++i) {
    std::size_t rle_len = (db_parts.data[i].length() + block_size - 1) / block_size;

    max_len = std::max(max_len, rle_len);
    rle_lengths[i] = static_cast<std::uint16_t>(rle_len);
  }

  if (max_len > (1 << index::g_len_bits) - 1) {
    return std::unexpected{"RLE length exceeds maximum supported length"};
  }

  std::vector<std::uint64_t> kmer_metadata(kmers, 0);
  std::uint64_t run_sum = 0;

  std::exclusive_scan(
      std::execution::par, rle_lengths.begin(), rle_lengths.end(), kmer_metadata.begin(), std::uint64_t{0});
  run_sum = kmer_metadata.back() + rle_lengths.back();

#pragma omp parallel for schedule(guided)
  for (std::size_t i = 0; i < kmers; ++i) {
    kmer_metadata[i] = index::pack(kmer_metadata[i], rle_lengths[i]);
  }

  auto sqrt_N = min_sqrt_N(run_sum, block_size);

  g_log->info("skimdb contains {} total runs, requires matrix with sqrt(N) = {} for block size {}",
              run_sum * block_size,
              sqrt_N,
              block_size);

  g_log->info("packing RLE encodings into matrix format...");

  auto DB = populate_skimdb_matrix(db_parts.data, kmer_metadata, sqrt_N, block_size);

  std::uint64_t p_mod = 1ULL << log_p;

  rlwe::RLWEContext ctx(poly_degree, log_q, p_mod);
  rlwe::RLWEKey key(ctx);

  siper_common_rng_t rng{seed};
  auto [a_seeds, num_seeds] = rlwe::gen_a_seeds(rng, sqrt_N, poly_degree);

  g_log->info("RLWE: num_seeds = {}", num_seeds);

  // compute hint_c = DB * A via NTT
  g_log->info("precomputing RLWE hint matrix via NTT, be patient...");
  auto hint_c = rlwe::compute_hint_ntt(ctx, key, DB, a_seeds, num_seeds, sqrt_N);

  g_log->info("saving client metadata...");
  std::string metadata_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.siper_server_store_dir) / rand_name;

    {
      std::ofstream os{temp_metadata_path, std::ios::binary};
      if (!os) {
        return std::unexpected{"could not create client metadata"};
      }

      cereal::BinaryOutputArchive archive{os};
      archive(db_parts.index, kmer_metadata, db_parts.labels);
    }

    auto hash_res = sha256_file(temp_metadata_path);

    if (!hash_res) {
      return std::unexpected{hash_res.error()};
    }

    metadata_hash = "meta." + hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_skim_config.siper_server_store_dir) / metadata_hash, ec);
    if (ec) {
      return std::unexpected{"could not rename client metadata"};
    }

    g_log->info("client metadata generated with hash {}", metadata_hash);
  }

  g_log->info("saving hint_c...");
  std::string hint_c_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.siper_server_store_dir) / rand_name;

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

    hint_c_hash = "hint." + hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_skim_config.siper_server_store_dir) / hint_c_hash, ec);

    if (ec) {
      return std::unexpected{"could not rename hint_c"};
    }

    g_log->info("hint_c generated with hash {}", hint_c_hash);
  }

  g_log->info("constructing server state...");

  skimdb_parameters skim_conf{.k = k, .s = s, .t = t};

  siperdb_parameters siper_conf{.n = poly_degree,
                                .sigma = 0.0,
                                .log_p = log_p,
                                .log_q = log_q,
                                .block_size = block_size,
                                .batch_size = batch_size,
                                .sqrt_N = sqrt_N,
                                .seed = seed,
                                .metadata_hash = metadata_hash,
                                .hint_c_hash = hint_c_hash};

  return siper_server_state{std::move(DB), std::move(skim_conf), std::move(siper_conf)};
}
#endif


class siper_client_state {
public:
  using rng_type = siper_common_rng_t;

  siper_client_state(skimdb_parameters skim_config,
                     skimdb_metadata skim_metadata,
                     siperdb_parameters siper_config,
                     siper_matrix<std::uint64_t> hint_c,
                     std::uint64_t seed = std::random_device{}())
      : skim_config_{std::move(skim_config)}, skim_metadata_{std::move(skim_metadata)},
        siper_config_{std::move(siper_config)}, A_{siper_config_.sqrt_N, siper_config_.n, siper_config_.log_q},
        hint_c_{std::move(hint_c)}, main_seed_{seed} {
    siper_common_rng_t rng{siper_config_.seed};
    A_.fill(rng);
  }


#ifdef SKIMDB_USE_RLWE
  void init_rlwe(std::uint64_t poly_degree, std::uint64_t plaintext_mod) {
    rlwe_ctx_ = std::make_unique<rlwe::RLWEContext>(poly_degree, siper_config_.log_q, plaintext_mod);
    rlwe_key_ = std::make_unique<rlwe::RLWEKey>(*rlwe_ctx_);

    siper_common_rng_t seed_rng{siper_config_.seed};
    auto [seeds, num_seeds] = rlwe::gen_a_seeds(seed_rng, siper_config_.sqrt_N, poly_degree);
    a_seeds_ = std::move(seeds);
    num_a_seeds_ = num_seeds;
  }
#endif


  [[nodiscard]] auto skim_parameters() const -> skimdb_parameters { return skim_config_; }

  [[nodiscard]] auto siper_parameters() const -> siperdb_parameters { return siper_config_; }


  [[nodiscard]] auto is_valid_kmer(const std::string& str) const -> bool {
    auto kmer = detail::kmer_to_binary(str);
    auto canonical = std::min(kmer, detail::reverse_complement(kmer, skim_config_.k));
    return detail::is_valid(str, skim_config_.k) &&
           detail::is_syncmer(canonical, skim_config_.k, skim_config_.s, skim_config_.t);
  }

  // convert a kmer to its corresponing position in the matrix (row index, column index, rle length in runs)
  [[nodiscard]] auto kmer_to_position(const std::string& s) const
      -> std::optional<std::tuple<std::size_t, std::size_t, std::size_t>> {
    LogFun lf{"siper_client_state::kmer_to_position(...)", spdlog::level::debug};

    auto kmer = detail::kmer_to_binary(s);
    auto canonical = std::min(kmer, detail::reverse_complement(kmer, skim_config_.k));
    auto res = skim_metadata_.index.find(canonical);

    if (!res.has_value()) {
      // if the k‑mer is valid but absent from the skimdb index, the k‑mer has no associated labels
      return std::nullopt;
    }

    auto k_idx = res.value();
    auto k_metadata = skim_metadata_.kmer_metadata[k_idx];
    auto start_block = index::unpack_start(k_metadata);

    std::size_t start_run = start_block * siper_config_.block_size;
    std::size_t i_col = start_run / siper_config_.sqrt_N;
    std::size_t i_row = start_run % siper_config_.sqrt_N;
    std::size_t rle_len = static_cast<std::size_t>(index::unpack_len(k_metadata)) * siper_config_.block_size;

    return std::make_tuple(i_row, i_col, rle_len);
  }

  // convert a row index and rle length (runs) to the corresponding batch partition and number of batches
  [[nodiscard]] auto row_to_partition(std::size_t i_row, std::size_t rle_len) const
      -> std::tuple<std::size_t, std::size_t> {
    // convert to blocks first to guarantee that blocks are not split across batch partitions
    std::size_t blocks_per_col = siper_config_.sqrt_N / siper_config_.block_size;
    std::size_t blocks_per_batch = blocks_per_col / siper_config_.batch_size;
    std::size_t remaining_blocks = blocks_per_col % siper_config_.batch_size;

    std::size_t start_block = i_row / siper_config_.block_size;
    std::size_t end_block = (i_row + rle_len - 1) / siper_config_.block_size;

    std::size_t start_batch = 0;
    std::size_t end_batch = 0;
    std::size_t batch_count = 0;

    if (start_block < remaining_blocks * (blocks_per_batch + 1)) {
      start_batch = start_block / (blocks_per_batch + 1);
    } else {
      start_batch = (start_block - remaining_blocks * (blocks_per_batch + 1)) / blocks_per_batch + remaining_blocks;
    }

    if (end_block < remaining_blocks * (blocks_per_batch + 1)) {
      end_batch = end_block / (blocks_per_batch + 1);
    } else {
      end_batch = (end_block - remaining_blocks * (blocks_per_batch + 1)) / blocks_per_batch + remaining_blocks;
    }

    batch_count = end_batch - start_batch + 1;

    return std::make_tuple(start_batch, batch_count);
  }


  [[nodiscard]] auto prepare_query(std::size_t i_col) -> siperdb_query_state {
    LogFun lf{"siper_client_state::prepare_query(...)", spdlog::level::debug};

    auto& rng = m_get_rng_();

    siper_matrix<std::uint64_t> s{siper_config_.n, siper_config_.log_q};
    s.fill(rng);

    dgpp::uniform_rejection dist{siper_config_.sigma};
    siper_matrix<std::uint64_t> e{siper_config_.sqrt_N, siper_config_.log_q};
    e.fill(rng, dist);

    std::size_t delta = 1ull << (siper_config_.log_q - siper_config_.log_p);

    auto qu = mat_vec(A_, s, siper_config_.log_q);
    qu.add(e);
    qu.set(i_col, qu.get(i_col) + delta);

    return siperdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }


  [[nodiscard]] auto new_batch() -> siperdb_query_state {
    LogFun lf{"siper_client_state::new_batch(...)", spdlog::level::debug};

    auto& rng = m_get_rng_();

    siper_matrix<std::uint64_t> s{siper_config_.batch_size, siper_config_.n, siper_config_.log_q};
    s.fill(rng);

    dgpp::uniform_rejection dist{siper_config_.sigma};
    siper_matrix<std::uint64_t> e{siper_config_.batch_size, siper_config_.sqrt_N, siper_config_.log_q};
    e.fill(rng, dist);

    siper_matrix<std::uint64_t> qu{siper_config_.batch_size, siper_config_.sqrt_N, siper_config_.log_q};

    for (std::size_t i = 0; i < siper_config_.batch_size; ++i) {
      mat_vec(A_, s.row(i), qu.row(i), siper_config_.log_q);
    }
    qu.add(e);

    return siperdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }

  void update_batch(siperdb_query_state& batch_state, std::size_t i_batch, std::size_t i_col) {
    LogFun lf{"siper_client_state::update_batch(...)", spdlog::level::trace};

    std::uint64_t delta = 1ull << (siper_config_.log_q - siper_config_.log_p);
    batch_state.qu_vec.set(i_batch, i_col, batch_state.qu_vec.get(i_batch, i_col) + delta);
  }


  template <typename T>
  void recover(const siper_matrix<T>& ans,
               const siperdb_query_state& qu,
               std::span<std::uint16_t> rle,
               std::size_t i_row,
               std::size_t count,
               std::size_t i_batch = 0) {
    LogFun lf{"siper_client_state::recover(...)", spdlog::level::debug};

    auto d = sub_mat_vec_rows(ans, hint_c_, qu.s_vec.row(i_batch), siper_config_.log_q, i_row, count);
    d.div_delta(siper_config_.log_q - siper_config_.log_p);
    auto d_data = d.span();

    for (std::size_t i = 0; i < d_data.size(); ++i) {
      rle[i] = static_cast<std::uint16_t>(d_data[i] & 0xFFFFull);
    }
  }

#ifdef SKIMDB_USE_RLWE
  [[nodiscard]] auto prepare_query_hybrid(std::size_t i_col) -> siperdb_query_state {
    LogFun lf{"siper_client_state::prepare_query_hybrid(...)", spdlog::level::debug};

    std::vector<std::uint64_t> pt_data(siper_config_.sqrt_N, 0);
    pt_data[i_col] = 1;

    auto qu = rlwe::prepare_query_hybrid(
        *rlwe_ctx_, *rlwe_key_, a_seeds_, num_a_seeds_, pt_data.data(), siper_config_.sqrt_N);

    auto s = rlwe_key_->extract_lwe_key();

    return siperdb_query_state{.s_vec = std::move(s), .qu_vec = std::move(qu)};
  }

  void recover_hybrid(const siper_matrix<std::uint64_t>& ans,
                      const siperdb_query_state& qu,
                      std::span<std::uint16_t> rle,
                      std::size_t i_row,
                      std::size_t count) {
    LogFun lf{"siper_client_state::recover_hybrid(...)", spdlog::level::debug};

    auto d = rlwe::recover_hybrid(*rlwe_ctx_, ans, hint_c_, qu.s_vec, siper_config_.log_q, i_row, count);
    auto d_data = d.span();

    for (std::size_t i = 0; i < count && i < d_data.size(); ++i) {
      rle[i] = static_cast<std::uint16_t>(d_data[i] & 0xFFFFull);
    }
  }
#endif

  [[nodiscard]] auto interpret(std::vector<std::uint16_t> src) -> std::generator<const std::string&> {
    LogFun lf{"siper_client_state::interpret(...)", spdlog::level::debug};

    auto rle = detail::encoding{std::move(src)};

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

  auto m_get_rng_() -> siper_common_rng_t& {
    thread_local siper_common_rng_t rng(m_make_local_seed_());
    return rng;
  }

  skimdb_parameters skim_config_; // skimdb index parameters
  skimdb_metadata skim_metadata_; // skimdb metadata (kmer index, labels)

  siperdb_parameters siper_config_; // siper parameters

  siper_matrix<std::uint64_t> A_;      // matrix A
  siper_matrix<std::uint64_t> hint_c_; // hint matrix from server

  std::uint64_t main_seed_;

#ifdef SKIMDB_USE_RLWE
  std::unique_ptr<rlwe::RLWEContext> rlwe_ctx_;
  std::unique_ptr<rlwe::RLWEKey> rlwe_key_;
  std::vector<std::uint64_t> a_seeds_;
  std::uint64_t num_a_seeds_ = 0;
#endif
};


[[nodiscard]] auto
load_client(skimdb_parameters skim_config, siperdb_parameters siper_config, std::uint64_t seed = std::random_device{}())
    -> std::expected<siper_client_state, std::string> {
  LogFun lf{"load_client(...)"};

  fs::path metadata_path = fs::path(g_skim_config.siper_client_metadata_dir) / siper_config.metadata_hash;
  fs::path hint_c_path = fs::path(g_skim_config.siper_client_hint_c_dir) / siper_config.hint_c_hash;

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
      std::vector<std::uint64_t> kmer_metadata;
      std::vector<std::string> labels;

      archive(index, kmer_metadata, labels);

      skim_metadata = skimdb_metadata{
          .index = std::move(index), .kmer_metadata = std::move(kmer_metadata), .labels = std::move(labels)};
    } catch (...) {
      return std::unexpected{"deserialization failed"};
    }
  }

  g_log->debug("loading hint_c from {}...", hint_c_path.string());

  siper_matrix<std::uint64_t> hint_c;

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

  return siper_client_state{
      std::move(skim_config), std::move(skim_metadata), std::move(siper_config), std::move(hint_c), seed};
}

} // namespace skim::siper

#endif // SKIMDB_SIPER_H
