#ifndef SKIMDB_SPIR_MATRIX_H
#define SKIMDB_SPIR_MATRIX_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <cereal/types/vector.hpp>
#include <dgpp/uniform_rejection.hpp>

#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/detail/skimdb_logger.h>


namespace skim::spir {

// implements matrices (and vectors) with modular arithmetic
class spir_matrix {
public:
  explicit spir_matrix() = default;

  explicit spir_matrix(std::size_t rows, std::size_t cols, std::size_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_(rows * cols, 0) {}

  explicit spir_matrix(std::size_t n, std::size_t log_mod) : spir_matrix(1, n, log_mod) {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::size_t rows, std::size_t cols, std::size_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_{std::move(data)} {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::size_t n, std::size_t log_mod)
      : spir_matrix(std::move(data), 1, n, log_mod) {}


  void set(std::size_t i, std::size_t j, std::uint64_t x) { data_[i * c_ + j] = x & mask_; }

  void set(std::size_t i, std::uint64_t x) { data_[i] = x & mask_; }

  auto get(std::size_t i, std::size_t j) const -> std::uint64_t { return data_[i * c_ + j]; }

  auto get(std::size_t i) const -> std::uint64_t { return data_[i]; }


  auto span() -> std::span<std::uint64_t> { return data_; }

  auto span() const -> std::span<const std::uint64_t> { return data_; }

  auto row(std::size_t i) -> std::span<std::uint64_t> { return std::span{data_}.subspan(i * c_, c_); }

  auto row(std::size_t i) const -> std::span<const std::uint64_t> { return std::span{data_}.subspan(i * c_, c_); }


  auto dimensions() const -> std::tuple<std::size_t, std::size_t> { return std::make_tuple(r_, c_); }


  template <typename URBG>
  void fill(URBG&& rng) {
    for (auto& val : data_) {
      val = rng() & mask_;
    }
  }

  template <typename URBG>
  void fill(URBG&& rng, dgpp::uniform_rejection& dist) {
    for (auto& val : data_) {
      val = dist(rng) & mask_;
    }
  }


  auto add(const spir_matrix& mat) -> spir_matrix& {
    LogFun lf{"spir_matrix::add(...)", spdlog::level::debug};

    auto* dst = data_.data();
    std::size_t n = data_.size();
    const auto* src = mat.data_.data();

  #pragma omp parallel for simd schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = (dst[i] + src[i]) & mask_;
    }

    return *this;
  }


  auto div_delta(std::size_t log_delta) -> spir_matrix& {
    LogFun lf{"spir_matrix::div_delta(...)", spdlog::level::debug};

    auto* dst = data_.data();
    std::size_t n = data_.size();
    std::uint64_t half_delta = (log_delta == 0) ? 0ull : (1ull << (log_delta - 1));

  #pragma omp parallel for simd schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = ((dst[i] + half_delta) & mask_) >> log_delta;
    }

    return *this;
  }


  auto transpose() const -> spir_matrix {
    LogFun lf{"spir_matrix::transpose()", spdlog::level::debug};

    const auto* src = data_.data();

    spir_matrix out{c_, r_, log_mod_};
    auto dst = out.span();

  #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < r_; ++i) {
      for (std::size_t j = 0; j < c_; ++j) {
        dst[j * r_ + i] = src[i * c_ + j];
      }
    }

    return out;
  }


  template <typename Archive>
  void serialize(Archive& archive) {
    archive(r_, c_, log_mod_, mask_, data_);
  }

private:
  std::size_t r_{0};
  std::size_t c_{0};

  std::size_t log_mod_{0};

  std::uint64_t mask_{0};

  std::vector<std::uint64_t> data_; // row-major flat storage
};


void mat_vec(const spir_matrix& mat, std::span<const std::uint64_t> vec, std::span<std::uint64_t> dst, std::size_t log_q) {
  std::size_t m_rows = 0; // declared explicitely for libomp
  std::size_t m_cols = 0;

  std::tie(m_rows, m_cols) = mat.dimensions();
  auto mat_data = mat.span();

  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < m_rows; ++i) {
    std::uint64_t sum = 0;

  #pragma omp simd reduction(+:sum)
    for (std::size_t j = 0; j < m_cols; ++j) {
      sum += (mat_data[i * m_cols + j] * vec[j]) & mask;
    }

    dst[i] = sum & mask;
  }
}


// client side, prepare query (A*s)
inline auto mat_vec(const spir_matrix& mat, const spir_matrix& vec, std::size_t log_q) -> spir_matrix {
  LogFun lf{"mat_vec(spir_matrix, ...)", spdlog::level::debug};

  auto [m_rows, _] = mat.dimensions();
  spir_matrix out{m_rows, log_q};

  mat_vec(mat, vec.span(), out.span(), log_q);

  return out;
}


// client side, answer recovery: computing (ans - hint_c*s) for a range of blocks making up the target rle.
inline auto sub_mat_vec_rows(const spir_matrix &ans, const spir_matrix &hint, std::span<const std::uint64_t> s_data,
    std::size_t log_q, std::size_t i_start, std::size_t n_rows) -> spir_matrix {
  LogFun lf{"sub_mat_vec_rows(...)", spdlog::level::debug};

  std::size_t r = 0;
  std::size_t c = 0;

  std::tie(r, c) = hint.dimensions();
  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  const auto h_data = hint.span();

  spir_matrix out{n_rows, log_q};

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < n_rows; ++i) {
    std::uint64_t sum = 0;

  #pragma omp simd reduction(+:sum)
    for (std::size_t j = 0; j < c; ++j) {
      sum += (h_data[(i_start + i) * c + j] * s_data[j]) & mask;
    }

    sum &= mask;
    out.set(i, ans.get(i_start + i) - sum);
  }

  return out;
}


// wrapper around detail::encoding to provide matrix-like access
/* Expected sizes:
 *                |   kemrs  | log_p  | rle_blocks | rle_per_col | sqrt_N |     N       | approx log_N
 * ----------------------------------------------------------------------------------------------------
 *                |          |    8   |    1180    |     233     | 274940 | 75592003600 |    36.1
 *  viral20250425 | 63512373 |   16   |     590    |     329     | 194110 | 37678692100 |    35.1
 *                |          |   24   |     394    |     402     | 158388 | 25086758544 |    34.5
 * ----------------------------------------------------------------------------------------------------
 */
class skimdb_matrix {
public:
  explicit skimdb_matrix() = default;

  explicit skimdb_matrix(std::vector<detail::encoding>&& data,
                         std::size_t block_size,
                         std::size_t rle_blocks,
                         std::size_t sqrt_N)
    : data_{std::move(data)}, block_size_{block_size}, rle_blocks_{rle_blocks}, sqrt_N_{sqrt_N} {}

  auto block_size() const -> std::size_t { return block_size_; }

  auto rle_in_col(std::size_t n, std::size_t col) const -> std::span<const std::uint16_t> {
    std::size_t kmer_idx = (col * sqrt_N_) / rle_blocks_ + n;
    if (kmer_idx >= data_.size()) [[unlikely]] return {};
    return data_[kmer_idx].span();
  }

  auto dimensions() const -> std::tuple<std::size_t, std::size_t> { return std::make_tuple(sqrt_N_, sqrt_N_); }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(data_, block_size_, rle_blocks_, sqrt_N_);
  }

private:
  std::vector<detail::encoding> data_;
  std::size_t block_size_;   // bytes of plaintext data we can pack into one block
  std::size_t rle_blocks_;   // blocks needed per RLE encoding
  std::size_t sqrt_N_;       // matrix side length (blocks of data)
};


void partitioned_mat_vec1(const skimdb_matrix& mat,
                          std::span<const std::uint64_t> vec,
                          std::span<std::uint64_t> dst,
                          std::size_t log_q,
                          std::size_t start,
                          std::size_t count,
                          std::size_t rle_blocks) {
  std::size_t m_cols = 0;
  std::tie(std::ignore, m_cols) = mat.dimensions();

  const std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::size_t p = start; p < start + count; ++p) {
    std::span<std::uint64_t> out = dst.subspan(p * rle_blocks, rle_blocks);

    for (std::size_t j = 0; j < m_cols; ++j) {
      auto rle = mat.rle_in_col(p, j);

      const auto* rle_ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
      const std::size_t len = rle.size() * 2;

      auto vec_val = vec[j];

      for (std::size_t i = 0; i < len; ++i) {
        out[i] += (static_cast<std::uint64_t>(rle_ptr[i]) * vec_val) & mask;
      }
    }
  }
}

void partitioned_mat_vec2(const skimdb_matrix& mat,
                          std::span<const std::uint64_t> vec,
                          std::span<std::uint64_t> dst,
                          std::size_t log_q,
                          std::size_t start,
                          std::size_t count,
                          std::size_t rle_blocks) {
  std::size_t m_cols = 0;
  std::tie(std::ignore, m_cols) = mat.dimensions();

  const std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::size_t p = start; p < start + count; ++p) {
    std::span<std::uint64_t> out = dst.subspan(p * rle_blocks, rle_blocks);

    for (std::size_t j = 0; j < m_cols; ++j) {
      auto rle = mat.rle_in_col(p, j);

      const std::uint16_t* rle_ptr = rle.data();
      const std::size_t len = rle.size();

      auto vec_val = vec[j];

      for (std::size_t i = 0; i < len; ++i) {
        out[i] += (static_cast<std::uint64_t>(rle_ptr[i]) * vec_val) & mask;
      }
    }
  }
}

void partitioned_mat_vec3(const skimdb_matrix& mat,
                          std::span<const std::uint64_t> vec,
                          std::span<std::uint64_t> dst,
                          std::size_t log_q,
                          std::size_t start,
                          std::size_t count,
                          std::size_t rle_blocks) {
  std::size_t m_cols = 0;
  std::tie(std::ignore, m_cols) = mat.dimensions();
  const std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::size_t p = start; p < start + count; ++p) {
    std::span<std::uint64_t> partition_dst = dst.subspan(p * rle_blocks, rle_blocks);

    for (std::size_t j = 0; j < m_cols; ++j) {
      auto rle = mat.rle_in_col(p, j);
      const auto* rle_ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
      const std::size_t len = rle.size() * 2;
      const std::size_t full_blocks = len / 3;
      auto vec_val = vec[j];

#pragma omp simd
      for (std::size_t i = 0; i < full_blocks; ++i) {
        partition_dst[i] +=
            (static_cast<std::uint64_t>(rle_ptr[i * 3]) << 16 | static_cast<std::uint64_t>(rle_ptr[i * 3 + 1]) << 8 |
             static_cast<std::uint64_t>(rle_ptr[i * 3 + 2])) *
                vec_val &
            mask;
      }

      if (full_blocks * 3 < len) {
        std::uint64_t val = 0;
        for (std::size_t i = 0; i < 3; ++i) {
          val = (full_blocks * 3 + i < len) ? (val << 8) | rle_ptr[full_blocks * 3 + i] : (val << 8);
        }
        partition_dst[full_blocks] += (val * vec_val) & mask;
      }
    }
  }
}

// TODO: looks overly complicated - this is also what we would like to offload to GPU
void partitioned_mat_vec(const skimdb_matrix& mat,
                         std::span<const std::uint64_t> vec,
                         std::span<std::uint64_t> dst,
                         std::size_t log_q,
                         std::size_t start,
                         std::size_t count,
                         std::size_t rle_blocks) {
  const std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  switch (mat.block_size()) {
  case 1: {
    partitioned_mat_vec1(mat, vec, dst, log_q, start, count, rle_blocks);
    break;
  }
  case 2: {
    partitioned_mat_vec2(mat, vec, dst, log_q, start, count, rle_blocks);
    break;
  }
  case 3: {
    partitioned_mat_vec3(mat, vec, dst, log_q, start, count, rle_blocks);
    break;
  }
  default:
    [[unlikely]] {
      g_log->error("impossible case, block size {}", mat.block_size());
      break;
    }
  }

#pragma omp parallel for simd schedule(static)
  for (std::size_t i = 0; i < dst.size(); ++i) {
    dst[i] &= mask;
  }
}


inline auto mat_vec(const skimdb_matrix& db, const spir_matrix& vec, std::size_t log_q, std::size_t rle_blocks) -> spir_matrix {
  LogFun lf{"mat_vec(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  spir_matrix out{db_r, log_q};

  partitioned_mat_vec(db, vec.span(), out.span(), log_q, 0, db_r / rle_blocks, rle_blocks);

  return out;
}


// server setup (hint_c = DB*A)
inline auto mat_mul(const skimdb_matrix& db, const spir_matrix& mat_a, std::size_t log_q, std::size_t rle_blocks) -> spir_matrix {
  LogFun lf{"mat_mul(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  auto [a_r, a_c] = mat_a.dimensions();

  auto trans_a = mat_a.transpose();
  spir_matrix out{a_c, db_r, log_q};

  for (std::size_t i = 0; i < a_c; ++i) {
    partitioned_mat_vec(db, trans_a.row(i), out.row(i), log_q, 0, db_r / rle_blocks, rle_blocks);
  }

  return out.transpose();
}

} // namespace skim::spir

#endif // SKIMDB_SPIR_MATRIX_H
