#!/usr/bin/env python3
"""Emit one canonical attestation for isolated q2ded/hook/CM/Pmove parity."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pty
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.hook_parity_contract import (  # noqa: E402
    build_attestation,
    canonical_bytes,
    file_sha256,
    source_closure_digest,
    validate_attestation,
    validate_hook_response,
)


ATTESTOR_FILES = (
    "tests/run_q2ded_hook_parity.py",
    "tools/hook_parity_contract.py",
    "tools/schemas/q2-hook-oracle-v1.response.schema.json",
    "tools/schemas/q2-hook-parity-attestation-v1.schema.json",
)


def probe_bsp_bytes() -> bytes:
    """Return the one canonical collision-valid IBSP-38 parity fixture."""

    planes = []
    for normal, distance, plane_type in (
        ((1, 0, 0), 0, 0),
        ((-1, 0, 0), 0, 3),
        ((-1, 0, 0), 2048, 3),
        ((1, 0, 0), 2048, 0),
        ((0, -1, 0), 2048, 4),
        ((0, 1, 0), 2048, 1),
        ((0, 0, -1), 128, 5),
        ((0, 0, 1), 0, 2),
    ):
        planes.append(struct.pack("<4fi", *normal, distance, plane_type))
    nodes = [
        struct.pack(
            "<i2i3h3h2H",
            plane,
            -2,
            index + 1 if index < 5 else -3,
            -4096,
            -4096,
            -4096,
            4096,
            4096,
            4096,
            0,
            0,
        )
        for index, plane in enumerate(range(2, 8))
    ]

    def leaf(contents: int, cluster: int, first: int, count: int) -> bytes:
        return struct.pack(
            "<ihh3h3h4H",
            contents,
            cluster,
            0,
            -4096,
            -4096,
            -4096,
            4096,
            4096,
            4096,
            0,
            0,
            first,
            count,
        )

    lumps = [b"" for _ in range(19)]
    lumps[0] = (
        b'{\n"classname" "worldspawn"\n}\n'
        b'{\n"classname" "info_player_deathmatch"\n"origin" "512 0 24"\n}\n\0'
    )
    lumps[1] = b"".join(planes)
    lumps[3] = struct.pack("<3iB", 1, 12, 12, 1)
    lumps[4] = b"".join(nodes)
    lumps[5] = struct.pack(
        "<8fii32si", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, b"oracle\0", -1
    )
    lumps[8] = leaf(1, -1, 0, 0) + leaf(0, 0, 0, 0) + leaf(1, -1, 0, 1)
    lumps[10] = struct.pack("<H", 0)
    lumps[13] = struct.pack(
        "<9f3i", -4096, -4096, -4096, 4096, 4096, 4096, 0, 0, 0, 0, 0, 0
    )
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
    return (
        struct.pack("<4sI", b"IBSP", 38)
        + b"".join(struct.pack("<2i", *entry) for entry in directory)
        + body
    )


def _parameter_block(args: argparse.Namespace) -> dict:
    values = {
        "hook_speed": args.hook_speed,
        "hook_pullspeed": args.hook_pullspeed,
        "hook_sky": bool(args.hook_sky),
        "hook_maxtime": args.hook_maxtime,
        "full_velocity_overwrite": True,
    }
    for name in ("hook_speed", "hook_pullspeed", "hook_maxtime"):
        if not math.isfinite(values[name]) or values[name] < 0:
            raise ValueError(f"{name} must be finite and nonnegative")
    return values


def _cvar(value: float) -> str:
    return format(value, ".9g")


def _run_records(command: list[str], records: list[dict], timeout: int = 10) -> list[dict]:
    payload = "".join(
        json.dumps(record, allow_nan=False, separators=(",", ":")) + "\n"
        for record in records
    )
    completed = subprocess.run(
        command,
        input=payload,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=timeout,
    )
    if completed.returncode:
        raise RuntimeError(
            f"{Path(command[0]).name} exited {completed.returncode}: {completed.stderr}"
        )
    responses = [json.loads(line) for line in completed.stdout.splitlines()]
    if len(responses) != len(records):
        raise RuntimeError(
            f"{Path(command[0]).name} returned {len(responses)} records for "
            f"{len(records)} requests"
        )
    return responses


def _run_q2ded(args: argparse.Namespace, bsp: bytes, parameters: dict) -> list[dict]:
    with tempfile.TemporaryDirectory(prefix="q2-hook-parity-") as directory:
        basedir = Path(directory)
        (basedir / "baseq2").mkdir()
        (basedir / "lithium" / "maps").mkdir(parents=True)
        user_lithium = basedir / "YamagiQ2" / "lithium"
        (user_lithium / "maps").mkdir(parents=True)
        shutil.copy2(args.game_module, user_lithium / "game.so")
        for destination in (
            basedir / "lithium" / "maps" / "hookprobe.bsp",
            user_lithium / "maps" / "hookprobe.bsp",
        ):
            destination.write_bytes(bsp)
        environment = os.environ.copy()
        environment["HOME"] = str(basedir)
        environment["XDG_DATA_HOME"] = str(basedir)
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [
                str(args.q2ded),
                "-datadir",
                str(basedir),
                "+set",
                "game",
                "lithium",
                "+set",
                "hook_speed",
                _cvar(parameters["hook_speed"]),
                "+set",
                "hook_pullspeed",
                _cvar(parameters["hook_pullspeed"]),
                "+set",
                "hook_sky",
                "1" if parameters["hook_sky"] else "0",
                "+set",
                "hook_maxtime",
                _cvar(parameters["hook_maxtime"]),
                "+map",
                "hookprobe",
            ],
            stdin=slave,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            close_fds=True,
            env=environment,
            cwd=basedir,
        )
        os.close(slave)
        try:
            time.sleep(0.5)
            os.write(master, b"sv hook_oracle_probe\nquit\n")
            output, _ = process.communicate(timeout=20)
        finally:
            os.close(master)
        server_output = output.decode("utf-8", errors="replace")
    if process.returncode:
        raise RuntimeError(f"q2ded exited {process.returncode}:\n{server_output}")
    samples = []
    for line in server_output.splitlines():
        marker = "Q2_HOOK_PARITY "
        if marker in line:
            samples.append(json.loads(line.split(marker, 1)[1]))
    if len(samples) != 9:
        raise RuntimeError(f"expected 9 q2ded samples, got {len(samples)}")
    return samples


def create_attestation(args: argparse.Namespace) -> tuple[dict, dict]:
    parameters = _parameter_block(args)
    oracle_parameters = {
        key: parameters[key]
        for key in ("hook_speed", "hook_pullspeed", "hook_sky", "hook_maxtime")
    }
    fixture_bytes = probe_bsp_bytes()
    fixture_sha = hashlib.sha256(fixture_bytes).hexdigest()
    samples = _run_q2ded(args, fixture_bytes, parameters)
    parameter_sample = samples[0]
    expected_parameter_sample = {"id": "parameters", "op": "parameters", **parameters}
    if parameter_sample != expected_parameter_sample:
        raise RuntimeError(
            f"q2ded parameter block {parameter_sample} != {expected_parameter_sample}"
        )
    server_samples = samples[1:]

    identity_request = {
        "id": "hook-identity",
        "op": "identity",
        **oracle_parameters,
    }
    hook_requests = []
    for sample in server_samples:
        if sample["op"] == "hook_landing":
            continue
        request = {
            key: value for key, value in sample.items() if not key.startswith("server_")
        }
        request.update(oracle_parameters)
        hook_requests.append(request)
    hook_responses = _run_records(
        [str(args.oracle)], [identity_request, *hook_requests]
    )
    hook_identity = validate_hook_response(hook_responses[0], expected_op="identity")
    for request, response in zip(hook_requests, hook_responses[1:]):
        validate_hook_response(response, expected_op=request["op"])
        if response["id"] != request["id"]:
            raise RuntimeError("hook response ID mismatch")
        if response["physics_identity"] != hook_identity["physics_identity"]:
            raise RuntimeError("hook response used a different physics identity")
        if response["tool_identity"] != hook_identity["tool_identity"]:
            raise RuntimeError("hook response used a different tool identity")

    ordinary_samples = [sample for sample in server_samples if sample["op"] != "hook_landing"]
    for sample, response in zip(ordinary_samples, hook_responses[1:]):
        for server_key, oracle_key in (
            ("server_velocity", "velocity"),
            ("server_distance", "distance"),
            ("server_hook_origin", "hook_origin"),
            ("server_action", "action"),
        ):
            if server_key in sample and sample[server_key] != response[oracle_key]:
                raise RuntimeError(
                    f"{sample['id']}: {server_key} mismatch "
                    f"{sample[server_key]} != {response[oracle_key]}"
                )

    landing = next(sample for sample in server_samples if sample["op"] == "hook_landing")
    pull_request = {
        "id": "landing-pull",
        "op": "pull",
        "owner_origin": landing["owner_origin"],
        "hook_origin": landing["hook_origin"],
        **oracle_parameters,
    }
    pull_result = _run_records([str(args.oracle)], [pull_request])[0]
    validate_hook_response(pull_result, expected_op="pull")
    if pull_result["physics_identity"] != hook_identity["physics_identity"]:
        raise RuntimeError("landing pull identity mismatch")

    with tempfile.TemporaryDirectory(prefix="q2-hook-landing-") as directory:
        map_path = Path(directory) / "hookprobe.bsp"
        map_path.write_bytes(fixture_bytes)
        collision_request = {
            "id": "landing-trace",
            "op": "box_trace",
            "start": landing["owner_origin"],
            "end": [0, 0, -64],
            "mins": [-16, -16, -24],
            "maxs": [16, 16, 32],
            "mask": 33619971,
        }
        collision_requests = [
            {"id": "cm-identity", "op": "identity"},
            collision_request,
        ]
        collision_identity, collision_result = _run_records(
            [str(args.cm_oracle), "--map", str(map_path)], collision_requests
        )
        pmove_request = {
            "id": "landing",
            "op": "simulate",
            "origin": landing["owner_origin"],
            "velocity": pull_result["velocity"],
            "gravity": args.gravity,
            "airaccelerate": args.airaccelerate,
            "snapinitial": False,
            "commands": landing["commands"],
        }
        pmove_requests = [
            {
                "id": "pmove-identity",
                "op": "identity",
                "gravity": args.gravity,
                "airaccelerate": args.airaccelerate,
            },
            pmove_request,
        ]
        pmove_identity, movement_result = _run_records(
            [str(args.pmove_oracle), "--map", str(map_path)], pmove_requests
        )

    if (
        collision_identity.get("ok") is not True
        or collision_identity.get("schema") != "q2-cm-oracle-v1"
        or collision_identity.get("map_sha256") != fixture_sha
    ):
        raise RuntimeError("collision identity does not bind the fixture")
    if (
        pmove_identity.get("ok") is not True
        or pmove_identity.get("schema") != "q2-pmove-oracle-v1"
        or pmove_identity.get("map_sha256") != fixture_sha
        or movement_result.get("physics_identity") != pmove_identity.get("physics_identity")
    ):
        raise RuntimeError("Pmove identity does not bind the fixture/trajectory")
    if landing["server_direct_entnum"] != 0:
        raise RuntimeError("q2ded direct trace hit a non-world entity")
    if landing["server_direct_endpos"] != collision_result["endpos"]:
        raise RuntimeError("exact-BSP CM end position mismatch")
    if landing["server_direct_fraction"] != collision_result["fraction"]:
        raise RuntimeError("exact-BSP CM fraction mismatch")
    final = movement_result["final"]
    for server_key, oracle_key in (
        ("server_origin_fixed", "origin_fixed"),
        ("server_velocity_fixed", "velocity_fixed"),
        ("server_grounded", "grounded"),
    ):
        if landing[server_key] != final[oracle_key]:
            raise RuntimeError(
                f"hook landing {server_key} mismatch "
                f"{landing[server_key]} != {final[oracle_key]}"
            )

    fixture = {
        "name": "hookprobe-v1",
        "bsp_sha256": fixture_sha,
        "bsp_bytes": len(fixture_bytes),
        "ibsp_version": 38,
    }
    identities = {
        "hook": {
            "schema": hook_identity["schema"],
            "physics_identity": hook_identity["physics_identity"],
            "tool_identity": hook_identity["tool_identity"],
            "source": hook_identity["source"],
        },
        "collision": {
            "schema": collision_identity["schema"],
            "physics_identity": collision_identity["physics_identity"],
            "map_sha256": collision_identity["map_sha256"],
        },
        "pmove": {
            "schema": pmove_identity["schema"],
            "physics_identity": pmove_identity["physics_identity"],
            "map_sha256": pmove_identity["map_sha256"],
        },
    }
    binaries = {
        "q2ded_sha256": file_sha256(args.q2ded),
        "probe_game_module_sha256": file_sha256(args.game_module),
        "hook_oracle_sha256": file_sha256(args.oracle),
        "cm_oracle_sha256": file_sha256(args.cm_oracle),
        "pmove_oracle_sha256": file_sha256(args.pmove_oracle),
    }
    evidence = {
        "server_parameters": parameter_sample,
        "server_samples": server_samples,
        "hook_requests": hook_requests,
        "hook_responses": hook_responses[1:],
        "landing_pull_request": pull_request,
        "landing_pull_response": pull_result,
        "collision_request": collision_request,
        "collision_response": collision_result,
        "pmove_request": pmove_request,
        "pmove_response": movement_result,
    }
    checks = {
        "bsp_identity": True,
        "collision_parity": True,
        "hook_identity_consistency": True,
        "hook_vector_parity": True,
        "pmove_landing_parity": True,
        "q2ded_parameter_block": True,
        "strict_hook_responses": True,
    }
    attestation = build_attestation(
        parameters=parameters,
        fixture=fixture,
        identities=identities,
        binaries=binaries,
        evidence=evidence,
        checks=checks,
        attestor_closure_sha256=source_closure_digest(ROOT, ATTESTOR_FILES),
    )
    validate_attestation(
        attestation, expected_parameters=parameters, evidence=evidence
    )
    return attestation, evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--q2ded", type=Path, required=True)
    parser.add_argument(
        "--game-module",
        type=Path,
        default=ROOT / "lithium" / "gamex86_64.so",
    )
    parser.add_argument(
        "--oracle", type=Path, default=ROOT / "tools" / "q2-hook-oracle"
    )
    parser.add_argument("--pmove-oracle", type=Path, required=True)
    parser.add_argument("--cm-oracle", type=Path, required=True)
    parser.add_argument("--hook-speed", type=float, default=900.0)
    parser.add_argument("--hook-pullspeed", type=float, default=1700.0)
    parser.add_argument("--hook-sky", type=int, choices=(0, 1), default=0)
    parser.add_argument("--hook-maxtime", type=float, default=5.0)
    parser.add_argument("--gravity", type=int, default=800)
    parser.add_argument("--airaccelerate", type=float, default=0.0)
    args = parser.parse_args()
    for binary in (
        args.q2ded,
        args.game_module,
        args.oracle,
        args.pmove_oracle,
        args.cm_oracle,
    ):
        if not binary.is_file():
            parser.error(f"missing file: {binary}")
    attestation, _ = create_attestation(args)
    sys.stdout.buffer.write(canonical_bytes(attestation))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
