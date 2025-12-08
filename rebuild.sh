#!/bin/bash

if [ -d build/ ]; then
  cd build/
  make
  make install
else
  ./build.sh
fi
