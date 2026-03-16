#!/usr/bin/env bash

if [ -d build/ ]; then
  cd build/
  make -j 8
  make install
else
  ./build.sh
fi
