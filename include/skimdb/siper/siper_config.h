#ifndef SIPER_CONFIG_H
#define SIPER_CONFIG_H

#include <format>
#include <stdexcept>

#include <cereal/cereal.hpp>
#include <string>

#include "skimdb/detail/skimdb_definitions.h"


struct siper_version_t {
  // *** UPDATE VERSION IF FILE FORMAT CHANGES!!! ***
  inline static constexpr unsigned int major = 0;
  inline static constexpr unsigned int minor = 4;
  inline static constexpr unsigned int patch = 1;

  template <class Archive>
  void save(Archive& ar) const {
    unsigned int kmer64{skim::g_use_64bit};
    ar(cereal::make_nvp("major", major),
       cereal::make_nvp("minor", minor),
       cereal::make_nvp("patch", patch),
       cereal::make_nvp("kmer64", kmer64));
  }

  template <class Archive>
  void load(Archive& ar) const {
    unsigned int lmajor = 0;
    unsigned int lminor = 0;
    unsigned int lpatch = 0;
    unsigned int lkmer64 = 0;

    ar(cereal::make_nvp("major", lmajor),
       cereal::make_nvp("minor", lminor),
       cereal::make_nvp("patch", lpatch),
       cereal::make_nvp("kmer64", lkmer64));

    if (lmajor != major) {
      throw std::runtime_error{std::format("incompatible major version, file has {}, current is {}", lmajor, major)};
    }

    if (lminor != minor) {
      throw std::runtime_error{
          std::format("incompatible minor version, file has {}.{}, current is {}.{}", lmajor, lminor, major, minor)};
    }

    if (lkmer64 != skim::g_use_64bit) {
      throw std::runtime_error{std::format("incompatible kmer encoding, file has kmer64={}, current is kmer64={}",
                                           static_cast<bool>(lkmer64),
                                           skim::g_use_64bit)};
    }

    kmer = lkmer64;
  }

  inline static bool kmer{skim::g_use_64bit}; // set by loader
};

namespace std {
constexpr auto to_string(siper_version_t o) noexcept -> std::string {
  return to_string(o.major) + '.' + to_string(o.minor) + '.' + to_string(o.patch) + (o.kmer ? "-kmer64" : "");
}
} // namespace std

#endif // SIPER_CONFIG_H
