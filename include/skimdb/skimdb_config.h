#ifndef SKIMDB_CONFIG_H
#define SKIMDB_CONFIG_H

#include <format>
#include <stdexcept>

#include <cereal/cereal.hpp>


struct skimdb_version_t {
  // *** UPDATE VERSION IF FILE FORMAT CHANGES!!! ***
  inline static constexpr unsigned int major = 0;
  inline static constexpr unsigned int minor = 1;
  inline static constexpr unsigned int patch = 0;

  template <class Archive>
  void save(Archive& ar) const {
    ar(cereal::make_nvp("major", major), cereal::make_nvp("minor", minor), cereal::make_nvp("patch", patch));
  }

  template <class Archive>
  void load(Archive& ar) const {
    unsigned int lmajor = 0;
    unsigned int lminor = 0;
    unsigned int lpatch = 0;

    ar(cereal::make_nvp("major", lmajor), cereal::make_nvp("minor", lminor), cereal::make_nvp("patch", lpatch));

    if (lmajor != major) {
      throw std::runtime_error{std::format("incompatible major version: file has {}, current is {}", lmajor, major)};
    }

    if (lminor > minor) {
      throw std::runtime_error{
          std::format("incompatible minor version: file has {}.{}, current is {}.{}", lmajor, lminor, major, minor)};
    }
  }
};

#endif // SKIMDB_CONFIG_H
