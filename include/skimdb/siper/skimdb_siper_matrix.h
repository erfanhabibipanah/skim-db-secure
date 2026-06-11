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
template <typename T>
class siper_matrix {
  static_assert(std::is_same_v<T, std::uint64_t> || std::is_same_v<T, const std::uint64_t>);

  // TODO: add concepts to differentiate owning and non-owning methods

public:
  explicit siper_matrix() = default;

  // owning constructors
  siper_matrix(std::size_t nrow, std::size_t ncol, std::size_t log_mod)
      : nrow_{nrow}, ncol_{ncol}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_owned_(nrow * ncol, 0) {
    data_ = data_owned_.data();
    size_ = data_owned_.size();
  }

  siper_matrix(std::size_t ncol, std::size_t log_mod) : siper_matrix(1, ncol, log_mod) {}

  siper_matrix(std::vector<std::uint64_t>&& data, std::size_t nrow, std::size_t ncol, std::size_t log_mod)
      : nrow_{nrow}, ncol_{ncol}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_owned_{std::move(data)} {
    data_ = data_owned_.data();
    size_ = data_owned_.size();
  }

  siper_matrix(std::vector<std::uint64_t>&& data, std::size_t ncol, std::size_t log_mod)
      : siper_matrix(std::move(data), 1, ncol, log_mod) {}

  // non-owning constructors
  siper_matrix(T* data, std::size_t nrow, std::size_t ncol, std::size_t log_mod)
      : nrow_{nrow}, ncol_{ncol}, log_mod_{log_mod}, mask_{(log_mod >= 64) ? ~0ull : ((1ull << log_mod) - 1)},
        data_{data}, size_{nrow * ncol} {}

  siper_matrix(std::span<T> data, std::size_t nrow, std::size_t ncol, std::size_t log_mod)
      : siper_matrix(data.data(), nrow, ncol, log_mod) {}


  operator siper_matrix<const std::uint64_t>() const {
    return siper_matrix<const std::uint64_t>(data_, nrow_, ncol_, log_mod_);
  }


  void set(std::size_t i, std::size_t j, std::uint64_t x) { data_[i * ncol_ + j] = x & mask_; }

  void set(std::size_t i, std::uint64_t x) { data_[i] = x & mask_; }

  [[nodiscard]] auto get(std::size_t i, std::size_t j) const -> std::uint64_t { return data_[i * ncol_ + j]; }

  [[nodiscard]] auto get(std::size_t i) const -> std::uint64_t { return data_[i]; }


  [[nodiscard]] auto span() noexcept -> std::span<T> { return {data_, size_}; }

  [[nodiscard]] auto span() const noexcept -> std::span<const std::uint64_t> { return {data_, size_}; }


  [[nodiscard]] auto row(std::size_t i) noexcept -> std::span<T> { return {data_ + i * ncol_, ncol_}; }

  [[nodiscard]] auto row(std::size_t i) const noexcept -> std::span<const std::uint64_t> {
    return {data_ + i * ncol_, ncol_};
  }


  [[nodiscard]] auto constexpr dimensions() const noexcept -> std::tuple<std::size_t, std::size_t> {
    return std::make_tuple(nrow_, ncol_);
  }


  template <typename URBG>
  void fill(URBG&& rng) {
    std::uniform_int_distribution<std::uint64_t> dist(0, mask_);
    for (std::size_t i = 0; i < size_; ++i) {
      data_[i] = dist(rng);
    }
  }

  template <typename URBG>
  void fill(URBG&& rng, dgpp::uniform_rejection& dist) {
    for (std::size_t i = 0; i < size_; ++i) {
      data_[i] = dist(rng) & mask_;
    }
  }


  auto add(const siper_matrix& mat) -> siper_matrix& {
    LogFun lf{"siper_matrix::add(...)", spdlog::level::debug};

    auto* __restrict dst = data_;
    const auto* __restrict src = mat.data_;

    const std::size_t n = size_;
    const std::uint64_t mask = mask_;

#pragma omp parallel for simd schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      dst[i] = (dst[i] + src[i]) & mask;
    }

    return *this;
  }

  auto div_delta(std::size_t log_delta) -> siper_matrix& {
    LogFun lf{"siper_matrix::div_delta(...)", spdlog::level::debug};

    auto* __restrict dst = data_;

    const std::size_t n = size_;
    const std::uint64_t mask = mask_;

    const std::uint64_t half_delta = (log_delta == 0) ? 0ull : (1ull << (log_delta - 1));

#pragma omp parallel for simd schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
      std::uint64_t v = dst[i] + half_delta;
      v = (v >> log_delta);
      dst[i] = v & mask;
    }

    return *this;
  }

  [[nodiscard]] auto transpose() const -> siper_matrix {
    LogFun lf{"siper_matrix::transpose()", spdlog::level::debug};

    const auto* src = data_;

    siper_matrix out{ncol_, nrow_, log_mod_};
    auto dst = out.span();

#pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < nrow_; ++i) {
      for (std::size_t j = 0; j < ncol_; ++j) {
        dst[j * nrow_ + i] = src[i * ncol_ + j];
      }
    }

    return out;
  }


  template <typename Archive>
  void save(Archive& ar) const {
    ar(nrow_, ncol_, log_mod_, mask_, size_);
    ar(cereal::binary_data(data_, size_ * sizeof(std::uint64_t)));
  }

  template <typename Archive>
  void load(Archive& ar) {
    ar(nrow_, ncol_, log_mod_, mask_, size_);
    data_owned_.resize(size_);
    data_ = data_owned_.data();
    ar(cereal::binary_data(data_, size_ * sizeof(std::uint64_t)));
  }

private:
  std::size_t nrow_{0};
  std::size_t ncol_{0};

  std::size_t log_mod_{0};

  std::uint64_t mask_{0};

  // row-major flat storage
  T* data_{nullptr};
  std::size_t size_{0};

  std::vector<std::uint64_t> data_owned_;
};


class skimdb_matrix {
public:
  explicit skimdb_matrix() = default;

  skimdb_matrix(std::vector<std::uint16_t>&& data, std::size_t block_size, std::size_t sqrt_N)
      : data_{std::move(data)}, block_size_{block_size}, sqrt_N_{sqrt_N} {}


  [[nodiscard]] auto span() noexcept -> std::span<std::uint16_t> { return data_; }

  [[nodiscard]] auto span() const noexcept -> std::span<const std::uint16_t> { return data_; }


  [[nodiscard]] auto constexpr block_size() const -> std::size_t { return block_size_; }

  [[nodiscard]] auto constexpr dimensions() const -> std::tuple<std::size_t, std::size_t> {
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


inline auto mat_vec(const skimdb_matrix& db, std::span<const std::uint64_t> vec, std::size_t log_q)
    -> siper_matrix<std::uint64_t> {
  LogFun lf{"mat_vec(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  siper_matrix<std::uint64_t> out{db_r, log_q};

  partitioned_mat_vec(db, vec, out.span(), log_q, 0, db_r);

  return out;
}

template <typename T>
void mat_vec(const siper_matrix<T>& mat,
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

template <typename T, typename U>
inline auto mat_vec(const siper_matrix<T>& mat, const siper_matrix<U>& vec, std::size_t log_q)
    -> siper_matrix<std::uint64_t> {
  LogFun lf{"mat_vec(siper_matrix, ...)", spdlog::level::debug};

  auto [m_rows, _] = mat.dimensions();
  siper_matrix<std::uint64_t> out{m_rows, log_q};

  mat_vec(mat, vec.span(), out.span(), log_q);

  return out;
}


// client side, answer recovery: computing (ans - hint_c * s) for a range of blocks making up the target rle.
template <typename T, typename U>
inline auto sub_mat_vec_rows(const siper_matrix<T>& ans,
                             const siper_matrix<U>& hint,
                             std::span<const std::uint64_t> s_data,
                             std::size_t log_q,
                             std::size_t i_start,
                             std::size_t n_rows) -> siper_matrix<std::uint64_t> {
  LogFun lf{"sub_mat_vec_rows(...)", spdlog::level::debug};

  std::size_t r = 0;
  std::size_t c = 0;

  std::tie(r, c) = hint.dimensions();
  std::uint64_t mask = (log_q >= 64) ? ~0ull : ((1ull << log_q) - 1);

  const auto h_data = hint.span();

  siper_matrix<std::uint64_t> out{n_rows, log_q};

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


// server setup (hint_c = DB*A)
template <typename T>
inline auto mat_mul(const skimdb_matrix& db, const siper_matrix<T>& mat_a, std::size_t log_q)
    -> siper_matrix<std::uint64_t> {
  LogFun lf{"mat_mul(skimdb_matrix, ...)", spdlog::level::debug};

  auto [db_r, _] = db.dimensions();
  auto [a_r, a_c] = mat_a.dimensions();

  auto trans_a = mat_a.transpose();
  siper_matrix<std::uint64_t> out{a_c, db_r, log_q};

  for (std::size_t i = 0; i < a_c; ++i) {
    partitioned_mat_vec(db, trans_a.row(i), out.row(i), log_q, 0, db_r);
  }

  return out.transpose();
}

} // namespace skim::siper

#endif // SKIMDB_SIPER_MATRIX_H
