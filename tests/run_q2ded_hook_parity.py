#!/usr/bin/env python3
"""Compare an isolated probe-enabled Lithium q2ded module with q2-hook-oracle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pty
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def write_probe_bsp(path: Path) -> None:
    """Write a collision-valid IBSP38 map sufficient to initialize q2ded."""
    planes = []
    for normal, distance, plane_type in (
        ((1, 0, 0), 0, 0), ((-1, 0, 0), 0, 3),
        ((-1, 0, 0), 2048, 3), ((1, 0, 0), 2048, 0),
        ((0, -1, 0), 2048, 4), ((0, 1, 0), 2048, 1),
        ((0, 0, -1), 128, 5), ((0, 0, 1), 0, 2),
    ):
        planes.append(struct.pack("<4fi", *normal, distance, plane_type))
    nodes = []
    for index, plane in enumerate(range(2, 8)):
        nodes.append(struct.pack(
            "<i2i3h3h2H", plane, -2, index + 1 if index < 5 else -3,
            -4096, -4096, -4096, 4096, 4096, 4096, 0, 0,
        ))
    leaf = lambda contents, cluster, first, count: struct.pack(
        "<ihh3h3h4H", contents, cluster, 0, -4096, -4096, -4096,
        4096, 4096, 4096, 0, 0, first, count,
    )
    lumps = [b"" for _ in range(19)]
    lumps[0] = (b'{\n"classname" "worldspawn"\n}\n'
                b'{\n"classname" "info_player_deathmatch"\n"origin" "512 0 24"\n}\n\0')
    lumps[1] = b"".join(planes)
    lumps[3] = struct.pack("<3iB", 1, 12, 12, 1)
    lumps[4] = b"".join(nodes)
    lumps[5] = struct.pack("<8fii32si", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, b"oracle\0", -1)
    lumps[8] = leaf(1, -1, 0, 0) + leaf(0, 0, 0, 0) + leaf(1, -1, 0, 1)
    lumps[10] = struct.pack("<H", 0)
    lumps[13] = struct.pack("<9f3i", -4096, -4096, -4096, 4096, 4096, 4096, 0, 0, 0, 0, 0, 0)
    # dbrush.firstside indexes the brushside array below (records 0..5),
    # whose planenum fields point at the six actual floor planes 2..7.
    lumps[14] = struct.pack("<3i", 0, 6, 1)
    lumps[15] = b"".join(struct.pack("<Hh", index, 0) for index in range(2, 8))
    lumps[17] = struct.pack("<2i", 0, 0)
    offset = 160
    body = bytearray()
    directory = []
    for lump in lumps:
        padding = (-offset) & 3
        body.extend(b"\0" * padding)
        offset += padding
        directory.append((offset, len(lump)))
        body.extend(lump)
        offset += len(lump)
    path.write_bytes(
        struct.pack("<4sI", b"IBSP", 38)
        + b"".join(struct.pack("<2i", *entry) for entry in directory)
        + body
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--q2ded", type=Path, required=True)
    parser.add_argument("--game-module", type=Path, default=ROOT / "lithium" / "gamex86_64.so")
    parser.add_argument("--oracle", type=Path, default=ROOT / "tools" / "q2-hook-oracle")
    parser.add_argument("--pmove-oracle", type=Path, required=True)
    parser.add_argument("--cm-oracle", type=Path, required=True)
    args = parser.parse_args()
    for binary in (args.q2ded, args.game_module, args.oracle, args.pmove_oracle, args.cm_oracle):
        if not binary.is_file():
            parser.error(f"missing file: {binary}")

    with tempfile.TemporaryDirectory(prefix="q2-hook-parity-") as directory:
        basedir = Path(directory)
        (basedir / "baseq2").mkdir()
        (basedir / "lithium").mkdir()
        (basedir / "lithium" / "maps").mkdir()
        user_lithium = basedir / "YamagiQ2" / "lithium"
        (user_lithium / "maps").mkdir(parents=True)
        shutil.copy2(args.game_module, user_lithium / "game.so")
        write_probe_bsp(basedir / "lithium" / "maps" / "hookprobe.bsp")
        write_probe_bsp(user_lithium / "maps" / "hookprobe.bsp")
        environment = os.environ.copy()
        environment["HOME"] = str(basedir)
        environment["XDG_DATA_HOME"] = str(basedir)
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [str(args.q2ded), "-datadir", str(basedir), "+set", "game", "lithium",
             "+map", "hookprobe"], stdin=slave, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, close_fds=True, env=environment, cwd=basedir,
        )
        os.close(slave)
        try:
            time.sleep(0.5)
            os.write(master, b"sv hook_oracle_probe\nquit\n")
            output, _ = process.communicate(timeout=20)
        finally:
            os.close(master)
        probe_bsp_bytes = (user_lithium / "maps" / "hookprobe.bsp").read_bytes()
        server_output = output.decode("utf-8", errors="replace")
    if process.returncode:
        raise SystemExit(f"q2ded exited {process.returncode}:\n{server_output}")
    samples = []
    for line in server_output.splitlines():
        marker = "Q2_HOOK_PARITY "
        if marker in line:
            samples.append(json.loads(line.split(marker, 1)[1]))
    if len(samples) != 8:
        raise SystemExit(f"expected 8 q2ded samples, got {len(samples)}:\n{server_output}")

    requests = []
    for sample in samples:
        if sample["op"] == "hook_landing":
            continue
        requests.append({key: value for key, value in sample.items() if not key.startswith("server_")})
    oracle = subprocess.run(
        [str(args.oracle)], input="".join(json.dumps(item) + "\n" for item in requests),
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=10,
    )
    if oracle.returncode:
        raise SystemExit(f"oracle exited {oracle.returncode}: {oracle.stderr}")
    actual = [json.loads(line) for line in oracle.stdout.splitlines()]
    ordinary_samples = [sample for sample in samples if sample["op"] != "hook_landing"]
    if len(actual) != len(ordinary_samples):
        raise SystemExit("oracle response count mismatch")
    for sample, result in zip(ordinary_samples, actual):
        if "server_velocity" in sample and sample["server_velocity"] != result["velocity"]:
            raise SystemExit(f"{sample['id']}: velocity mismatch {sample['server_velocity']} != {result['velocity']}")
        if "server_distance" in sample and sample["server_distance"] != result["distance"]:
            raise SystemExit(f"{sample['id']}: distance mismatch {sample['server_distance']} != {result['distance']}")
        if "server_hook_origin" in sample and sample["server_hook_origin"] != result["hook_origin"]:
            raise SystemExit(f"{sample['id']}: backoff mismatch {sample['server_hook_origin']} != {result['hook_origin']}")
        if "server_action" in sample and sample["server_action"] != result["action"]:
            raise SystemExit(f"{sample['id']}: touch mismatch {sample['server_action']} != {result['action']}")
    landing = next(sample for sample in samples if sample["op"] == "hook_landing")
    pull_request = {
        "id": "landing-pull", "op": "pull",
        "owner_origin": landing["owner_origin"], "hook_origin": landing["hook_origin"],
        "hook_pullspeed": landing["hook_pullspeed"],
    }
    pull = subprocess.run(
        [str(args.oracle)], input=json.dumps(pull_request) + "\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
    )
    pull_result = json.loads(pull.stdout)
    with tempfile.TemporaryDirectory(prefix="q2-hook-landing-") as directory:
        map_path = Path(directory) / "hookprobe.bsp"
        map_path.write_bytes(probe_bsp_bytes)
        collision_request = {
            "id": "landing-trace", "op": "box_trace",
            "start": landing["owner_origin"], "end": [0, 0, -64],
            "mins": [-16, -16, -24], "maxs": [16, 16, 32], "mask": 33619971,
        }
        collision = subprocess.run(
            [str(args.cm_oracle), "--map", str(map_path)],
            input=json.dumps({"id": "cm-identity", "op": "identity"}) + "\n"
                  + json.dumps(collision_request) + "\n", text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
        collision_identity, collision_result = [
            json.loads(line) for line in collision.stdout.splitlines()
        ]
        pmove_request = {
            "id": "landing", "op": "simulate", "origin": landing["owner_origin"],
            "velocity": pull_result["velocity"], "gravity": landing["gravity"],
            "snapinitial": False, "commands": landing["commands"],
        }
        movement = subprocess.run(
            [str(args.pmove_oracle), "--map", str(map_path)],
            input=json.dumps(pmove_request) + "\n", text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
    movement_result = json.loads(movement.stdout)
    final = movement_result["final"]
    if landing["server_direct_entnum"] != 0:
        raise SystemExit(f"hook-landing: q2ded direct trace hit non-world entity {landing['server_direct_entnum']}")
    if landing["server_direct_endpos"] != collision_result["endpos"]:
        raise SystemExit(
            f"hook-landing: exact-BSP CM endpos mismatch "
            f"{landing['server_direct_endpos']} != {collision_result['endpos']}"
        )
    if landing["server_direct_fraction"] != collision_result["fraction"]:
        raise SystemExit(
            f"hook-landing: exact-BSP CM fraction mismatch "
            f"{landing['server_direct_fraction']} != {collision_result['fraction']}"
        )
    for server_key, oracle_key in (
        ("server_origin_fixed", "origin_fixed"),
        ("server_velocity_fixed", "velocity_fixed"),
        ("server_grounded", "grounded"),
    ):
        if landing[server_key] != final[oracle_key]:
            raise SystemExit(
                f"hook-landing: {server_key} mismatch {landing[server_key]} != {final[oracle_key]}; "
                f"q2ded bounds={landing.get('server_mins')}/{landing.get('server_maxs')}; "
                f"direct={landing.get('server_direct_endpos')} fraction={landing.get('server_direct_fraction')} "
                f"ent={landing.get('server_direct_entnum')}"
            )
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    map_sha = hashlib.sha256(probe_bsp_bytes).hexdigest()
    print(f"isolated q2ded hook parity: {len(samples)} cases matched (including exact-BSP collision/landing)")
    print(
        "parity identities: "
        f"hook={pull_result['physics_identity']} "
        f"cm={collision_identity['physics_identity']} "
        f"pmove={movement_result['physics_identity']} "
        f"map={map_sha} q2ded={sha(args.q2ded)} probe_game={sha(args.game_module)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
