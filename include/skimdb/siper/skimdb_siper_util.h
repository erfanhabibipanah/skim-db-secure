#ifndef SKIMDB_SIPER_UTIL_H
#define SKIMDB_SIPER_UTIL_H

#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "skimdb_siper_matrix.h"


namespace skim::siper {

namespace fs = std::filesystem;

namespace index {

inline constexpr std::uint64_t g_len_bits = 12;
inline constexpr std::uint64_t g_len_mask = (1ULL << g_len_bits) - 1;

inline auto pack(std::uint64_t start, std::uint16_t len) noexcept -> std::uint64_t { return (start << g_len_bits) | len; }

inline auto unpack_start(std::uint64_t v) noexcept -> std::uint64_t { return v >> g_len_bits; }

inline auto unpack_len(std::uint64_t v) noexcept -> std::uint16_t { return static_cast<std::uint16_t>(v & g_len_mask); }

} // namespace index


auto min_sqrt_N(std::size_t min_blocks, std::size_t block_size) -> std::size_t {
  std::size_t min_cells = min_blocks * block_size;
  auto min_side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(min_cells))));
  return (min_side + block_size - 1) / block_size * block_size;
}


auto populate_skimdb_matrix(const std::vector<skim::detail::encoding>& data,
                            const std::vector<std::uint64_t>& metadata,
                            std::size_t sqrt_N,
                            std::size_t block_size) -> skimdb_matrix {
  std::size_t size = sqrt_N * sqrt_N;
  auto packed_data = std::make_unique_for_overwrite<std::uint16_t[]>(size);
  std::memset(packed_data.get(), 0, size * sizeof(std::uint16_t));


#pragma omp parallel for schedule(guided)
  for (std::size_t i = 0; i < size; ++i) {
    auto start_idx = index::unpack_start(metadata[i]);
    auto src = data[i].span();

    std::size_t rle_len{data[i].length()};

    for (std::size_t j = 0; j < rle_len; ++j) {
      std::size_t col_idx = (start_idx + j) / sqrt_N;
      std::size_t row_idx = (start_idx + j) % sqrt_N;
      packed_data[row_idx * sqrt_N + col_idx] = src[j];
    }
  }

  return skimdb_matrix{std::move(packed_data), size, block_size, sqrt_N};
}


auto sha256_file(const fs::path& path) -> std::expected<std::string, std::string> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected{"unable to open file"};
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return std::unexpected{"EVP_MD_CTX_new failed"};
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::unexpected{"EVP_DigestInit_ex failed"};
  }

  std::vector<char> buffer(1024 * 1024 * 1024);

  while (file) {
    file.read(buffer.data(), buffer.size());
    std::streamsize bytes = file.gcount();
    if (bytes > 0) {
      EVP_DigestUpdate(ctx, buffer.data(), bytes);
    }
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;

  EVP_DigestFinal_ex(ctx, digest, &digest_len);
  EVP_MD_CTX_free(ctx);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  for (unsigned i = 0; i < digest_len; ++i) {
    oss << std::setw(2) << static_cast<unsigned>(digest[i]);
  }

  return oss.str();
}

} // namespace skim::siper

#endif // SKIMDB_SIPER_UTIL_H
