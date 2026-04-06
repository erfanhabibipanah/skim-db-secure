#!/usr/bin/env bash

export SPDLOG_LEVEL=debug
#../release/bin/skimdb-index-create -i reference/sequences -o test.skimdb -k 15 -s 0 -t 0
../release/bin/skimdb-index-create -i reference/sequences -l reference/file2taxid -o test.skimdb -k 15 -s 0 -t 0
echo -e "AATGAATAATGGAAC\nAAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-index-query -i test.skimdb
