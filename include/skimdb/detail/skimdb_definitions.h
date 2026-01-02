#ifndef SKIMDB_DEFINITIONS_H
#define SKIMDB_DEFINITIONS_H

#include <roaring.hh>


namespace skim {

// default bitmap type for intermediate data handling
using bitmap_t = roaring::Roaring;

// max k-mer size handled by skimdb
inline constexpr std::size_t g_kmer_limit = 16;

} // namespace skimdb

#endif // SKIMDB_DEFINITIONS_H
