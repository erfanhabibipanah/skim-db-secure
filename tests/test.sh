#!/bin/bash
../release/bin/skimdb-index-create -i reference/sequences -o test.skimdb
echo "AAAATATATAATAAA AACGGTCCTAAGGTA" | ../release/bin/skimdb-index-query -i test.skimdb
