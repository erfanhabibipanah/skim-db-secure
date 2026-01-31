#ifndef SKIMDB_SPIR_RANDOM_H
#define SKIMDB_SPIR_RANDOM_H

#include <cstdint>
#include <random>


namespace skim {
namespace spir {

// TODO: improve later with better randomness source
inline auto new_seed() -> std::uint64_t {
    std::random_device rd;

    uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    return seed;
}

} // namespace spir
} // namespace skim

#endif // SKIMDB_SPIR_RANDOM_H