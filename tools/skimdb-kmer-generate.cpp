/***
 *  $Id$
 **
 *  File: skimdb-kmer-generate.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2026 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#include <generator>
#include <iostream>
#include <random>

#include <cxxopts.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>


class kmer_distribution {
public:
  using result_type = std::string;

  struct param_type {
    std::size_t k;
    friend bool operator==(const param_type&, const param_type&) = default;
  };

  kmer_distribution() = default;
  explicit kmer_distribution(std::size_t k) : params_{k} {}
  explicit kmer_distribution(const param_type& p) : params_{p} {}

  [[nodiscard]] param_type param() const noexcept { return params_; }
  void param(const param_type& p) noexcept { params_ = p; }

  [[nodiscard]] std::size_t k() const noexcept { return params_.k; }

  void reset() noexcept {}

  template <typename URBG>
  auto operator()(URBG& g) -> result_type {
    return (*this)(g, params_);
  }

  template <typename URBG>
  result_type operator()(URBG& g, const param_type& p) {
    std::string s;
    s.resize(p.k);

    std::uniform_int_distribution<int> dist(0, 3);

    for (std::size_t i = 0; i < p.k; ++i) {
      s[i] = alphabet_[dist(g)];
    }

    return s;
  }

  friend bool operator==(const kmer_distribution&, const kmer_distribution&) = default;

private:
  param_type params_{0};
  static constexpr char alphabet_[4] = {'A', 'C', 'G', 'T'};
};


auto random_kmers(std::size_t k, std::int64_t seed) -> std::generator<std::string> {
  if (seed < 0) {
    seed = std::random_device{}();
  }

  std::mt19937 rng(seed);
  kmer_distribution dist(k);

  while (true) {
    co_yield dist(rng);
  }
}

auto main(int argc, char* argv[]) -> int {
  int k = 15;
  int l = 100000;
  std::int64_t seed = 666;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("k", "k-mer size", cxxopts::value<int>(k)->default_value(std::to_string(k)))
      ("l", "sample size", cxxopts::value<int>(l)->default_value(std::to_string(l)))
      ("s,seed", "random seed", cxxopts::value<std::int64_t>(seed)->default_value(std::to_string(seed)))
      ("h,help", "print this help");

    auto opt_res = options.parse(argc, argv);

    if ((opt_res.unmatched().size() != 0) || opt_res.count("help")) {
      std::cout << options.help() << std::endl;
      return 0;
    }
  } catch (const cxxopts::exceptions::exception& e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  spdlog::cfg::load_env_levels();
  auto log = spdlog::stdout_color_mt("skimdb-kmer-generate");

  if (k < 2) {
    log->error("incorrect k, must be  k > 1");
    return -1;
  }

  if (l < 1) {
    log->error("incorrect l, must be s > 0");
    return -1;
  }

  for (const auto& kmer : random_kmers(k, seed) | std::views::take(l)) {
    std::cout << kmer << '\n';
  }

  return 0;
}
