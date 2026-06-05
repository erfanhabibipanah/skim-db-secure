#include <skimdb/skimdb.h>

auto main(int argc, char* argv[]) -> int {
  skim::skimdb db;
  auto res = db.load(argv[1]);
  db.save(argv[2]);
  return 0;
}
