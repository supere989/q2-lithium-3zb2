#!/usr/bin/env python3
import hashlib
import sys
from pathlib import Path

root = Path(sys.argv[1])
output = Path(sys.argv[2])

def digest(name: str) -> str:
    return hashlib.sha256((root / name).read_bytes()).hexdigest()

output.write_text(
    "#ifndef Q2_HOOK_ORACLE_IDENTITY_H\n#define Q2_HOOK_ORACLE_IDENTITY_H\n"
    f'#define Q2_HOOK_SHARED_C_SHA256 "{digest("ml_hook_physics.c")}"\n'
    f'#define Q2_HOOK_SHARED_H_SHA256 "{digest("ml_hook_physics.h")}"\n'
    f'#define Q2_HOOK_INTEGRATION_SHA256 "{digest("l_hook.c")}"\n'
    f'#define Q2_HOOK_MATH_SHA256 "{digest("q_shared.c")}"\n'
    '#define Q2_HOOK_BUILD_CONTRACT "lithium-linux-c99-o1-f32-shared-hook-v1"\n'
    "#endif\n"
)
