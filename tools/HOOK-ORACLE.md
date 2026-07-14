# q2-hook-oracle

`q2-hook-oracle` is an offline NDJSON batch interface to the same pure hook
law compiled into Lithium's `l_hook.c`. It does not estimate collision or
movement. Consumers compose its velocity overwrite with the pinned
`q2-pmove-oracle`; if either oracle or the isolated-q2ded parity proof is
missing, hook edges must be omitted.

Build and test:

```sh
make hook-oracle
make test-hook-oracle
```

An isolated q2ded parity build uses a compile-time-only probe command. The
documented runtime uses `hook_pullspeed=1700`, and every runtime parameter is
passed explicitly to q2ded and to the oracle:

```sh
make clean
make CFLAGS='-O1 -g -DNEED_STRLCAT -DNEED_STRLCPY -DQ2_HOOK_PARITY_PROBE' \
  lithium/gamex86_64.so hook-oracle
make Q2DED=/path/to/q2ded PMOVE_ORACLE=/path/to/q2-pmove-oracle \
  CM_ORACLE=/path/to/q2-cm-oracle HOOK_PARITY_SPEED=900 \
  HOOK_PARITY_PULLSPEED=1700 HOOK_PARITY_SKY=0 HOOK_PARITY_MAXTIME=5 \
  test-hook-q2ded-parity
```

On success, this writes exactly one canonical JSON object to
`.build/hook-oracle/hook-parity-attestation.json`. It contains
`passed=true`, the exact parameter block and fixture BSP identity, hook/CM/
Pmove physics identities, SHA-256 identities for q2ded and all oracle/probe
binaries, an attestor source-closure identity, and a digest of the vectors and
results for all parity cases. A mismatch exits nonzero and does not produce an
admissible `passed=true` attestation. The whole artifact can be identified
with `sha256sum` and reproduced byte-for-byte from the same closure.

The consumer must validate both
`tools/schemas/q2-hook-parity-attestation-v1.schema.json` and the semantic
checks in `tools/hook_parity_contract.py`; schema validation alone cannot
recompute the parameter-bound hook identity or vector/results digest. A
changed `hook_pullspeed` or a changed result vector invalidates the proof.

Normal game builds contain neither the probe command nor its output path.
After returning to normal compiler flags, verify the boundary with:

```sh
make clean
make lithium/gamex86_64.so
make test-hook-probe-boundary
```

Every input and output is one JSON object per line. The schema is
`q2-hook-oracle-v1`; its strict machine-readable contracts are
`tools/schemas/q2-hook-oracle-v1.schema.json` for requests and
`tools/schemas/q2-hook-oracle-v1.response.schema.json` for responses.
Successful records carry both the parameter-bound `physics_identity` and the
source/tool-closure-bound `tool_identity`. Operations are:

- `identity`: source/build hashes and the hook parameter block.
- `launch`: requires `forward`; uses `hook_speed` (default `900`).
- `pull`: requires `owner_origin` and `hook_origin`; when
  `enemy_is_client=true`, also requires `enemy_origin`. The result explicitly
  records `full_velocity_overwrite=true`.
- `touch`: classifies owner, invalid-owner, nonblocking/flymissile, sky, and
  attach outcomes in the exact `Hook_Touch` order.
- `backoff`: reproduces the 10-unit obstructed muzzle-start retraction.

The physics identity binds `ml_hook_physics.c/.h`, its `l_hook.c`
integration, `q_shared.c` math, build contract, tool-closure identity,
`hook_speed`, `hook_pullspeed`, `hook_sky`, `hook_maxtime`, and the
full-overwrite law. The tool closure is a deterministic digest of the exact
hook implementation, generator, SHA-256 implementation, Makefile, request
schema, and response schema. Collision/landing attestation additionally binds
the CM and Pmove identities to the exact same fixture BSP.
