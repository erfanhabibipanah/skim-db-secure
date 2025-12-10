/***
 *  $Id$
 **
 *  File: skimdb-index-create.cpp
 *  Author: Jaroslaw Zola <jaroslaw.zola@hush.com>
 *
 *  Copyright (c) 2025 SCoRe Group http://www.score-group.org/
 *  See accompanying LICENSE
 */

#include <iostream>
#include <string>

#include <cxxopts.hpp>


auto main(int argc, char* argv[]) -> int {
  std::string in;
  std::string out;

  int k = 15;
  int s = 9;
  int t = 0;

  try {
    cxxopts::Options options(argv[0]);

    options.add_options()
      ("i,input", "input file or directory FASTA format", cxxopts::value<std::string>(in))
      ("o,output", "database file name", cxxopts::value<std::string>(out))
      ("k", "k-mer size", cxxopts::value<int>(k)->default_value("15"))
      ("s", "syncmer s size", cxxopts::value<int>(s)->default_value("9"))
      ("t", "syncmer t parameter", cxxopts::value<int>(t)->default_value("0"))
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

  return 0;
}
