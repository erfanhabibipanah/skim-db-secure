#!/bin/bash

rm -rf .cpm-include/

if [ -d .cpm-cache/ ]; then
  mkdir .cpm-include/
  for i in .cpm-cache/*; do
    DIR=$(basename "$i")
    HASH=(.cpm-cache/$DIR/*.hash)
    if compgen -G ".cpm-cache/$DIR/*.hash" > /dev/null; then
      HASH=$(basename "${HASH[0]}" .hash)
      ln -s ../.cpm-cache/$DIR/$HASH .cpm-include/$DIR
      echo `pwd`/.cpm-include/$DIR
    fi
  done
else
  echo "no .cpm-cache..."
fi
