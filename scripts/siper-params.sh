#!/usr/bin/env bash
# pick SiPeR LWE parameters (n, sigma, log_p) for a database
#
# installs SageMath (from conda-forge) if it is not already available
# then runs siper-params.py to compute acceptable SiPeR parameters for the given
# database (or database dimension m) and target security level (default: 128 bits)
#
# usage:
#   ./siper-params.sh --m 178870
#   ./siper-params.sh --db ../tests/test.spir --json
#
# the conda env name can be overridden with SIPER_SAGE_ENV (default: siper-sage)

set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

HERE=$(cd "$(dirname "$0")" && pwd)
ENV_NAME="${SIPER_SAGE_ENV:-siper-sage}"

# find conda or mamba (prefer mamba, it is faster)
find_conda() {
  if command -v mamba >/dev/null 2>&1; then echo mamba; return; fi
  if command -v conda >/dev/null 2>&1; then echo conda; return; fi
  for base in "$HOME/miniforge3" "$HOME/miniconda3" "$HOME/anaconda3"; do
    [ -x "$base/bin/conda" ] && { echo "$base/bin/conda"; return; }
  done
  echo ""
}

CONDA=$(find_conda)

# no conda at all: install miniforge (headless)
if [ -z "$CONDA" ]; then
  echo "no conda found, installing Miniforge to $HOME/miniforge3 ..."
  OS=$(uname -s)
  ARCH=$(uname -m)
  URL="https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-${OS}-${ARCH}.sh"
  TMP=$(mktemp -d)
  curl -fsSL "$URL" -o "$TMP/miniforge.sh"
  bash "$TMP/miniforge.sh" -b -p "$HOME/miniforge3"
  rm -rf "$TMP"
  CONDA="$HOME/miniforge3/bin/conda"
fi

# create the sage env if it does not exist yet
if ! "$CONDA" run -n "$ENV_NAME" python -c "import sage.all" >/dev/null 2>&1; then
  echo "setting up SageMath in conda env '$ENV_NAME' (one time, ~2 GB from conda-forge) ..."
  "$CONDA" create -y -n "$ENV_NAME" -c conda-forge sage
fi

exec "$CONDA" run -n "$ENV_NAME" --no-capture-output \
  env PYTHONDONTWRITEBYTECODE=1 python "$HERE/siper-params.py" "$@"
