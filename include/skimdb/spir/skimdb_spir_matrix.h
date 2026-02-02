#ifndef SKIMDB_SPIR_MATRIX_H
#define SKIMDB_SPIR_MATRIX_H

#include <cstdint>
#include <execution>
#include <expected>
#include <random>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include <dgpp/uniform_rejection.hpp>

#include <fastxrd/fasta_buffered_reader.h>
#include <fastxrd/fastx_files_reader.h>

#include "skimdb/detail/skimdb_encoding.h"
#include "skimdb/detail/skimdb_logger.h"


namespace skim {
namespace spir {

// implements matrices (and vectors) with modular arithmetic
class spir_matrix {
public:
  explicit spir_matrix(std::uint64_t rows, std::uint64_t cols, std::uint64_t log_mod)
      : r_{rows}, c_{cols}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)}, data_(rows * cols, 0) {}

  explicit spir_matrix(std::uint64_t n, std::uint64_t log_mod) : spir_matrix(n, 1, log_mod) {}


  inline void set(std::uint64_t i, std::uint64_t j, std::uint64_t x) { data_[i * c_ + j] = x & mask_; }

  inline void set(std::uint64_t i, std::uint64_t x) { data_[i] = x & mask_; }

  inline auto get(std::uint64_t i, std::uint64_t j) const -> std::uint64_t { return data_[i * c_ + j]; }

  inline auto get(std::uint64_t i) const -> std::uint64_t { return data_[i]; }


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
    auto* dst = data_.data();
    const auto* src = mat.data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] - src[i]) & mask_;
    }

    return *this;
  }


  auto add(const spir_matrix& mat) -> spir_matrix& {
    auto* dst = data_.data();
    const auto* src = mat.data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] + src[i]) & mask_;
    }

    return *this;
  }

  auto add(std::uint64_t scalar) -> spir_matrix& {
    auto* dst = data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] + scalar) & mask_;
    }

    return *this;
  }


  auto mul(std::uint64_t scalar) -> spir_matrix& {
    auto* dst = data_.data();

    for (std::size_t i = 0, n = data_.size(); i < n; ++i) {
      dst[i] = (dst[i] * scalar) & mask_;
    }

    return *this;
  }

private:
  std::uint64_t r_;
  std::uint64_t c_;
  std::uint64_t mask_;              // precomputed mask
  std::vector<std::uint64_t> data_; // row-major flat storage
};


// wrapper around skim::detail::encoding to provide matrix-like access
// TODO: will want to improve this later to account for access patterns
class skimdb_matrix {
public:
  explicit skimdb_matrix(std::vector<skim::detail::encoding>&& data,
                         std::uint64_t log_p,
                         std::uint64_t rle_blocks,
                         std::uint64_t sqrt_N)
    : data_{std::move(data)}, log_p_{log_p}, rle_blocks_{rle_blocks}, sqrt_N_{sqrt_N} {}

  auto get(std::uint64_t i, std::uint64_t j) const -> std::uint64_t {
    std::uint64_t col_maj  = j * sqrt_N_ + i;
    std::uint64_t kmer_idx = col_maj / rle_blocks_;
    std::uint64_t in_block = col_maj % rle_blocks_;
    std::uint64_t offset   = (log_p_ == 8) ? (in_block / 2) : in_block;

    if (kmer_idx >= data_.size() || offset >= data_[kmer_idx].length()) {
      return 0;
    }

    std::uint64_t val = static_cast<std::uint64_t>(data_[kmer_idx].get(offset));

    if (log_p_ == 8) {
      val = ((in_block & 1ull) == 0) ? ((val >> 8) & 0xFF) : (val & 0xFF);
    }

    return val;
  }

  auto dimensions() const -> std::tuple<std::uint64_t, std::uint64_t> {
    return std::make_tuple(sqrt_N_, sqrt_N_);
  }

private:
  std::vector<skim::detail::encoding> data_;
  std::uint64_t log_p_;       // log of plaintext modulus
  std::uint64_t rle_blocks_;  // blocks needed per RLE encoding
  std::uint64_t sqrt_N_;      // matrix side length (blocks of data)
};


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

inline auto mat_vec(const skimdb_matrix& mat, const spir_matrix& vec, std::uint64_t log_q) -> spir_matrix {
  LogFun lf{"mat_vec(skimdb_matrix, ...)"};

  auto [m_rows, m_cols] = mat.dimensions();
  auto [v_rows, v_cols] = vec.dimensions();

  spir_matrix out{m_rows, log_q};

  for (std::uint64_t i = 0; i < m_rows; ++i) {
    std::uint64_t sum = 0;
    for (std::uint64_t j = 0; j < m_cols; ++j) {
      sum += mat.get(i, j) * vec.get(j);
    }
    out.set(i, sum);
  }

  return out;
}


inline auto mat_mul(const skimdb_matrix& mat_a, const spir_matrix& mat_b, std::uint64_t log_q) -> spir_matrix {
  LogFun lf{"mat_mul(skimdb_matrix, ...)"};

  auto [a_rows, a_cols] = mat_a.dimensions();
  auto [b_rows, b_cols] = mat_b.dimensions();

  spir_matrix out{a_rows, b_cols, log_q};

  for (std::uint64_t j = 0; j < b_cols; ++j) {
    std::vector<std::uint64_t> acc(a_rows, 0);
    for (std::uint64_t k = 0; k < a_cols; ++k) {
      const std::uint64_t bkj = mat_b.get(k, j);
      for (std::uint64_t i = 0; i < a_rows; ++i) {
        acc[i] += mat_a.get(i, k) * bkj;
      }
    }

    for (std::uint64_t i = 0; i < a_rows; ++i) {
      out.set(i, j, acc[i]);
    }
  }

  return out;
}


inline auto vec_mul(const spir_matrix& vec_a, const spir_matrix& vec_b) -> std::uint64_t {
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
