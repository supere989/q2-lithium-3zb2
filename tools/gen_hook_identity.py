#!/usr/bin/env python3
"""Generate deterministic semantic and tool-closure hook identities."""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys


BUILD_CONTRACT = "lithium-linux-c99-o1-f32-shared-hook-v2"
TOOL_CLOSURE_FILES = (
    "Makefile",
    "l_hook.c",
    "ml_hook_physics.c",
    "ml_hook_physics.h",
    "q_shared.c",
    "q_shared.h",
    "tools/gen_hook_identity.py",
    "tools/oracle_sha256.c",
    "tools/oracle_sha256.h",
    "tools/q2_hook_oracle.c",
    "tools/schemas/q2-hook-oracle-v1.schema.json",
    "tools/schemas/q2-hook-oracle-v1.response.schema.json",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def closure_digest(root: Path) -> str:
    digestor = hashlib.sha256()
    digestor.update(b"q2-hook-tool-closure-v1\n")
    digestor.update(f"build={BUILD_CONTRACT}\n".encode())
    for name in sorted(TOOL_CLOSURE_FILES):
        digestor.update(f"{name}\0{digest(root / name)}\n".encode())
    return digestor.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_hook_identity.py REPOSITORY_ROOT OUTPUT_HEADER")
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2])
    body = (
        "#ifndef Q2_HOOK_ORACLE_IDENTITY_H\n"
        "#define Q2_HOOK_ORACLE_IDENTITY_H\n"
        f'#define Q2_HOOK_SHARED_C_SHA256 "{digest(root / "ml_hook_physics.c")}"\n'
        f'#define Q2_HOOK_SHARED_H_SHA256 "{digest(root / "ml_hook_physics.h")}"\n'
        f'#define Q2_HOOK_INTEGRATION_SHA256 "{digest(root / "l_hook.c")}"\n'
        f'#define Q2_HOOK_MATH_SHA256 "{digest(root / "q_shared.c")}"\n'
        f'#define Q2_HOOK_BUILD_CONTRACT "{BUILD_CONTRACT}"\n'
        f'#define Q2_HOOK_TOOL_CLOSURE_SHA256 "{closure_digest(root)}"\n'
        "#endif\n"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != body:
        output.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
