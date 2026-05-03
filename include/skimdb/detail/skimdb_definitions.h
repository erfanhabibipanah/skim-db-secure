#ifndef SKIMDB_DEFINITIONS_H
#define SKIMDB_DEFINITIONS_H

#include <climits>
#include <cstddef>
#include <cstdint>

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

} // namespace skim

#endif // SKIMDB_DEFINITIONS_H
