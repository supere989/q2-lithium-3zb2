"""Strict hook-oracle responses and canonical parity attestations."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Any, Mapping, Sequence


HOOK_SCHEMA = "q2-hook-oracle-v1"
ATTESTATION_SCHEMA = "q2-hook-parity-attestation-v1"
BUILD_CONTRACT = "lithium-linux-c99-o1-f32-shared-hook-v2"
HEX = frozenset("0123456789abcdef")


class ContractError(ValueError):
    """Raised when an oracle record or parity attestation is not admissible."""


def canonical_bytes(value: object) -> bytes:
    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical_digest(value: object) -> str:
    return sha256_bytes(canonical_bytes(value))


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_closure_digest(root: Path, names: Sequence[str]) -> str:
    digest = hashlib.sha256()
    digest.update(b"q2-hook-attestor-closure-v1\n")
    for name in sorted(names):
        digest.update(f"{name}\0{file_sha256(root / name)}\n".encode())
    return digest.hexdigest()


def _exact_keys(value: Mapping[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ContractError(f"{label} keys differ; missing={missing}, extra={extra}")


def _mapping(value: object, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ContractError(f"{label} must be an object")
    return value


def _digest(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in HEX for character in value)
    ):
        raise ContractError(f"{label} must be a lowercase SHA-256")
    return value


def _number(value: object, label: str, *, nonnegative: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ContractError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result) or (nonnegative and result < 0):
        raise ContractError(f"{label} must be finite and nonnegative")
    return result


def _vec3(value: object, label: str) -> None:
    if not isinstance(value, list) or len(value) != 3:
        raise ContractError(f"{label} must be a three-number array")
    for index, item in enumerate(value):
        _number(item, f"{label}[{index}]")


def validate_parameters(value: object) -> dict[str, Any]:
    parameters = _mapping(value, "hook parameters")
    _exact_keys(
        parameters,
        {
            "hook_speed",
            "hook_pullspeed",
            "hook_sky",
            "hook_maxtime",
            "full_velocity_overwrite",
        },
        "hook parameters",
    )
    _number(parameters["hook_speed"], "hook_speed", nonnegative=True)
    _number(parameters["hook_pullspeed"], "hook_pullspeed", nonnegative=True)
    _number(parameters["hook_maxtime"], "hook_maxtime", nonnegative=True)
    if not isinstance(parameters["hook_sky"], bool):
        raise ContractError("hook_sky must be boolean")
    if parameters["full_velocity_overwrite"] is not True:
        raise ContractError("full_velocity_overwrite must be true")
    return dict(parameters)


def validate_source(value: object) -> dict[str, str]:
    source = _mapping(value, "hook source")
    expected = {
        "shared_c_sha256",
        "shared_h_sha256",
        "integration_sha256",
        "math_sha256",
        "build_contract",
        "tool_closure_sha256",
    }
    _exact_keys(source, expected, "hook source")
    for field in expected - {"build_contract"}:
        _digest(source[field], field)
    if source["build_contract"] != BUILD_CONTRACT:
        raise ContractError("unexpected hook build contract")
    return {field: str(source[field]) for field in expected}


def hook_physics_identity(parameters: Mapping[str, Any], source: Mapping[str, Any]) -> str:
    params = validate_parameters(parameters)
    validated_source = validate_source(source)
    number = lambda value: format(float(value), ".9g")
    canonical = (
        f"schema={HOOK_SCHEMA};shared_c={validated_source['shared_c_sha256']};"
        f"shared_h={validated_source['shared_h_sha256']};"
        f"integration={validated_source['integration_sha256']};"
        f"math={validated_source['math_sha256']};"
        f"build={validated_source['build_contract']};"
        f"tool_closure={validated_source['tool_closure_sha256']};"
        f"hook_speed={number(params['hook_speed'])};"
        f"hook_pullspeed={number(params['hook_pullspeed'])};"
        f"hook_sky={1 if params['hook_sky'] else 0};"
        f"hook_maxtime={number(params['hook_maxtime'])};"
        "full_velocity_overwrite=1"
    )
    return sha256_bytes(canonical.encode())


def validate_hook_response(value: object, *, expected_op: str | None = None) -> dict:
    record = _mapping(value, "hook response")
    if record.get("ok") is False:
        _exact_keys(record, {"ok", "id", "error", "detail"}, "error response")
        if not all(isinstance(record[field], str) for field in ("id", "error", "detail")):
            raise ContractError("error response strings are invalid")
        return dict(record)
    common = {"ok", "id", "op", "schema", "physics_identity", "tool_identity"}
    if record.get("ok") is not True:
        raise ContractError("hook response ok must be boolean true/false")
    if not isinstance(record.get("id"), str) or len(record["id"]) > 127:
        raise ContractError("hook response id is invalid")
    if record.get("schema") != HOOK_SCHEMA:
        raise ContractError("hook response schema mismatch")
    operation = record.get("op")
    if expected_op is not None and operation != expected_op:
        raise ContractError(f"hook response op {operation!r} != {expected_op!r}")
    _digest(record.get("physics_identity"), "hook physics identity")
    _digest(record.get("tool_identity"), "hook tool identity")

    variants = {
        "identity": {"parameters", "source"},
        "launch": {"velocity"},
        "pull": {
            "target_source",
            "distance",
            "prior_velocity",
            "velocity",
            "full_velocity_overwrite",
        },
        "touch": {"action", "attached"},
        "backoff": {"hook_origin"},
    }
    fields = variants.get(operation)
    if fields is None:
        raise ContractError(f"unknown successful hook operation {operation!r}")
    _exact_keys(record, common | fields, f"{operation} response")
    if operation == "identity":
        params = validate_parameters(record["parameters"])
        source = validate_source(record["source"])
        if record["tool_identity"] != source["tool_closure_sha256"]:
            raise ContractError("hook tool identity differs from source closure")
        if record["physics_identity"] != hook_physics_identity(params, source):
            raise ContractError("hook physics identity does not match its canonical preimage")
    elif operation == "launch":
        _vec3(record["velocity"], "launch velocity")
    elif operation == "pull":
        if record["target_source"] not in {"hook_origin", "enemy_origin"}:
            raise ContractError("invalid hook target source")
        _number(record["distance"], "hook distance", nonnegative=True)
        _vec3(record["prior_velocity"], "prior velocity")
        _vec3(record["velocity"], "pull velocity")
        if record["full_velocity_overwrite"] is not True:
            raise ContractError("pull did not attest full velocity overwrite")
    elif operation == "touch":
        actions = {
            "attach",
            "ignore_owner",
            "ignore_invalid_owner",
            "ignore_nonblocking",
            "reset_sky",
        }
        if record["action"] not in actions or not isinstance(record["attached"], bool):
            raise ContractError("invalid touch response")
        if record["attached"] != (record["action"] == "attach"):
            raise ContractError("touch attached flag contradicts action")
    else:
        _vec3(record["hook_origin"], "backoff hook origin")
    return dict(record)

def build_attestation(
    *,
    parameters: Mapping[str, Any],
    fixture: Mapping[str, Any],
    identities: Mapping[str, Any],
    binaries: Mapping[str, Any],
    evidence: Mapping[str, Any],
    checks: Mapping[str, bool],
    attestor_closure_sha256: str,
) -> dict:
    params = validate_parameters(parameters)
    hook = _mapping(identities.get("hook"), "hook attestation identity")
    hook_source = validate_source(hook.get("source"))
    expected_hook = hook_physics_identity(params, hook_source)
    if hook.get("physics_identity") != expected_hook:
        raise ContractError("attested hook identity does not bind its parameter block")
    if hook.get("tool_identity") != hook_source["tool_closure_sha256"]:
        raise ContractError("attested hook tool identity differs from source closure")
    if identities.get("collision", {}).get("map_sha256") != fixture.get("bsp_sha256"):
        raise ContractError("collision identity does not bind the fixture BSP")
    if identities.get("pmove", {}).get("map_sha256") != fixture.get("bsp_sha256"):
        raise ContractError("Pmove identity does not bind the fixture BSP")
    if not checks or not all(value is True for value in checks.values()):
        raise ContractError("a parity check did not pass")
    return {
        "schema": ATTESTATION_SCHEMA,
        "passed": True,
        "parameters": params,
        "fixture": dict(fixture),
        "identities": {key: dict(value) for key, value in identities.items()},
        "binaries": dict(binaries),
        "attestor_closure_sha256": _digest(
            attestor_closure_sha256, "attestor closure"
        ),
        "evidence": {
            "case_count": len(evidence.get("server_samples", ())),
            "case_ids": [
                str(sample.get("id", ""))
                for sample in evidence.get("server_samples", ())
            ],
            "vector_results_sha256": canonical_digest(evidence),
        },
        "checks": dict(sorted(checks.items())),
    }


def validate_attestation(
    value: object,
    *,
    expected_parameters: Mapping[str, Any] | None = None,
    evidence: Mapping[str, Any] | None = None,
) -> dict:
    record = _mapping(value, "hook parity attestation")
    expected_keys = {
        "schema",
        "passed",
        "parameters",
        "fixture",
        "identities",
        "binaries",
        "attestor_closure_sha256",
        "evidence",
        "checks",
    }
    _exact_keys(record, expected_keys, "hook parity attestation")
    if record["schema"] != ATTESTATION_SCHEMA or record["passed"] is not True:
        raise ContractError("hook parity attestation did not pass")
    parameters = validate_parameters(record["parameters"])
    if expected_parameters is not None and parameters != validate_parameters(
        expected_parameters
    ):
        raise ContractError("hook parameter block differs from the required runtime")
    fixture = _mapping(record["fixture"], "fixture")
    _exact_keys(
        fixture,
        {"name", "bsp_sha256", "bsp_bytes", "ibsp_version"},
        "fixture",
    )
    _digest(fixture["bsp_sha256"], "fixture BSP")
    if (
        fixture["name"] != "hookprobe-v1"
        or not isinstance(fixture["bsp_bytes"], int)
        or fixture["bsp_bytes"] <= 0
        or fixture["ibsp_version"] != 38
    ):
        raise ContractError("fixture identity is invalid")
    identities = _mapping(record["identities"], "identities")
    _exact_keys(identities, {"hook", "collision", "pmove"}, "identities")
    hook = _mapping(identities["hook"], "hook identity")
    _exact_keys(
        hook,
        {"schema", "physics_identity", "tool_identity", "source"},
        "hook identity",
    )
    if hook["schema"] != HOOK_SCHEMA:
        raise ContractError("hook identity schema mismatch")
    source = validate_source(hook["source"])
    if hook["tool_identity"] != source["tool_closure_sha256"]:
        raise ContractError("hook tool closure mismatch")
    if hook["physics_identity"] != hook_physics_identity(parameters, source):
        raise ContractError("hook physics identity mismatch")
    for name, schema in (
        ("collision", "q2-cm-oracle-v1"),
        ("pmove", "q2-pmove-oracle-v1"),
    ):
        identity = _mapping(identities[name], f"{name} identity")
        _exact_keys(identity, {"schema", "physics_identity", "map_sha256"}, name)
        if identity["schema"] != schema:
            raise ContractError(f"{name} schema mismatch")
        _digest(identity["physics_identity"], f"{name} physics identity")
        if identity["map_sha256"] != fixture["bsp_sha256"]:
            raise ContractError(f"{name} map identity mismatch")
    binaries = _mapping(record["binaries"], "binary identities")
    expected_binaries = {
        "q2ded_sha256",
        "probe_game_module_sha256",
        "hook_oracle_sha256",
        "cm_oracle_sha256",
        "pmove_oracle_sha256",
    }
    _exact_keys(binaries, expected_binaries, "binary identities")
    for name in expected_binaries:
        _digest(binaries[name], name)
    _digest(record["attestor_closure_sha256"], "attestor closure")
    proof = _mapping(record["evidence"], "evidence identity")
    _exact_keys(
        proof,
        {"case_count", "case_ids", "vector_results_sha256"},
        "evidence identity",
    )
    if not isinstance(proof["case_count"], int) or proof["case_count"] <= 0:
        raise ContractError("invalid parity case count")
    if (
        not isinstance(proof["case_ids"], list)
        or len(proof["case_ids"]) != proof["case_count"]
        or not all(isinstance(item, str) and item for item in proof["case_ids"])
    ):
        raise ContractError("invalid parity case IDs")
    _digest(proof["vector_results_sha256"], "vector/results evidence")
    if evidence is not None and proof["vector_results_sha256"] != canonical_digest(
        evidence
    ):
        raise ContractError("vector/results digest mismatch")
    checks = _mapping(record["checks"], "checks")
    if not checks or not all(value is True for value in checks.values()):
        raise ContractError("attestation contains a failed/non-boolean check")
    return dict(record)
