#ifndef SKIMDB_DEFINITIONS_H
#define SKIMDB_DEFINITIONS_H

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <roaring/roaring.hh>
#include <roaring/roaring64map.hh>


namespace skim {

// we need 64bit std::size_t (e.g., 64bit is used by gRPC)
static_assert(sizeof(std::size_t) == 8, "std::size_t must be 64-bit");

// parameters describing skimdb database
struct skimdb_parameters {
  std::size_t k;
  std::size_t s;
  std::size_t t;
};

// for now equivalence is defined by all parameters being the same
// perhaps will fix that in the future :-)
inline auto operator==(const skimdb_parameters& lhs, const skimdb_parameters& rhs) {
  return ((lhs.k == rhs.k) && (lhs.s == rhs.s) && (lhs.t == rhs.t));
}

inline auto operator!=(const skimdb_parameters& lhs, const skimdb_parameters& rhs) {
  return !(lhs == rhs);
}


#ifdef SKIMDB_64BIT
inline constexpr bool g_use_64bit = true;
#else
inline constexpr bool g_use_64bit = false;
#endif

// type to represent encoded k-mer
using kmer_binary_t = std::conditional_t<g_use_64bit, std::uint64_t, std::uint32_t>;

// default bitmap type for k-mer handling (k-mer binary type must be storable in bitmap_t)
using bitmap_t = std::conditional_t<g_use_64bit, roaring::Roaring64Map, roaring::Roaring>;

// max k-mer size handled by skimdb
inline constexpr std::size_t g_kmer_limit = sizeof(kmer_binary_t) * CHAR_BIT;

  // available ordering strategies that can be used to build skimdb index
enum class skimdb_rle_ordering : std::uint8_t { none, tsp, minmax };

inline constexpr auto parse_rle_ordering(std::string_view s) -> skimdb_rle_ordering {
  if (s == "none") {
    return skimdb_rle_ordering::none;
  }
  if (s == "tsp") {
    return skimdb_rle_ordering::tsp;
  }
  if (s == "minmax") {
    return skimdb_rle_ordering::minmax;
  }
  throw std::runtime_error("Invalid RLE ordering strategy: " + std::string(s));
}

} // namespace skim

namespace std {
constexpr auto to_string(skim::skimdb_rle_ordering o) noexcept -> std::string {
  switch (o) {
  case skim::skimdb_rle_ordering::none:
    return "none";
  case skim::skimdb_rle_ordering::tsp:
    return "tsp";
  case skim::skimdb_rle_ordering::minmax:
    return "minmax";
  }
  return "unknown";
}
} // namespace std

#endif // SKIMDB_DEFINITIONS_H
