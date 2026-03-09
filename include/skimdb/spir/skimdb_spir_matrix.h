#ifndef SKIMDB_SPIR_MATRIX_H
#define SKIMDB_SPIR_MATRIX_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <cereal/types/vector.hpp>

#include <dgpp/uniform_rejection.hpp>

#include <skimdb/detail/skimdb_encoding.h>
#include <skimdb/detail/skimdb_logger.h>


namespace skim {
namespace spir {

// implements matrices (and vectors) with modular arithmetic
class spir_matrix {
public:
  explicit spir_matrix() = default;

  explicit spir_matrix(std::uint64_t rows, std::uint64_t cols, std::uint32_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_(rows * cols, 0) {}

  explicit spir_matrix(std::uint64_t n, std::uint32_t log_mod) : spir_matrix(1, n, log_mod) {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::uint64_t rows, std::uint64_t cols, std::uint32_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_{std::move(data)} {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::uint64_t n, std::uint32_t log_mod)
      : spir_matrix(std::move(data), 1, n, log_mod) {}


  void set(std::uint64_t i, std::uint64_t j, std::uint64_t x) { data_[i * c_ + j] = x & mask_; }

  void set(std::uint64_t i, std::uint64_t x) { data_[i] = x & mask_; }

  auto get(std::uint64_t i, std::uint64_t j) const -> std::uint64_t { return data_[i * c_ + j]; }

  auto get(std::uint64_t i) const -> std::uint64_t { return data_[i]; }


  auto span() -> std::span<std::uint64_t> { return data_; }

  auto span() const -> std::span<const std::uint64_t> { return data_; }

  auto row(std::size_t i) -> std::span<std::uint64_t> { return std::span{data_}.subspan(i * c_, c_); }

  auto row(std::size_t i) const -> std::span<const std::uint64_t> { return std::span{data_}.subspan(i * c_, c_); }


  auto dimensions() const -> std::tuple<std::uint64_t, std::uint64_t> { return std::make_tuple(r_, c_); }


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


  auto div_delta(std::uint64_t log_delta) -> spir_matrix& {
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
    for (std::uint64_t i = 0; i < r_; ++i) {
      for (std::uint64_t j = 0; j < c_; ++j) {
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
  std::uint64_t r_;
  std::uint64_t c_;

  std::uint32_t log_mod_;
  std::uint64_t mask_;

  std::vector<std::uint64_t> data_; // row-major flat storage
};


void mat_vec(const spir_matrix&mat, std::span<const std::uint64_t> vec, std::span<std::uint64_t> dst, std::uint32_t log_q) {
  auto [m_rows, m_cols] = mat.dimensions();
  auto mat_data = mat.span();

  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::uint64_t i = 0; i < m_rows; ++i) {
    std::uint64_t sum = 0;

  #pragma omp simd reduction(+:sum)
    for (std::uint64_t j = 0; j < m_cols; ++j) {
      sum += (mat_data[i * m_cols + j] * vec[j]) & mask;
    }

    dst[i] = sum & mask;
  }
}


// client side, prepare query (A*s)
auto mat_vec(const spir_matrix& mat, const spir_matrix& vec, std::uint32_t log_q) -> spir_matrix {
  LogFun lf{"mat_vec(spir_matrix, ...)", spdlog::level::debug};

  auto [m_rows, _] = mat.dimensions();
  spir_matrix out{m_rows, log_q};

  mat_vec(mat, vec.span(), out.span(), log_q);

  return out;
}


// client side, answer recovery: computing (ans - hint_c*s) for a range of blocks making up the target rle.
auto sub_mat_vec_rows(const spir_matrix &ans, const spir_matrix &hint, std::span<const std::uint64_t> s_data,
    std::uint32_t log_q, std::uint64_t i_start, std::uint64_t n_rows) -> spir_matrix {
  LogFun lf{"sub_mat_vec_rows(...)", spdlog::level::debug};

  auto [r, c] = hint.dimensions();
  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  const auto h_data = hint.span();

  spir_matrix out{n_rows, log_q};

#pragma omp parallel for schedule(static)
  for (std::uint64_t i = 0; i < n_rows; ++i) {
    std::uint64_t sum = 0;

  #pragma omp simd reduction(+:sum)
    for (std::uint64_t j = 0; j < c; ++j) {
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
                         std::uint32_t block_len,
                         std::uint64_t rle_blocks,
                         std::uint64_t sqrt_N)
    : data_{std::move(data)}, block_len_{block_len}, rle_blocks_{rle_blocks}, sqrt_N_{sqrt_N} {}

  auto get_block_len() const -> std::uint32_t { return block_len_; }

  auto get_rle_in_col(std::uint64_t n, std::uint64_t col) const -> std::span<const std::uint16_t> {
    std::uint64_t kmer_idx = (col * sqrt_N_) / rle_blocks_ + n;
    if (kmer_idx >= data_.size()) [[unlikely]] return {};
    return data_[kmer_idx].span();
  }

  auto dimensions() const -> std::tuple<std::uint64_t, std::uint64_t> { return std::make_tuple(sqrt_N_, sqrt_N_); }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(data_, block_len_, rle_blocks_, sqrt_N_);
  }

private:
  std::vector<detail::encoding> data_;
  std::uint32_t block_len_;   // bytes of plaintext data we can pack into one block
  std::uint64_t rle_blocks_;  // blocks needed per RLE encoding
  std::uint64_t sqrt_N_;      // matrix side length (blocks of data)
};


void partitioned_mat_vec(const skimdb_matrix& mat, std::span<const std::uint64_t> vec, std::span<std::uint64_t> dst, 
    std::uint32_t log_q, std::uint32_t start, std::uint32_t count, std::uint64_t rle_blocks) {
  auto [_, m_cols] = mat.dimensions();
  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::uint64_t p = start; p < start + count; ++p) {
    std::span<std::uint64_t> partition_dst = dst.subspan(p * rle_blocks, rle_blocks);

    switch (mat.get_block_len()) {
      case 1: {
        for (std::uint64_t j = 0; j < m_cols; ++j) {
          auto rle = mat.get_rle_in_col(p, j);
          const std::uint8_t* rle_ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
          const std::uint64_t len = rle.size() * 2;
          auto vec_val = vec[j];

        #pragma omp simd
          for (std::uint64_t i = 0; i < len; ++i) {
            partition_dst[i] += (static_cast<std::uint64_t>(rle_ptr[i]) * vec_val) & mask;
          }
        }

        break;
      }
      case 2: {
        for (std::uint64_t j = 0; j < m_cols; ++j) {
          auto rle = mat.get_rle_in_col(p, j);
          const std::uint16_t* rle_ptr = rle.data();
          const std::uint64_t len = rle.size();
          auto vec_val = vec[j];

        #pragma omp simd
          for (std::uint64_t i = 0; i < len; ++i) {
            partition_dst[i] += (static_cast<std::uint64_t>(rle_ptr[i]) * vec_val) & mask;
          }
        }

        break;
      }
      case 3: {
        for (std::uint64_t j = 0; j < m_cols; ++j) {
          auto rle = mat.get_rle_in_col(p, j);
          const std::uint8_t* rle_ptr = reinterpret_cast<const std::uint8_t*>(rle.data());
          const std::uint64_t len = rle.size() * 2;
          const std::uint64_t full_blocks = len / 3;
          auto vec_val = vec[j];

        #pragma omp simd
          for (std::uint64_t i = 0; i < full_blocks; ++i) {
            partition_dst[i] += (static_cast<std::uint64_t>(rle_ptr[i*3]) << 16
                                | static_cast<std::uint64_t>(rle_ptr[i*3 + 1]) << 8
                                | static_cast<std::uint64_t>(rle_ptr[i*3 + 2])) * vec_val & mask;
          }

          if (full_blocks * 3 < len) {
            std::uint64_t val = 0;
            for (auto i = 0; i < 3; ++i) {
              val = (full_blocks * 3 + i < len) ? (val << 8) | rle_ptr[full_blocks * 3 + i] : (val << 8);
            }
            partition_dst[full_blocks] += (val * vec_val) & mask;
          }
        }

        break;
      }
      default: [[unlikely]] {
        g_log->error("impossible case, block length {}", mat.get_block_len());
        break;
      }
    }
  }

#pragma omp parallel for simd schedule(static)
  for (std::uint64_t i = 0; i < dst.size(); ++i) {
    dst[i] &= mask;
  }
}


auto mat_vec(const skimdb_matrix& db, const spir_matrix& vec, std::uint32_t log_q, std::uint64_t rle_blocks) -> spir_matrix {
  LogFun lf{"mat_vec(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  spir_matrix out{db_r, log_q};

  partitioned_mat_vec(db, vec.span(), out.span(), log_q, 0, db_r / rle_blocks, rle_blocks);

  return out;
}


// server setup (hint_c = DB*A)
auto mat_mul(const skimdb_matrix& db, const spir_matrix& mat_a, std::uint32_t log_q, std::uint64_t rle_blocks) -> spir_matrix {
  LogFun lf{"mat_mul(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  auto [a_r, a_c] = mat_a.dimensions();

  auto trans_a = mat_a.transpose();
  spir_matrix out{a_c, db_r, log_q};

  for (std::uint64_t i = 0; i < a_c; ++i) {
    partitioned_mat_vec(db, trans_a.row(i), out.row(i), log_q, 0, db_r / rle_blocks, rle_blocks);
  }

  return out.transpose();
}

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_MATRIX_H
