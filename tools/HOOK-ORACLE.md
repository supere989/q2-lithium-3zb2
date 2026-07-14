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

An isolated q2ded parity build uses a compile-time-only probe command:

```sh
make clean
make CFLAGS='-O1 -g -DNEED_STRLCAT -DNEED_STRLCPY -DQ2_HOOK_PARITY_PROBE' \
  lithium/gamex86_64.so hook-oracle
make Q2DED=/path/to/q2ded PMOVE_ORACLE=/path/to/q2-pmove-oracle \
  CM_ORACLE=/path/to/q2-cm-oracle test-hook-q2ded-parity
```

Normal game builds contain neither the probe command nor its output path.

Every input and output is one JSON object per line. The schema is
`q2-hook-oracle-v1`; its machine-readable request contract is
`tools/schemas/q2-hook-oracle-v1.schema.json`. Operations are:

- `identity`: source/build hashes and the hook parameter block.
- `launch`: requires `forward`; uses `hook_speed` (default `900`).
- `pull`: requires `owner_origin` and `hook_origin`; when
  `enemy_is_client=true`, also requires `enemy_origin`. The result explicitly
  records `full_velocity_overwrite=true`.
- `touch`: classifies owner, invalid-owner, nonblocking/flymissile, sky, and
  attach outcomes in the exact `Hook_Touch` order.
- `backoff`: reproduces the 10-unit obstructed muzzle-start retraction.

The identity binds `ml_hook_physics.c/.h`, its `l_hook.c` integration,
`q_shared.c` math, build contract, `hook_speed`, `hook_pullspeed`, `hook_sky`,
`hook_maxtime`, and the full-overwrite law. Collision/landing identity is the
ordered pair of this identity and the companion Pmove physics identity.
