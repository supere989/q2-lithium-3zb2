# q2-fall-oracle

`q2-fall-oracle` is an offline NDJSON interface to the exact side-effect-free
severity and raw-damage law used by Lithium's `P_FallingDamage`. The runtime
and oracle both compile `ml_fall_physics.c`; `p_view.c` is now an adapter that
applies the returned event, view-kick state, pain debounce, and `T_Damage`
request. No approximate ballistic or height-to-damage formula is involved.

Build and test:

```sh
make fall-oracle
make test-fall-oracle
```

Example:

```sh
tools/q2-fall-oracle <<'EOF'
{"id":"identity","op":"identity","fall_damagemod":1.5,"deathmatch":true,"dmflags":0}
{"id":"drop","op":"evaluate","old_velocity_z":-1000,"velocity_z":0,"grapple_release_elapsed":1,"fall_damagemod":1.5,"modelindex":255,"movetype":4,"grounded":true,"hook_out":false,"grapple_present":false,"grapple_state":0,"waterlevel":0,"deathmatch":true,"dmflags":0,"health":52}
EOF
```

An `evaluate` request supplies every runtime precondition used by the law:
vertical velocities, grounded state, player model index, movement type, Orange
hook state, CTF grapple presence/state/release interval, water level, current
health, deathmatch/`dmflags`, and `fall_damagemod`. Missing, duplicate,
operation-inapplicable, unknown, malformed, nonfinite, or out-of-range fields
fail closed. The machine-readable request and sealed response contracts are in
`tools/schemas/q2-fall-oracle-v1*.json`.

The result separates severity from suppression and side effects. In
particular, `DF_NO_FALLING` prevents `T_Damage` but preserves fall events and
view kick, water levels 1 and 2 scale severity before thresholds, water level
3 suppresses the fall, and active/recent grapples suppress it. Damage retains
Lithium's two integer truncation points, including 35 becoming 52 at a 1.5
modifier.

`unmitigated_health_after` and `unmitigated_lethal` describe the raw fall
damage request. They deliberately do not claim the final result of `T_Damage`,
which may apply armor, power armor, godmode, invincibility, safety time, skill,
or other game-state protection. Atlas controlled-drop analysis should use the
raw amount as severity and add live protection state separately when needed.

Every successful record carries a tool identity over the shared helper,
runtime adapter, relevant game headers, schemas, generator, SHA implementation,
and build contract. Its physics identity additionally binds the complete
constants contract plus effective `fall_damagemod`, deathmatch mode, and
`dmflags`. Consumers should admit records through
`tools/fall_oracle_contract.py`; pin an `identity` response and pass evaluation
records through `admit_fall_response`. Omit fall-damage claims when identity or
response validation fails.
