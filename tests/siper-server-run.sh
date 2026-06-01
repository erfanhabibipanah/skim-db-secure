#!/bin/bash

CORES="0,4,8,12,16,20,24,28,32,36,40,44,48"
CORES="$CORES,2,6,10,14,18,22,26,30,34,38,42,46,50"

OMP_NUM_THREADS=24 OMP_PLACES="$CORES" OMP_PROC_BIND=close ../release/bin/skimdb-siper-serve-rpc $@
