#ifndef SKIMDB_SIPER_MATRIX_H
#define SKIMDB_SIPER_MATRIX_H

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


namespace skim::siper {

// implements matrices (and vectors) with modular arithmetic
class siper_matrix {
public:
  explicit siper_matrix() = default;

  explicit siper_matrix(std::size_t rows, std::size_t cols, std::size_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_(rows * cols, 0) {}

  explicit siper_matrix(std::size_t n, std::size_t log_mod) : siper_matrix(1, n, log_mod) {}

  explicit siper_matrix(std::vector<std::uint64_t>&& data, std::size_t rows, std::size_t cols, std::size_t log_mod)
      : r_{rows}, c_{cols}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_{std::move(data)} {}

  explicit siper_matrix(std::vector<std::uint64_t>&& data, std::size_t n, std::size_t log_mod)
      : siper_matrix(std::move(data), 1, n, log_mod) {}


  void set(std::size_t i, std::size_t j, std::uint64_t x) { data_[i * c_ + j] = x & mask_; }

  void set(std::size_t i, std::uint64_t x) { data_[i] = x & mask_; }

  [[nodiscard]] auto get(std::size_t i, std::size_t j) const -> std::uint64_t { return data_[i * c_ + j]; }

  [[nodiscard]] auto get(std::size_t i) const -> std::uint64_t { return data_[i]; }


  [[nodiscard]] auto span() -> std::span<std::uint64_t> { return data_; }

  [[nodiscard]] auto span() const -> std::span<const std::uint64_t> { return data_; }

  [[nodiscard]] auto row(std::size_t i) -> std::span<std::uint64_t> { return std::span{data_}.subspan(i * c_, c_); }

  [[nodiscard]] auto row(std::size_t i) const -> std::span<const std::uint64_t> {
    return std::span{data_}.subspan(i * c_, c_);
  }


  [[nodiscard]] auto dimensions() const -> std::tuple<std::size_t, std::size_t> { return std::make_tuple(r_, c_); }


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


  auto add(const siper_matrix& mat) -> siper_matrix& {
    LogFun lf{"siper_matrix::add(...)", spdlog::level::debug};

    auto* dst = data_.data();
    std::size_t n = data_.size();
    const auto* src = mat.data_.data();

    // vectorization does not work
#pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = (dst[i] + src[i]) & mask_;
    }

    return *this;
  }


  auto div_delta(std::size_t log_delta) -> siper_matrix& {
    LogFun lf{"siper_matrix::div_delta(...)", spdlog::level::debug};

    auto* dst = data_.data();
    std::size_t n = data_.size();
    std::uint64_t half_delta = (log_delta == 0) ? 0ull : (1ull << (log_delta - 1));

    // vectorization does not work
#pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = ((dst[i] + half_delta) & mask_) >> log_delta;
    }

    return *this;
  }


  [[nodiscard]] auto transpose() const -> siper_matrix {
    LogFun lf{"siper_matrix::transpose()", spdlog::level::debug};

    const auto* src = data_.data();

    siper_matrix out{c_, r_, log_mod_};
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
  void save(Archive& ar) const {
    std::size_t size{data_.size()};
    ar(r_, c_, log_mod_, mask_, size);
    ar(cereal::binary_data(data_.data(), size * sizeof(std::uint64_t)));
  }

  template <typename Archive>
  void load(Archive& ar) {
    std::size_t size{0};
    ar(r_, c_, log_mod_, mask_, size);
    data_.resize(size);
    ar(cereal::binary_data(data_.data(), size * sizeof(std::uint64_t)));
  }


private:
  std::size_t r_{0};
  std::size_t c_{0};

  std::size_t log_mod_{0};

  std::uint64_t mask_{0};

  std::vector<std::uint64_t> data_; // row-major flat storage
};


void mat_vec(const siper_matrix& mat,
             std::span<const std::uint64_t> vec,
             std::span<std::uint64_t> dst,
             std::size_t log_q) {
  std::size_t m_rows = 0; // declared explicitely for libomp
  std::size_t m_cols = 0;

  std::tie(m_rows, m_cols) = mat.dimensions();
  auto mat_data = mat.span();

  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < m_rows; ++i) {
    std::uint64_t sum = 0;

#pragma omp simd reduction(+ : sum)
    for (std::size_t j = 0; j < m_cols; ++j) {
      sum += (mat_data[i * m_cols + j] * vec[j]) & mask;
    }

    dst[i] = sum & mask;
  }
}


// client side, prepare query (A*s)
inline auto mat_vec(const siper_matrix& mat, const siper_matrix& vec, std::size_t log_q) -> siper_matrix {
  LogFun lf{"mat_vec(siper_matrix, ...)", spdlog::level::debug};

  auto [m_rows, _] = mat.dimensions();
  siper_matrix out{m_rows, log_q};

  mat_vec(mat, vec.span(), out.span(), log_q);

  return out;
}


// client side, answer recovery: computing (ans - hint_c*s) for a range of blocks making up the target rle.
inline auto sub_mat_vec_rows(const siper_matrix& ans,
                             const siper_matrix& hint,
                             std::span<const std::uint64_t> s_data,
                             std::size_t log_q,
                             std::size_t i_start,
                             std::size_t n_rows) -> siper_matrix {
  LogFun lf{"sub_mat_vec_rows(...)", spdlog::level::debug};

  std::size_t r = 0;
  std::size_t c = 0;

  std::tie(r, c) = hint.dimensions();
  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  const auto h_data = hint.span();

  siper_matrix out{n_rows, log_q};

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < n_rows; ++i) {
    std::uint64_t sum = 0;

#pragma omp simd reduction(+ : sum)
    for (std::size_t j = 0; j < c; ++j) {
      sum += (h_data[(i_start + i) * c + j] * s_data[j]) & mask;
    }

    sum &= mask;
    out.set(i, ans.get(i_start + i) - sum);
  }

  return out;
}


class skimdb_matrix {
public:
  explicit skimdb_matrix() = default;

  explicit skimdb_matrix(std::vector<std::uint16_t>&& data, std::size_t block_size, std::size_t sqrt_N)
      : data_{std::move(data)}, block_size_{block_size}, sqrt_N_{sqrt_N} {}


  [[nodiscard]] auto span() -> std::span<std::uint16_t> { return data_; }

  [[nodiscard]] auto span() const -> std::span<const std::uint16_t> { return data_; }


  [[nodiscard]] auto block_size() const -> std::size_t { return block_size_; }

  [[nodiscard]] auto dimensions() const -> std::tuple<std::size_t, std::size_t> {
    return std::make_tuple(sqrt_N_, sqrt_N_);
  }


  template <typename Archive>
  void save(Archive& ar) const {
    std::size_t size{data_.size()};
    ar(block_size_, sqrt_N_, size);
    ar(cereal::binary_data(data_.data(), size * sizeof(std::uint16_t)));
  }

  template <typename Archive>
  void load(Archive& ar) {
    std::size_t size{0};
    ar(block_size_, sqrt_N_, size);
    data_.resize(size);
    ar(cereal::binary_data(data_.data(), size * sizeof(std::uint16_t)));
  }


private:
  std::vector<std::uint16_t> data_; // row major flat storage
  std::size_t block_size_{0};       // number of runs stored in each block
  std::size_t sqrt_N_{0};           // runs per row and column, should be multiple of block_size
};


void partitioned_mat_vec(const skimdb_matrix& mat,
                         std::span<const uint64_t> vec,
                         std::span<uint64_t> dst,
                         std::size_t log_q,
                         std::size_t start,
                         std::size_t count) {
  LogFun lf{"partitioned_mat_vec(skimdb_matrix, ...)", spdlog::level::debug};

  std::size_t m_cols = 0;
  std::tie(std::ignore, m_cols) = mat.dimensions();

  auto src = mat.span().subspan(start * m_cols, count * m_cols);

  const uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  const auto* __restrict__ s = src.data();
  const uint64_t* __restrict__ v = vec.data();
  uint64_t* __restrict__ d = dst.data();

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < count; ++i) {
    const auto* row = s + i * m_cols;
    uint64_t sum = 0;

#pragma omp simd reduction(+ : sum)
    for (std::size_t j = 0; j < m_cols; ++j) {
      sum += row[j] * v[j];
    }

    d[i] = sum & mask;
  }
}


inline auto mat_vec(const skimdb_matrix& db, const siper_matrix& vec, std::size_t log_q) -> siper_matrix {
  LogFun lf{"mat_vec(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  siper_matrix out{db_r, log_q};

  partitioned_mat_vec(db, vec.span(), out.span(), log_q, 0, db_r);

  return out;
}


// server setup (hint_c = DB*A)
inline auto mat_mul(const skimdb_matrix& db, const siper_matrix& mat_a, std::size_t log_q) -> siper_matrix {
  LogFun lf{"mat_mul(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  auto [a_r, a_c] = mat_a.dimensions();

  auto trans_a = mat_a.transpose();
  siper_matrix out{a_c, db_r, log_q};

  for (std::size_t i = 0; i < a_c; ++i) {
    partitioned_mat_vec(db, trans_a.row(i), out.row(i), log_q, 0, db_r);
  }

  return out.transpose();
}

} // namespace skim::siper

#endif // SKIMDB_SIPER_MATRIX_H
