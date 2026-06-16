#!/usr/bin/env bash
# refresh the lattice-estimator from upstream (malb)
# needs network access

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HERE/../third_party/lattice-estimator"
TMP=$(mktemp -d)

# git >= 2.25 sparse partial clone; falls back to a plain shallow clone otherwise
if ! git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/malb/lattice-estimator.git "$TMP" 2>/dev/null; then
  git clone --depth 1 https://github.com/malb/lattice-estimator.git "$TMP"
else
  git -C "$TMP" sparse-checkout set estimator
fi

mkdir -p "$DEST/estimator"
rsync -a --delete --exclude '__pycache__' --exclude '*.pyc' \
  "$TMP/estimator/" "$DEST/estimator/"
git -C "$TMP" rev-parse HEAD > "$DEST/UPSTREAM_COMMIT"

rm -rf "$TMP"
echo "estimator updated to $(cat "$DEST/UPSTREAM_COMMIT")"
