from __future__ import annotations

from copy import deepcopy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.fall_oracle_contract import (
    admit_fall_response,
    FallContractError,
    validate_fall_response,
)


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tools" / "q2-fall-oracle"


def invoke(requests: list[dict]) -> list[dict]:
    payload = "".join(
        json.dumps(request, separators=(",", ":")) + "\n" for request in requests
    )
    return invoke_raw(payload)


def invoke_raw(payload: str) -> list[dict]:
    result = subprocess.run(
        [str(ORACLE)], input=payload, text=True, cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise AssertionError(f"fall oracle exited {result.returncode}: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines()]


def request(velocity: float = -1000, **changes: object) -> dict:
    base = {
        "id": "fall",
        "op": "evaluate",
        "old_velocity_z": velocity,
        "velocity_z": 0,
        "grapple_release_elapsed": 1,
        "fall_damagemod": 1,
        "modelindex": 255,
        "movetype": 4,
        "grounded": True,
        "hook_out": False,
        "grapple_present": False,
        "grapple_state": 0,
        "waterlevel": 0,
        "deathmatch": True,
        "dmflags": 0,
        "health": 100,
    }
    base.update(changes)
    return base


class FallOracleTests(unittest.TestCase):
    def test_machine_readable_schemas_are_strict(self) -> None:
        request_schema = json.loads(
            (ROOT / "tools/schemas/q2-fall-oracle-v1.schema.json").read_text()
        )
        response_schema = json.loads(
            (ROOT / "tools/schemas/q2-fall-oracle-v1.response.schema.json").read_text()
        )
        self.assertEqual(request_schema["$id"], "urn:q2-ml:q2-fall-oracle-v1")
        self.assertEqual(response_schema["$id"], "urn:q2-ml:q2-fall-oracle-v1:response")
        for variant in request_schema["oneOf"]:
            self.assertFalse(variant["additionalProperties"])
        self.assertIn("unevaluatedProperties", response_schema["oneOf"][0])

    def test_tesla_velocity_vectors(self) -> None:
        velocities = (-300, -400, -600, -1000, -1600)
        responses = invoke([request(value, id=f"v{abs(value)}") for value in velocities])
        for response in responses:
            validate_fall_response(response, expected_op="evaluate")
        self.assertEqual(
            [item["severity"] for item in responses],
            ["footstep", "short", "fall", "far", "far"],
        )
        self.assertEqual([item["delta"] for item in responses], [9, 16, 36, 100, 256])
        self.assertEqual([item["damage"] for item in responses], [0, 0, 3, 35, 113])
        self.assertEqual([item["fall_value"] for item in responses], [0, 8, 18, 40, 40])

    def test_waterlevels_zero_through_three(self) -> None:
        responses = invoke([request(-1000, id=f"water{level}", waterlevel=level) for level in range(4)])
        self.assertEqual([item["delta"] for item in responses], [100, 50, 25, 100])
        self.assertEqual([item["severity"] for item in responses], ["far", "fall", "short", "none"])
        self.assertEqual([item["damage"] for item in responses], [35, 10, 0, 0])
        self.assertEqual(responses[3]["suppression"], "underwater")

    def test_tesla_velocity_water_cross_product(self) -> None:
        table = {
            -300: [(9, "footstep", 0), (4.5, "footstep", 0), (2.25, "footstep", 0), (9, "none", 0)],
            -400: [(16, "short", 0), (8, "footstep", 0), (4, "footstep", 0), (16, "none", 0)],
            -600: [(36, "fall", 3), (18, "short", 0), (9, "footstep", 0), (36, "none", 0)],
            -1000: [(100, "far", 35), (50, "fall", 10), (25, "short", 0), (100, "none", 0)],
            -1600: [(256, "far", 113), (128, "far", 49), (64, "far", 17), (256, "none", 0)],
        }
        requests = [
            request(velocity, id=f"v{abs(velocity)}-w{water}", waterlevel=water)
            for velocity in table
            for water in range(4)
        ]
        responses = iter(invoke(requests))
        for velocity, expected_levels in table.items():
            for water, (delta, severity, damage) in enumerate(expected_levels):
                with self.subTest(velocity=velocity, waterlevel=water):
                    response = next(responses)
                    self.assertEqual(response["delta"], delta)
                    self.assertEqual(response["severity"], severity)
                    self.assertEqual(response["damage"], damage)

    def test_flags_grapple_and_preconditions(self) -> None:
        no_falling, released, pulling, hook_out, wrong_model, noclip, airborne = invoke([
            request(dmflags=8),
            request(grapple_release_elapsed=0.1),
            request(grapple_present=True, grapple_state=1),
            request(grounded=False, hook_out=True, velocity_z=-500),
            request(modelindex=1),
            request(movetype=1),
            request(grounded=False, velocity_z=-1100),
        ])
        self.assertEqual(no_falling["severity"], "far")
        self.assertEqual(no_falling["damage"], 35)
        self.assertFalse(no_falling["apply_damage"])
        self.assertEqual(
            [item["suppression"] for item in (released, pulling, hook_out, wrong_model, noclip, airborne)],
            ["grapple", "grapple", "hook_out", "not_player_model", "noclip", "airborne"],
        )

    def test_damage_modifier_health_and_lethality(self) -> None:
        lethal, survives, disabled = invoke([
            request(fall_damagemod=1.5, health=52),
            request(fall_damagemod=1.5, health=53),
            request(fall_damagemod=1.5, health=52, dmflags=8),
        ])
        self.assertEqual(lethal["damage"], 52)
        self.assertEqual(lethal["unmitigated_health_after"], 0)
        self.assertTrue(lethal["unmitigated_lethal"])
        self.assertEqual(survives["unmitigated_health_after"], 1)
        self.assertFalse(survives["unmitigated_lethal"])
        self.assertEqual(disabled["unmitigated_health_after"], 52)
        self.assertFalse(disabled["unmitigated_lethal"])

    def test_identity_binds_sources_constants_build_and_parameters(self) -> None:
        default, modified, flags = invoke([
            {"id": "default", "op": "identity"},
            {"id": "modified", "op": "identity", "fall_damagemod": 1.5},
            {"id": "flags", "op": "identity", "dmflags": 8},
        ])
        for response in (default, modified, flags):
            validate_fall_response(response, expected_op="identity")
        self.assertEqual(default["tool_identity"], modified["tool_identity"])
        self.assertNotEqual(default["physics_identity"], modified["physics_identity"])
        self.assertNotEqual(default["physics_identity"], flags["physics_identity"])
        mutated = deepcopy(default)
        mutated["constants"] += ",mutation=1"
        with self.assertRaises(FallContractError):
            validate_fall_response(mutated, expected_op="identity")

        evaluation = invoke([request()])[0]
        admit_fall_response(evaluation, default)
        with self.assertRaises(FallContractError):
            admit_fall_response(evaluation, modified)
        invalid_input = deepcopy(evaluation)
        invalid_input["input"]["velocity_z"] = 32769
        with self.assertRaises(FallContractError):
            admit_fall_response(invalid_input, default)

    def test_identity_generation_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="fall-identity-") as directory:
            first = Path(directory) / "first.h"
            second = Path(directory) / "second.h"
            for destination in (first, second):
                subprocess.run(
                    ["python3", "tools/gen_fall_identity.py", ".", str(destination)],
                    cwd=ROOT, check=True,
                )
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_malformed_nonfinite_out_of_range_and_unknown_fail_closed(self) -> None:
        cases = [
            request(old_velocity_z=float("nan")),
            request(old_velocity_z=32769),
            request(grapple_release_elapsed=86401),
            request(fall_damagemod=-1),
            request(modelindex=256),
            request(movetype=10),
            request(waterlevel=4),
            request(health=1000001),
        ]
        responses = invoke(cases)
        self.assertTrue(all(item["ok"] is False for item in responses))
        unknown = invoke_raw(json.dumps({**request(), "unknown": 1}) + "\n")[0]
        duplicate = invoke_raw('{"op":"identity","op":"identity"}\n')[0]
        inapplicable = invoke_raw('{"op":"identity","health":100}\n')[0]
        exponent, leading_zero = invoke_raw(
            '{"op":"identity","fall_damagemod":1e0}\n'
            '{"op":"identity","fall_damagemod":01}\n'
        )
        self.assertFalse(unknown["ok"])
        self.assertFalse(duplicate["ok"])
        self.assertFalse(inapplicable["ok"])
        self.assertTrue(exponent["ok"])
        self.assertFalse(leading_zero["ok"])


if __name__ == "__main__":
    unittest.main()
