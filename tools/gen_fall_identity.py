#!/usr/bin/env python3
"""Generate deterministic semantic and tool-closure fall identities."""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import sys


BUILD_CONTRACT = "lithium-linux-c99-o1-f32-shared-fall-v1"
TOOL_CLOSURE_FILES = (
    "Makefile",
    "g_ctf.h",
    "g_local.h",
    "lithium.h",
    "ml_fall_physics.c",
    "ml_fall_physics.h",
    "p_view.c",
    "q_shared.h",
    "tools/gen_fall_identity.py",
    "tools/fall_oracle_contract.py",
    "tools/oracle_sha256.c",
    "tools/oracle_sha256.h",
    "tools/q2_fall_oracle.c",
    "tools/schemas/q2-fall-oracle-v1.schema.json",
    "tools/schemas/q2-fall-oracle-v1.response.schema.json",
)
CONSTANTS_PATTERN = re.compile(
    r'^#define Q2_FALL_CONSTANTS_CONTRACT "([^"]+)"$', re.MULTILINE
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def constants_contract(root: Path) -> str:
    matched = CONSTANTS_PATTERN.search(
        (root / "ml_fall_physics.h").read_text(encoding="utf-8")
    )
    if matched is None:
        raise SystemExit("Q2_FALL_CONSTANTS_CONTRACT must be one literal string")
    return matched.group(1)


def text_digest(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def closure_digest(root: Path, constants: str) -> str:
    digestor = hashlib.sha256()
    digestor.update(b"q2-fall-tool-closure-v1\n")
    digestor.update(f"build={BUILD_CONTRACT}\nconstants={constants}\n".encode())
    for name in sorted(TOOL_CLOSURE_FILES):
        digestor.update(f"{name}\0{digest(root / name)}\n".encode())
    return digestor.hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_fall_identity.py REPOSITORY_ROOT OUTPUT_HEADER")
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2])
    constants = constants_contract(root)
    body = (
        "#ifndef Q2_FALL_ORACLE_IDENTITY_H\n"
        "#define Q2_FALL_ORACLE_IDENTITY_H\n"
        f'#define Q2_FALL_SHARED_C_SHA256 "{digest(root / "ml_fall_physics.c")}"\n'
        f'#define Q2_FALL_SHARED_H_SHA256 "{digest(root / "ml_fall_physics.h")}"\n'
        f'#define Q2_FALL_INTEGRATION_SHA256 "{digest(root / "p_view.c")}"\n'
        f'#define Q2_FALL_GAME_HEADER_SHA256 "{digest(root / "g_local.h")}"\n'
        f'#define Q2_FALL_CONSTANTS_SHA256 "{text_digest(constants)}"\n'
        f'#define Q2_FALL_BUILD_CONTRACT "{BUILD_CONTRACT}"\n'
        f'#define Q2_FALL_TOOL_CLOSURE_SHA256 "{closure_digest(root, constants)}"\n'
        "#endif\n"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != body:
        output.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
