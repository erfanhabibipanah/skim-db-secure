#ifndef SKIMDB_DEFINITIONS_H
#define SKIMDB_DEFINITIONS_H

#include <cstdint>
#include <roaring/roaring.hh>


namespace skim {

// parameters describing skimdb database
struct skimdb_parameters {
  std::uint64_t k;
  std::uint64_t s;
  std::uint64_t t;
};

// for now equivalence is defined by all parameters being the same
// perhaps will fix that in the future :-)
inline auto operator==(const skimdb_parameters& lhs, const skimdb_parameters& rhs) {
  return ((lhs.k == rhs.k) && (lhs.s == rhs.s) && (lhs.t == rhs.t));
}

inline auto operator!=(const skimdb_parameters& lhs, const skimdb_parameters& rhs) {
  return !(lhs == rhs);
}


// default bitmap type for intermediate data handling
using bitmap_t = roaring::Roaring;


// max k-mer size handled by skimdb
inline constexpr std::uint64_t g_kmer_limit = 16;

} // namespace skim

#endif // SKIMDB_DEFINITIONS_H
