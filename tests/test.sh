#!/usr/bin/env bash

export SPDLOG_LEVEL=debug
../release/bin/skimdb-index-create -i reference/sequences -o test.skimdb -k 15 -s 9 -t 0
echo -e "AAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-index-query -i test.skimdb
