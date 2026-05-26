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

#include "skimdb/skimdb_config.h"
#include "skimdb_spir_definitions.h"
#include "skimdb_spir_matrix.h"
#include "skimdb_spir_util.h"


namespace skim::spir {

namespace fs = std::filesystem;


class spir_server_state final {
public:
  // TODO: check that this does not blow up things...
  spir_server_state() = default;

  explicit spir_server_state(skimdb_matrix&& DB, skimdb_parameters&& skim_config, spirdb_parameters&& spir_config)
      : DB_{std::move(DB)}, skim_config_{std::move(skim_config)}, spir_config_{std::move(spir_config)} {}


  [[nodiscard]] auto skim_parameters() const -> const skimdb_parameters& { return skim_config_; }

  [[nodiscard]] auto spir_parameters() const -> const spirdb_parameters& { return spir_config_; }


  void update_batch_size(std::size_t new_batch_size) {
    if (new_batch_size == 0 || spir_config_.sqrt_N / spir_config_.block_size < new_batch_size) {
      g_log->error("invalid batch size {}, max supported batch size is {}", new_batch_size, spir_config_.sqrt_N / spir_config_.block_size);
      return;
    }

    g_log->debug("updating batch size from {} to {}...", spir_config_.batch_size, new_batch_size);

    spir_config_.batch_size = new_batch_size;
  }


  [[nodiscard]] auto answer(const spir_matrix& qu) const -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = qu.dimensions();

    if (q_rows != 1 || q_cols != spir_config_.sqrt_N) {
      return std::unexpected{"invalid query vector dimensions"};
    }

    return mat_vec(DB_, qu, spir_config_.log_q);
  }

  [[nodiscard]] auto batch_answer(const spir_matrix& qu_mat) const -> std::expected<spir_matrix, std::string> {
    auto [q_rows, q_cols] = qu_mat.dimensions();
    if (q_rows != spir_config_.batch_size || q_cols != spir_config_.sqrt_N) {
      return std::unexpected{"invalid query matrix dimensions"};
    }

    spir_matrix ans{spir_config_.sqrt_N, spir_config_.log_q};
    auto dst = ans.span();

    std::size_t blocks_per_col = spir_config_.sqrt_N / spir_config_.block_size;
    std::size_t blocks_per_batch = blocks_per_col / spir_config_.batch_size;
    std::size_t remaining_blocks = blocks_per_col % spir_config_.batch_size;
    
    for (std::size_t i = 0; i < spir_config_.batch_size; ++i) {
      std::size_t start_idx, count;

      if (i < remaining_blocks) {
        start_idx = i * (blocks_per_batch + 1) * spir_config_.block_size;
        count = (blocks_per_batch + 1) * spir_config_.block_size;
      } else {
        start_idx = (i * blocks_per_batch + remaining_blocks) * spir_config_.block_size;
        count = blocks_per_batch * spir_config_.block_size;
      }

      partitioned_mat_vec(DB_, qu_mat.row(i), dst.subspan(start_idx, count), spir_config_.log_q, start_idx, count);
    }

    return ans;
  }


  auto load(const fs::path& path) -> std::expected<void, std::string> {
    std::ifstream is{path, std::ios::binary};
    if (!is) {
      return std::unexpected{"could not open file"};
    }

    try {
      cereal::BinaryInputArchive archive(is);
      skimdb_version_t ver;
      archive(ver,
              DB_,
              skim_config_.k,
              skim_config_.s,
              skim_config_.t,
              spir_config_.n,
              spir_config_.sigma,
              spir_config_.log_p,
              spir_config_.log_q,
              spir_config_.block_size,
              spir_config_.batch_size,
              spir_config_.sqrt_N,
              spir_config_.seed,
              spir_config_.metadata_hash,
              spir_config_.hint_c_hash);
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
      cereal::BinaryOutputArchive archive{of};
      skimdb_version_t ver;
      archive(ver,
              DB_,
              skim_config_.k,
              skim_config_.s,
              skim_config_.t,
              spir_config_.n,
              spir_config_.sigma,
              spir_config_.log_p,
              spir_config_.log_q,
              spir_config_.block_size,
              spir_config_.batch_size,
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
  skimdb_matrix DB_{};              // matrix representation of rle encodings
  skimdb_parameters skim_config_{}; // skimdb index parameters
  spirdb_parameters spir_config_{}; // SPIR parameters
};


[[nodiscard]] auto load_server(const fs::path& path) -> std::expected<spir_server_state, std::string> {
  spir_server_state state;
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
    -> std::expected<spir_server_state, std::string> {
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

  // need to store the starting position and length of each RLE in the matrix (in terms of blocks)
  // encoding using uint64_t with lower 12 bits for length and upper 52 bits for starting positions
  // this allows for up to 4095 blocks per RLE and a matrix with up to 2^52 blocks, which should be sufficient for our use case.

  g_log->info("skimdb contains {} kmers, computing sqrt N...", kmers);

  std::vector<std::uint16_t> rle_lengths(kmers, 0);
  std::size_t max_len = 0;
  
#pragma omp parallel for schedule(static) reduction(max:max_len)
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
#pragma omp parallel for reduction(inscan, +:run_sum)
  for (std::size_t i = 0; i < kmers; ++i){
    kmer_metadata[i] = run_sum;

  #pragma omp scan exclusive(run_sum)

    run_sum += rle_lengths[i];
  }

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < kmers; ++i) {
    kmer_metadata[i] = index::pack(kmer_metadata[i], rle_lengths[i]);
  }

  if (kmers == 0 || run_sum == 0) {
    return std::unexpected{"empty skimdb index"};
  }

  auto sqrt_N = min_sqrt_N(run_sum, block_size);

  g_log->info("skimdb contains {} total runs, requires matrix with sqrt(N) = {} for block size {}", run_sum * block_size, sqrt_N, block_size);

  g_log->info("packing RLE encodings into matrix format...");

  auto DB = populate_skimdb_matrix(db_parts.data, kmer_metadata, sqrt_N, block_size);

  spir_common_rng_t rng{seed};

  spir_matrix A{sqrt_N, n, log_q};
  A.fill(rng);

  // compute hint_c = DB * A
  g_log->info("precomputing hint matrix DB * A, be patient...");
  auto hint_c = mat_mul(DB, A, log_q);

  g_log->info("saving client metadata...");
  std::string metadata_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.spir_server_store_dir) / rand_name;

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

    metadata_hash = hash_res.value();

    std::error_code ec;
    fs::rename(temp_metadata_path, fs::path(g_skim_config.spir_server_store_dir) / metadata_hash, ec);
    if (ec) {
      return std::unexpected{"could not rename client metadata"};
    }

    g_log->info("client metadata generated with hash {}", metadata_hash);
  }

  g_log->info("saving hint_c...");
  std::string hint_c_hash;

  {
    std::string rand_name = std::to_string(std::random_device{}());
    fs::path temp_metadata_path = fs::path(g_skim_config.spir_server_store_dir) / rand_name;

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
    fs::rename(temp_metadata_path, fs::path(g_skim_config.spir_server_store_dir) / hint_c_hash, ec);

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
                              .block_size = block_size,
                              .batch_size = batch_size,
                              .sqrt_N = sqrt_N,
                              .seed = seed,
                              .metadata_hash = metadata_hash,
                              .hint_c_hash = hint_c_hash};

  return spir_server_state{std::move(DB), std::move(skim_conf), std::move(spir_conf)};
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
    auto canonical = std::min(kmer, detail::reverse_complement(kmer, skim_config_.k));
    return detail::is_valid(str, skim_config_.k) &&
           detail::is_syncmer(canonical, skim_config_.k, skim_config_.s, skim_config_.t);
  }

  // convert a kmer to its corresponing position in the matrix (row index, column index, rle length in runs)
  [[nodiscard]] auto kmer_to_position(const std::string& s) const
      -> std::optional<std::tuple<std::size_t, std::size_t, std::size_t>> {
    LogFun lf{"spir_client_state::kmer_to_position(...)", spdlog::level::debug};

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

    std::size_t start_run = start_block * spir_config_.block_size;
    std::size_t i_col = start_run / spir_config_.sqrt_N;
    std::size_t i_row = start_run % spir_config_.sqrt_N;
    std::size_t rle_len = static_cast<std::size_t>(index::unpack_len(k_metadata)) * spir_config_.block_size;

    return std::make_tuple(i_row, i_col, rle_len);
  }

  // convert a row index and rle length (runs) to the corresponding batch partition and number of batches
  [[nodiscard]] auto row_to_partition(std::size_t i_row, std::size_t rle_len) const -> std::tuple<std::size_t, std::size_t> {
    // convert to blocks first to guarantee that blocks are not split across batch partitions
    std::size_t blocks_per_col = spir_config_.sqrt_N / spir_config_.block_size;
    std::size_t blocks_per_batch = blocks_per_col / spir_config_.batch_size;
    std::size_t remaining_blocks = blocks_per_col % spir_config_.batch_size;

    std::size_t start_block = i_row / spir_config_.block_size;
    std::size_t end_block = (i_row + rle_len - 1) / spir_config_.block_size;

    std::size_t start_batch, end_batch, batch_count;
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


  [[nodiscard]] auto prepare_query(std::size_t i_col) -> spirdb_query_state {
    LogFun lf{"spir_client_state::prepare_query(...)", spdlog::level::debug};

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


  void recover(const spir_matrix& ans,
               const spirdb_query_state& qu,
               std::span<std::uint16_t> rle,
               std::size_t i_row,
               std::size_t count,
               std::size_t i_batch = 0) {
    LogFun lf{"spir_client_state::recover(...)", spdlog::level::debug};

    auto d = sub_mat_vec_rows(ans, hint_c_, qu.s_vec.row(i_batch), spir_config_.log_q, i_row, count);
    d.div_delta(spir_config_.log_q - spir_config_.log_p);
    auto d_data = d.span();

    for (std::size_t i = 0; i < d_data.size(); ++i) {
      rle[i] = static_cast<std::uint16_t>(d_data[i] & 0xFFFFull);
    }
  }

  [[nodiscard]] auto interpret(std::vector<std::uint16_t> src) -> std::generator<const std::string&> {
    LogFun lf{"spir_client_state::interpret(...)", spdlog::level::debug};

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

  auto m_get_rng_() -> spir_common_rng_t& {
    thread_local spir_common_rng_t rng(m_make_local_seed_());
    return rng;
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

  fs::path metadata_path = fs::path(g_skim_config.spir_client_metadata_dir) / spir_config.metadata_hash;
  fs::path hint_c_path = fs::path(g_skim_config.spir_client_hint_c_dir) / spir_config.hint_c_hash;

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

      skim_metadata = skimdb_metadata{.index = std::move(index), .kmer_metadata = std::move(kmer_metadata), .labels = std::move(labels)};
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
