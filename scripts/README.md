# scripts/

## siper-params.sh
Picks SiPeR LWE parameters (n, sigma, log_p) for a database. This is the script
to run. It checks for SageMath and installs it once from conda-forge if it is
not there, then runs siper-params.py.

```
./siper-params.sh --m 178870
./siper-params.sh --db ../tests/test.spir --json
```

It uses a conda env called `siper-sage` (override with the SIPER_SAGE_ENV
variable). If there is no conda on the machine it installs Miniforge first. The
Sage download is about 2 GB and happens only the first time.

It picks the lowest log_p that keeps sigma under sigma_max (default 100000), then
returns the minimum n that reaches the target security (default 128 bit).

## siper-params.py
The actual selection logic. Needs SageMath with the lattice estimator on the
Python path. The vendored estimator in ../third_party/lattice-estimator is found
automatically; you can also point at another copy with LATTICE_ESTIMATOR_DIR.
Run it directly only if you already have a Sage python; otherwise use
siper-params.sh.

Options: --m, --db, --log_q, --log_delta, --sigma_max, --security_bits, --json.

## update-estimator.sh
Refreshes ../third_party/lattice-estimator from upstream (malb) and records the
source commit in UPSTREAM_COMMIT. Needs network access.
