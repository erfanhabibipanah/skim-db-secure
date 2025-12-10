#include <filesystem>
#include <iostream>
#include <print>

#include <skimdb/skimdb.h>
#include <skimdb/skimdb_builder.h>

namespace fs = std::filesystem;

#ifndef REFERENCE_DIR
#define REFERENCE_DIR "reference"
#endif

int main() {
  const fs::path ref_dir = REFERENCE_DIR;
  const fs::path db_path = ref_dir/"demo_db.skimdb";
  const fs::path file2labels = ref_dir/"file2labels";
  const fs::path sequences = ref_dir/"sequences";

  if (!fs::exists(db_path)) {
    std::print("Building SkimDB...\n");
    if (!skim::skim_db_builder::build_file_index(sequences, file2labels, 15, 9, 0).save(db_path.string())) {
      std::print("Failed to build SkimDB\n");
      return -1;
    }
  }

  std::print("Loading SkimDB...\n");
  skim::skim_db db;
  if (!db.load(db_path.string())) {
    std::print("Failed to load SkimDB\n");
    return -1;
  }

  // Appears in files 5-8
  std::print("Querying kmer: AAAATATATAATAAA\n");
  for (auto label : db.query("AAAATATATAATAAA")) {
    std::print("\t{}\n", label);
  }

  // Appears in all 8 files
  std::print("Querying kmer: AACGGTCCTAAGGTA\n");
  for (auto label : db.query("AACGGTCCTAAGGTA")) {
    std::print("\t{}\n", label);
  }

  return 0;
}
