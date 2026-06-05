#!/usr/bin/env bash

#export SPDLOG_LEVEL=debug

#../release/bin/skimdb-index-create -i reference/sequences -o test.skim -k 15 -s 0 -t 0
#../release/bin/skimdb-index-create -i reference/sequences -l reference/file2strain -o test.skim -k 15 -s 0 -t 0
../release/bin/skimdb-index-create -i reference/sequences -l reference/file2taxid -o test.skim -k 15 -s 0 -t 0
echo -e "AATGAATAATGGAAC\nGTTCCATTATTCATT\nAAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-index-query -i test.skim

../release/bin/skimdb-siper-create -i test.skim -o test.siper
echo -e "AATGAATAATGGAAC\nGTTCCATTATTCATT\nAAAATATATAATAAA\nAACGGTCCTAAGGTA" | ../release/bin/skimdb-siper-query -v -i test.siper
