#ifndef SKIMDB_SPIR_UTIL_H
#define SKIMDB_SPIR_UTIL_H

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


namespace skim::spir {

namespace fs = std::filesystem;

namespace index {

inline constexpr uint64_t g_len_bits = 12;
inline constexpr uint64_t g_len_mask = (1ULL << g_len_bits) - 1;

inline uint64_t pack(uint64_t start, uint16_t len) {
  return (start << g_len_bits) | len;
}

inline uint64_t unpack_start(uint64_t v) {
  return v >> g_len_bits;
}

inline uint16_t unpack_len(uint64_t v) {
  return static_cast<uint16_t>(v & g_len_mask);
}

} // namespace index


auto min_sqrt_N(std::size_t min_blocks, std::size_t block_size) -> std::size_t {
  std::size_t min_cells = min_blocks * block_size;
  std::size_t min_side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(min_cells))));
  return (min_side + block_size - 1) / block_size * block_size;
}


auto populate_skimdb_matrix(const std::vector<skim::detail::encoding>& data,
                            const std::vector<std::uint64_t>& metadata,
                            std::size_t sqrt_N, 
                            std::size_t block_size)
    -> skimdb_matrix {
  std::vector<std::uint16_t> packed_data(sqrt_N * sqrt_N, 0);

#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < data.size(); ++i) {
    auto start_idx = index::unpack_start(metadata[i]);
    auto src = data[i].span();
    auto rle_len = data[i].length();

    for (std::size_t j = 0; j < rle_len; ++j) {
      std::size_t col_idx = (start_idx + j) / sqrt_N;
      std::size_t row_idx = (start_idx + j) % sqrt_N;
      packed_data[row_idx * sqrt_N + col_idx] = src[j];
    }
  }

  return skimdb_matrix{std::move(packed_data), block_size, sqrt_N};
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

} // namespace skim::spir

#endif // SKIMDB_SPIR_UTIL_H
