#ifndef SKIMDB_SPIR_MATRIX_H
#define SKIMDB_SPIR_MATRIX_H

#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
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

  explicit spir_matrix(std::uint64_t rows, std::uint64_t cols, std::uint64_t log_mod)
      : r_{rows}, c_{cols}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_(rows * cols, 0) {}

  explicit spir_matrix(std::uint64_t n, std::uint64_t log_mod) : spir_matrix(n, 1, log_mod) {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::uint64_t rows, std::uint64_t cols, std::uint64_t log_mod)
      : r_{rows}, c_{cols}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_{std::move(data)} {}

  explicit spir_matrix(std::vector<std::uint64_t>&& data, std::uint64_t rows, std::uint64_t log_mod)
      : spir_matrix(std::move(data), rows, 1, log_mod) {}


  void set(std::uint64_t i, std::uint64_t j, std::uint64_t x) { data_[i * c_ + j] = x & mask_; }

  void set(std::uint64_t i, std::uint64_t x) { data_[i] = x & mask_; }

  auto get(std::uint64_t i, std::uint64_t j) const -> std::uint64_t { return data_[i * c_ + j]; }

  auto get(std::uint64_t i) const -> std::uint64_t { return data_[i]; }

  auto vec() -> std::vector<std::uint64_t>& { return data_; }

  auto vec() const -> const std::vector<std::uint64_t>& { return data_; }

  auto data() -> std::uint64_t* { return data_.data(); }

  auto data() const -> const std::uint64_t* { return data_.data(); }


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


  auto sub(const spir_matrix& mat) -> spir_matrix& {
    LogFun lf{"spir_matrix::sub(...)"};

    auto* dst = data_.data();
    const auto* src = mat.data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] - src[i]) & mask_;
    }

    return *this;
  }


  auto add(const spir_matrix& mat) -> spir_matrix& {
    LogFun lf{"spir_matrix::add(...)"};

    auto* dst = data_.data();
    const auto* src = mat.data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] + src[i]) & mask_;
    }

    return *this;
  }

  auto add(std::uint64_t scalar) -> spir_matrix& {
    LogFun lf{"spir_matrix::add(scalar)"};

    auto* dst = data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] + scalar) & mask_;
    }

    return *this;
  }


  auto mul(std::uint64_t scalar) -> spir_matrix& {
    LogFun lf{"spir_matrix::mul(...)"};

    auto* dst = data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] * scalar) & mask_;
    }

    return *this;
  }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(r_, c_, mask_, data_);
  }

private:
  std::uint64_t r_;
  std::uint64_t c_;
  std::uint64_t mask_;              // precomputed mask
  std::vector<std::uint64_t> data_; // row-major flat storage
};


// wrapper around skim::detail::encoding to provide matrix-like access
// TODO: will want to improve this later to account for access patterns
/* Expected sizes:
 *                | log_p  |   kemrs  | max_rle | rle_per_col | sqrt_N |     N 
 *  viral20250425 |   16   | 63512373 |   590   |     329     | 194110 | 37678692100
 */ 
class skimdb_matrix {
public:
  explicit skimdb_matrix() = default;

  explicit skimdb_matrix(std::vector<skim::detail::encoding>&& data,
                         std::uint64_t log_p,
                         std::uint64_t rle_blocks,
                         std::uint64_t sqrt_N)
    : data_{std::move(data)}, log_p_{log_p}, rle_blocks_{rle_blocks}, sqrt_N_{sqrt_N} {}

  auto get(std::uint64_t i, std::uint64_t j) const -> std::uint64_t {
    const std::uint64_t col_maj = j * sqrt_N_ + i;

    const std::uint64_t kmer_idx = col_maj / rle_blocks_;
    const std::uint64_t in_block = col_maj - kmer_idx * rle_blocks_;

    if (kmer_idx >= data_.size()) [[unlikely]] {
      return 0;
    }

    const auto& block = data_[kmer_idx];
    const std::uint64_t offset = (log_p_ == 8) ? (in_block >> 1) : in_block;

    if (offset >= block.length()) [[unlikely]] {
      return 0;
    }

    std::uint64_t val = static_cast<std::uint64_t>(block.get(offset));

    if (log_p_ == 8) {
      const std::uint64_t shift = (in_block & 1ull) << 3; // 0 or 8
      val = (val >> (8 - shift)) & 0xFF;
    }

    return val;
  }

  auto get_rle_in_col(std::uint64_t n, std::uint64_t col) const -> std::span<const std::uint16_t> {
    std::uint64_t kmer_idx = (col * sqrt_N_) / rle_blocks_ + n;
    if (kmer_idx >= data_.size()) [[unlikely]] return {};
    return data_[kmer_idx].span();
  }

  auto dimensions() const -> std::tuple<std::uint64_t, std::uint64_t> { return std::make_tuple(sqrt_N_, sqrt_N_); }

  template <typename Archive>
  void serialize(Archive& archive) {
    archive(data_, log_p_, rle_blocks_, sqrt_N_);
  }

private:
  std::vector<skim::detail::encoding> data_;
  std::uint64_t log_p_;       // log of plaintext modulus
  std::uint64_t rle_blocks_;  // blocks needed per RLE encoding
  std::uint64_t sqrt_N_;      // matrix side length (blocks of data)
};


// client side, prepare query
inline auto mat_vec(const spir_matrix& mat, const spir_matrix& vec, std::uint64_t log_q) -> spir_matrix {
  LogFun lf{"mat_vec(spir_matrix, ...)"};

  auto [m_rows, m_cols] = mat.dimensions();
  auto [v_rows, v_cols] = vec.dimensions();

  spir_matrix out{m_rows, 1, log_q};

  for (std::uint64_t i = 0; i < m_rows; ++i) {
    std::uint64_t sum = 0;
    for (std::uint64_t j = 0; j < m_cols; ++j) {
      sum += mat.get(i, j) * vec.get(j);
    }
    out.set(i, sum);
  }

  return out;
}

// server side, prepare response
// TODO - this currently only handles p = 16, will need to adjust for p = 8 case
          // can just cast uint16_t to uint8_t but then client and server must agree on endianess
inline auto mat_vec(const skimdb_matrix& mat, const spir_matrix& vec, std::uint64_t log_p, 
    std::uint64_t log_q, std::uint64_t rle_blocks) -> spir_matrix {
  LogFun lf{"mat_vec(skimdb_matrix, ...)"};

  auto [m_rows, m_cols] = mat.dimensions();
  spir_matrix out{m_rows, log_q};
  std::uint64_t* out_data = out.data();
  std::uint64_t partitions = m_rows / rle_blocks;

#pragma omp parallel for schedule(static)
  for (std::uint64_t p = 0; p < partitions; ++p) {
    std::uint64_t* partition_data = out_data + p * rle_blocks;

    for (std::uint64_t j = 0; j < m_cols; ++j) {
      auto rle = mat.get_rle_in_col(p, j);
      const std::uint16_t* rle_ptr = rle.data();
      const std::uint64_t len = rle.size();
      auto vec_val = vec.get(j);

#pragma omp simd
      for (std::uint64_t i = 0; i < len; ++i) {
        partition_data[i] += static_cast<std::uint64_t>(rle_ptr[i]) * vec_val;
      }
    }
  }

  return out;
}

// minor: used in server setup (large matrices)
inline auto mat_mul(const skimdb_matrix& mat_a, const spir_matrix& mat_b, std::uint64_t log_q) -> spir_matrix {
  LogFun lf{"mat_mul(skimdb_matrix, ...)"};

  auto [a_rows, a_cols] = mat_a.dimensions();
  auto [b_rows, b_cols] = mat_b.dimensions();

  spir_matrix out{a_rows, b_cols, log_q};

#pragma omp parallel
  {
    std::unique_ptr<std::uint64_t[]> acc(new std::uint64_t[a_rows]); // probably faster than std::vector

#pragma omp for schedule(static)
    for (std::uint64_t j = 0; j < b_cols; ++j) {
      std::memset(acc.get(), 0, a_rows * sizeof(std::uint64_t)); // probably faster than std::fill

      for (std::uint64_t k = 0; k < a_cols; ++k) {
        const std::uint64_t bkj = mat_b.get(k, j);

        // could be good trick?
        // if (bkj == 0) {
        // continue;
        // }

#pragma omp simd
        for (std::uint64_t i = 0; i < a_rows; ++i) {
          acc[i] += mat_a.get(i, k) * bkj;
        }
      }

      for (std::uint64_t i = 0; i < a_rows; ++i) {
        out.set(i, j, acc[i]);
      }
    }
  }

  return out;
}

// client side, answer recovery
inline auto vec_mul(const spir_matrix& vec_a, const spir_matrix& vec_b) -> std::uint64_t {
  LogFun lf{"vec_mul(spir_matrix, ...)"};

  auto [a_rows, a_cols] = vec_a.dimensions();
  auto [b_rows, b_cols] = vec_b.dimensions();

  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < a_rows; ++i) {
    sum += vec_a.get(i) * vec_b.get(i);
  }

  return sum;
}

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_MATRIX_H
