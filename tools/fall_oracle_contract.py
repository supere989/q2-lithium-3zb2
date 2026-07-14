#!/usr/bin/env python3
"""Strict response and identity admission for q2-fall-oracle."""

from __future__ import annotations

import hashlib
import math
from collections.abc import Mapping
from typing import Any


SCHEMA = "q2-fall-oracle-v1"
BUILD_CONTRACT = "lithium-linux-c99-o1-f32-shared-fall-v1"
HEX = frozenset("0123456789abcdef")
COMMON = {"ok", "id", "op", "schema", "physics_identity", "tool_identity"}


class FallContractError(ValueError):
    """A fall oracle response is not sealed or self-consistent."""


def _mapping(value: object, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise FallContractError(f"{label} must be an object")
    return value


def _exact(value: Mapping[str, Any], keys: set[str], label: str) -> None:
    actual = set(value)
    if actual != keys:
        raise FallContractError(
            f"{label} fields differ; missing={sorted(keys - actual)}, "
            f"extra={sorted(actual - keys)}"
        )


def _sha(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in HEX for character in value)
    ):
        raise FallContractError(f"{label} must be a lowercase SHA-256")
    return value


def _number(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise FallContractError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result):
        raise FallContractError(f"{label} must be a finite number")
    return result


def validate_parameters(value: object) -> dict[str, Any]:
    parameters = _mapping(value, "fall parameters")
    _exact(parameters, {"fall_damagemod", "deathmatch", "dmflags"}, "fall parameters")
    modifier = _number(parameters["fall_damagemod"], "fall_damagemod")
    if modifier < 0 or modifier > 1000:
        raise FallContractError("fall_damagemod is out of range")
    if not isinstance(parameters["deathmatch"], bool):
        raise FallContractError("deathmatch must be boolean")
    if type(parameters["dmflags"]) is not int or not 0 <= parameters["dmflags"] <= 2147483647:
        raise FallContractError("dmflags is out of range")
    return dict(parameters)


def validate_source(value: object) -> dict[str, str]:
    source = _mapping(value, "fall source")
    fields = {
        "shared_c_sha256", "shared_h_sha256", "integration_sha256",
        "game_header_sha256", "constants_sha256", "build_contract",
        "tool_closure_sha256",
    }
    _exact(source, fields, "fall source")
    for field in fields - {"build_contract"}:
        _sha(source[field], field)
    if source["build_contract"] != BUILD_CONTRACT:
        raise FallContractError("fall build contract mismatch")
    return {field: str(source[field]) for field in fields}


def physics_identity(
    parameters: Mapping[str, Any], source: Mapping[str, Any], constants: str
) -> str:
    params = validate_parameters(parameters)
    validated = validate_source(source)
    if not isinstance(constants, str) or not constants:
        raise FallContractError("fall constants contract is empty")
    constants_sha = hashlib.sha256(constants.encode()).hexdigest()
    if constants_sha != validated["constants_sha256"]:
        raise FallContractError("fall constants digest mismatch")
    number = lambda value: format(float(value), ".9g")
    canonical = (
        f"schema={SCHEMA};tool={validated['tool_closure_sha256']};"
        f"shared_c={validated['shared_c_sha256']};shared_h={validated['shared_h_sha256']};"
        f"integration={validated['integration_sha256']};"
        f"game_header={validated['game_header_sha256']};"
        f"constants_sha256={validated['constants_sha256']};constants={constants};"
        f"build={validated['build_contract']};"
        f"fall_damagemod={number(params['fall_damagemod'])};"
        f"deathmatch={1 if params['deathmatch'] else 0};dmflags={params['dmflags']}"
    )
    return hashlib.sha256(canonical.encode()).hexdigest()


def _validate_input(value: object) -> dict[str, Any]:
    fall_input = _mapping(value, "fall input")
    fields = {
        "old_velocity_z", "velocity_z", "grapple_release_elapsed",
        "fall_damagemod", "modelindex", "movetype", "grounded", "hook_out",
        "grapple_present", "grapple_state", "waterlevel", "deathmatch",
        "dmflags", "health",
    }
    _exact(fall_input, fields, "fall input")
    for field in ("old_velocity_z", "velocity_z"):
        if not -32768 <= _number(fall_input[field], field) <= 32768:
            raise FallContractError(f"{field} is out of range")
    if not -86400 <= _number(
        fall_input["grapple_release_elapsed"], "grapple_release_elapsed"
    ) <= 86400:
        raise FallContractError("grapple_release_elapsed is out of range")
    if not 0 <= _number(fall_input["fall_damagemod"], "fall_damagemod") <= 1000:
        raise FallContractError("fall_damagemod is out of range")
    for field in ("modelindex", "movetype", "grapple_state", "waterlevel", "dmflags", "health"):
        if type(fall_input[field]) is not int:
            raise FallContractError(f"{field} must be an integer")
    for field in ("grounded", "hook_out", "grapple_present", "deathmatch"):
        if not isinstance(fall_input[field], bool):
            raise FallContractError(f"{field} must be boolean")
    for field, minimum, maximum in (
        ("modelindex", 0, 255),
        ("movetype", 0, 9),
        ("grapple_state", 0, 2),
        ("waterlevel", 0, 3),
        ("dmflags", 0, 2147483647),
        ("health", -1000000, 1000000),
    ):
        if not minimum <= fall_input[field] <= maximum:
            raise FallContractError(f"{field} is out of range")
    return dict(fall_input)


def validate_fall_response(value: object, *, expected_op: str | None = None) -> dict[str, Any]:
    record = _mapping(value, "fall response")
    if record.get("ok") is False:
        _exact(record, {"ok", "id", "error", "detail"}, "fall error")
        return dict(record)
    if record.get("ok") is not True:
        raise FallContractError("fall response ok must be true or false")
    if record.get("schema") != SCHEMA:
        raise FallContractError("fall schema mismatch")
    operation = record.get("op")
    if expected_op is not None and operation != expected_op:
        raise FallContractError(f"fall operation {operation!r} != {expected_op!r}")
    if not isinstance(record.get("id"), str) or len(record["id"]) > 127:
        raise FallContractError("fall id is invalid")
    _sha(record.get("tool_identity"), "fall tool identity")
    _sha(record.get("physics_identity"), "fall physics identity")
    if operation == "identity":
        fields = {"parameters", "constants", "source"}
        _exact(record, COMMON | fields, "fall identity")
        parameters = validate_parameters(record["parameters"])
        source = validate_source(record["source"])
        if record["tool_identity"] != source["tool_closure_sha256"]:
            raise FallContractError("fall tool identity differs from source closure")
        if record["physics_identity"] != physics_identity(parameters, source, record["constants"]):
            raise FallContractError("fall physics identity preimage mismatch")
        return dict(record)
    if operation != "evaluate":
        raise FallContractError(f"unknown fall operation {operation!r}")
    fields = {
        "input", "suppression", "severity", "delta", "fall_value",
        "fall_time_offset", "emit_event", "set_fall_state",
        "set_pain_debounce", "damage", "apply_damage",
        "unmitigated_health_after", "unmitigated_lethal",
    }
    _exact(record, COMMON | fields, "fall evaluation")
    _validate_input(record["input"])
    if record["suppression"] not in {
        "none", "not_player_model", "noclip", "hook_out", "airborne",
        "grapple", "underwater", "below_threshold",
    }:
        raise FallContractError("invalid fall suppression")
    if record["severity"] not in {"none", "footstep", "short", "fall", "far"}:
        raise FallContractError("invalid fall severity")
    for field in ("delta", "fall_value", "fall_time_offset"):
        _number(record[field], field)
    for field in ("damage", "unmitigated_health_after"):
        if type(record[field]) is not int:
            raise FallContractError(f"{field} must be an integer")
    for field in ("emit_event", "set_fall_state", "set_pain_debounce", "apply_damage", "unmitigated_lethal"):
        if not isinstance(record[field], bool):
            raise FallContractError(f"{field} must be boolean")
    return dict(record)


def admit_fall_response(value: object, identity: object) -> dict[str, Any]:
    """Admit one successful record against a separately pinned identity."""
    expected = validate_fall_response(identity, expected_op="identity")
    record = validate_fall_response(value)
    if record.get("ok") is not True:
        raise FallContractError("fall oracle returned an error")
    for field in ("schema", "tool_identity", "physics_identity"):
        if record[field] != expected[field]:
            raise FallContractError(f"fall {field} differs from pinned identity")
    if record["op"] == "evaluate":
        parameters = expected["parameters"]
        for field in ("fall_damagemod", "deathmatch", "dmflags"):
            if record["input"][field] != parameters[field]:
                raise FallContractError(f"fall input {field} differs from pinned identity")
    return record
