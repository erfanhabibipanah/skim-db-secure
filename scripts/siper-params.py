#!/usr/bin/env python3
"""
Select SiPeR parameters (n, sigma) which achieve the target security for a given database.

Given sqrt(N) (either from --m or read from a SKiMdb file via skimdb-siper-dbsize),
finds the smallest log_p such that sigma <= sigma_max, then returns the minimum n
that achieves the target security at that log_p.

Requires SageMath with the lattice-estimator package available for import. See:
    https://github.com/malb/lattice-estimator
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys


def _add_estimator_to_path():
  env_path = os.environ.get("LATTICE_ESTIMATOR_DIR")
  if env_path:
    sys.path.insert(0, env_path)
    return

  here = os.path.dirname(os.path.abspath(__file__))
  candidates = [
    os.path.join(here, "..", "third_party", "lattice-estimator"),
    os.path.join(here, "..", "..", "lattice-estimator"),
    os.path.expanduser("~/lattice-estimator"),
  ]

  for path in candidates:
    if os.path.isdir(os.path.join(path, "estimator")):
      sys.path.insert(0, path)
      return


_add_estimator_to_path()


TARGET_SECURITY = 128
ESTIMATOR_JOBS = 4

LOG_P_MIN = 16
LOG_P_MAX = 32

N_MIN = 1024
N_MAX = 4096


def compute_sigma_for_p(q, target_p, N, delta):
  ln_term = math.log(2 / delta)
  return q / (math.sqrt(2) * (target_p ** 2) * (N ** 0.25) * math.sqrt(ln_term))


def security_bits(n, log_q, sigma, m):
  from estimator import LWE, ND
  from sage.all import oo

  q = 2 ** log_q
  params = LWE.Parameters(
    n=n, 
    q=q,
    Xs=ND.UniformMod(q),
    Xe=ND.DiscreteGaussian(sigma),
    m=m,
  )
  
  result = LWE.estimate(params, jobs=ESTIMATOR_JOBS)

  min_bits = float("inf")
  for _, data in result.items():
    rop = data.get("rop", oo)
    if rop != oo:
      bits = math.log2(float(rop))
      if bits < min_bits:
        min_bits = bits
  
  return min_bits


def find_min_n(log_q, sigma, m, target_security):
  lo, hi = N_MIN, N_MAX
  best = None

  while lo <= hi:
    mid = (lo + hi) // 2

    if security_bits(mid, log_q, sigma, m) >= target_security:
      best = mid
      hi = mid - 1
    else:
      lo = mid + 1

  return best


def select_params(m, log_q, log_delta, sigma_max, target_security):
  N = m * m
  delta = 2 ** (-log_delta)

  for log_p in range(LOG_P_MIN, LOG_P_MAX + 1):
    sigma = compute_sigma_for_p(2 ** log_q, 2 ** log_p, N, delta)
    if sigma > sigma_max:
      continue

    n = find_min_n(log_q, sigma, m, target_security)
    if n is None:
      continue

    hint_bytes = m * n * (log_q // 8)
    return {
      "n": n,
      "sigma": round(sigma, 4),
      "log_p": log_p,
      "log_q": log_q,
      "log_delta": log_delta,
      "m": m,
      "N": N,
      "hint_bytes": hint_bytes,
      "security_bits": target_security,
    }
  
  return None


def read_sqrt_n_from_db(db_path):
  here = os.path.dirname(os.path.abspath(__file__))
  candidates = [
    "skimdb-siper-dbsize",
    os.path.join(here, "..", "build", "tools", "skimdb-siper-dbsize"),
    os.path.join(here, "..", "release", "bin", "skimdb-siper-dbsize"),
  ]

  print("locating skimdb-siper-dbsize binary...")

  for tool in candidates:
    print(f"checking {tool}:")
    try:
      result = subprocess.run([tool, "-i", db_path], stderr=subprocess.STDOUT, stdout=subprocess.PIPE, text=True, check=True)
      out = result.stdout
      break
    except FileNotFoundError:
      print(f"  {tool} not found")
      continue
    except subprocess.CalledProcessError as e:
      print(f"  {tool}: found, but exited with code {e.returncode}")
      if e.stdout and e.stdout.rstrip():
        print(f"    {e.stdout.rstrip().splitlines()[-1]}")
      continue
  else:
    raise RuntimeError("error computing database size. (see above for subprocess output)")

  match = re.search(r"sqrt\(N\)\s*=\s*(\d+)", out)
  if not match:
    raise RuntimeError(f"could not parse sqrt(N) from skimdb-siper-dbsize output:\n{out}")

  return int(match.group(1))


def main():
  parser = argparse.ArgumentParser(description="select secure SiPeR parameters")
  src = parser.add_mutually_exclusive_group(required=True)
  src.add_argument("--m", type=int, help="sqrt(N) of the database (matrix dimension)")
  src.add_argument("--db", type=str, help="path to SKiMdb file (calls skimdb-siper-dbsize to read sqrt(N))")
  parser.add_argument("--log_q", type=int, default=64)
  parser.add_argument("--log_delta", type=int, default=40)
  parser.add_argument("--sigma_max", type=float, default=100000.0)
  parser.add_argument("--security_bits", type=int, default=TARGET_SECURITY)
  parser.add_argument("--json", action="store_true", help="output as JSON")
  args = parser.parse_args()

  if args.db:
    m = read_sqrt_n_from_db(args.db)
  else:
    m = args.m

  result = select_params(m, args.log_q, args.log_delta, args.sigma_max, args.security_bits)

  if result is None:
    print(f"no log_p in [{LOG_P_MIN}, {LOG_P_MAX}] satisfies sigma <= {args.sigma_max} for m={m}", file=sys.stderr)
    sys.exit(1)

  if args.json:
    print(json.dumps(result, indent=2))
  else:
    for key in ("n", "sigma", "log_p", "log_q", "log_delta", "m", "N", "hint_bytes", "security_bits"):
      print(f"{key}={result[key]}")


if __name__ == "__main__":
  main()
