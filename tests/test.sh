#!/usr/bin/env bash

#export SPDLOG_LEVEL=debug
#../release/bin/skimdb-index-create -i reference/sequences -o test.skimdb -k 15 -s 0 -t 0
#../release/bin/skimdb-index-create -i reference/sequences -l reference/file2strain -o test.skimdb -k 15 -s 0 -t 0
../release/bin/skimdb-index-create -i reference/sequences -l reference/file2taxid -o test.skimdb -k 15 -s 0 -t 0
echo -e "AATGAATAATGGAAC\nGTTCCATTATTCATT\nAAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-index-query -i test.skimdb

../release/bin/skimdb-spir-create -i test.skimdb -o test.spir
echo -e "AATGAATAATGGAAC\nGTTCCATTATTCATT\nAAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-spir-query -i test.spir
