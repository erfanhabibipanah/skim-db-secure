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

# check for the dbsize binary before the conda setup below, which downloads
# ~2 GB and takes minutes; a missing binary should be reported now, not then.
# only --db needs it, --m takes the dimension directly
needs_dbsize=0
for arg in "$@"; do
  case "$arg" in
    --db|--db=*) needs_dbsize=1 ;;
  esac
done

if [ "$needs_dbsize" -eq 1 ]; then
  if [ -n "${SKIMDB_DBSIZE_BIN:-}" ]; then
    if [ ! -x "$SKIMDB_DBSIZE_BIN" ]; then
      echo "error: SKIMDB_DBSIZE_BIN is set to '$SKIMDB_DBSIZE_BIN' but that is not executable." >&2
      exit 1
    fi
  elif ! command -v skimdb-siper-dbsize >/dev/null 2>&1 \
     && [ ! -x "$HERE/../build/tools/skimdb-siper-dbsize" ] \
     && [ ! -x "$HERE/../release/bin/skimdb-siper-dbsize" ]; then
    echo "error: skimdb-siper-dbsize not found." >&2
    echo "  looked on PATH, in $HERE/../build/tools, and in $HERE/../release/bin" >&2
    echo "  build the SKiMdb tools, put their directory on PATH, or set" >&2
    echo "    SKIMDB_DBSIZE_BIN=/path/to/skimdb-siper-dbsize" >&2
    echo "  (--m <sqrt(N)> skips this tool entirely)" >&2
    exit 1
  fi
fi

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

# test whether the env exists, not whether "import sage.all" works. that import
# is slow and fails for reasons unrelated to a missing env (broken R config,
# partial install), which sent the script into a fresh 2 GB conda create on
# every run. SIPER_SKIP_ENV_SETUP=1 skips this and uses the env as-is
env_exists() {
  "$CONDA" env list 2>/dev/null | awk -v n="$ENV_NAME" '
    /^#/ { next }
    { name = $1; path = $NF; sub(/.*\//, "", path); if (name == n || path == n) found = 1 }
    END { exit !found }
  '
}

if [ "${SIPER_SKIP_ENV_SETUP:-0}" != "1" ]; then
  if ! env_exists; then
    echo "conda env '$ENV_NAME' not found."
    echo "installing SageMath into it now (about 2 GB from conda-forge, this takes a while)."
    echo "press Ctrl-C within 5 seconds to cancel."
    sleep 5
    if ! "$CONDA" create -y -n "$ENV_NAME" -c conda-forge sage; then
      echo "error: could not create the conda env '$ENV_NAME'." >&2
      echo "  create it yourself with:" >&2
      echo "    $CONDA create -n $ENV_NAME -c conda-forge sage" >&2
      echo "  or point SIPER_SAGE_ENV at an env that already has Sage." >&2
      exit 1
    fi
  fi
fi

exec "$CONDA" run -n "$ENV_NAME" --no-capture-output \
  env PYTHONDONTWRITEBYTECODE=1 python "$HERE/siper-params.py" "$@"
