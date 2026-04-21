#ifndef SKIMDB_SPIR_UTIL_H
#define SKIMDB_SPIR_UTIL_H

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

auto sha256_file(const fs::path& path) -> std::expected<std::string, std::string> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected{"Unable to open file"};
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return std::unexpected{"EVP_MD_CTX_new failed"};
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::unexpected{"EVP_DigestInit_ex failed"};
  }

  std::vector<char> buffer(8192); // increase buffer size
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