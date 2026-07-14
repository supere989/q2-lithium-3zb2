from __future__ import annotations

import json
import math
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.hook_parity_contract import validate_hook_response

ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tools" / "q2-hook-oracle"


def invoke(requests: list[dict]) -> list[dict]:
    payload = "".join(json.dumps(request, separators=(",", ":")) + "\n" for request in requests)
    result = subprocess.run(
        [str(ORACLE)], input=payload, text=True, cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise AssertionError(f"oracle exited {result.returncode}: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines()]


class HookOracleTests(unittest.TestCase):
    def test_machine_readable_schema(self) -> None:
        request_schema = json.loads(
            (ROOT / "tools" / "schemas" / "q2-hook-oracle-v1.schema.json").read_text()
        )
        response_schema = json.loads(
            (ROOT / "tools" / "schemas" / "q2-hook-oracle-v1.response.schema.json").read_text()
        )
        self.assertEqual(request_schema["$id"], "urn:q2-ml:q2-hook-oracle-v1")
        self.assertFalse(request_schema["additionalProperties"])
        self.assertEqual(
            response_schema["$id"], "urn:q2-ml:q2-hook-oracle-v1:response"
        )
        self.assertIn("oneOf", response_schema)

    def test_launch_and_spawn_backoff_golden(self) -> None:
        launch, backoff = invoke([
            {"id": "launch", "op": "launch", "forward": [0.6, 0, 0.8]},
            {"id": "backoff", "op": "backoff", "hook_origin": [24, 8, 14],
             "forward": [1, 0, 0]},
        ])
        validate_hook_response(launch, expected_op="launch")
        validate_hook_response(backoff, expected_op="backoff")
        self.assertEqual(launch["velocity"], [540, 0, 720])
        self.assertEqual(backoff["hook_origin"], [14, 8, 14])

    def test_pull_is_full_velocity_overwrite(self) -> None:
        base = {
            "op": "pull", "owner_origin": [10, -20, 30],
            "hook_origin": [110, 30, 80], "hook_pullspeed": 700,
        }
        first, second = invoke([
            {**base, "id": "a", "prior_velocity": [1000, -4, 22]},
            {**base, "id": "b", "prior_velocity": [-1, 2, -900]},
        ])
        validate_hook_response(first, expected_op="pull")
        validate_hook_response(second, expected_op="pull")
        self.assertEqual(first["velocity"], second["velocity"])
        self.assertTrue(first["full_velocity_overwrite"])
        self.assertAlmostEqual(first["distance"], math.sqrt(15000), places=4)
        self.assertAlmostEqual(sum(value * value for value in first["velocity"]), 700 * 700, delta=0.1)

    def test_client_target_origin_replaces_hook_origin(self) -> None:
        response = invoke([{
            "id": "client", "op": "pull", "owner_origin": [0, 0, 0],
            "hook_origin": [100, 0, 0], "enemy_origin": [0, 50, 0],
            "enemy_is_client": True,
        }])[0]
        self.assertEqual(response["target_source"], "enemy_origin")
        self.assertEqual(response["velocity"], [0, 700, 0])

    def test_touch_classification_order_and_sky_policy(self) -> None:
        responses = invoke([
            {"id": "owner", "op": "touch", "target_is_owner": True},
            {"id": "owner-invalid", "op": "touch", "owner_has_client": False},
            {"id": "trigger", "op": "touch", "owner_has_client": True,
             "target_is_nonblocking": True},
            {"id": "sky", "op": "touch", "owner_has_client": True,
             "surface_is_sky": True},
            {"id": "sky-enabled", "op": "touch", "owner_has_client": True,
             "surface_is_sky": True, "hook_sky": True},
            {"id": "solid", "op": "touch", "owner_has_client": True},
        ])
        self.assertEqual([item["action"] for item in responses], [
            "ignore_owner", "ignore_invalid_owner", "ignore_nonblocking",
            "reset_sky", "attach", "attach",
        ])
        self.assertEqual([item["attached"] for item in responses], [False, False, False, False, True, True])

    def test_identity_binds_sources_build_and_parameters(self) -> None:
        default, changed = invoke([
            {"id": "default", "op": "identity"},
            {"id": "changed", "op": "identity", "hook_pullspeed": 1700},
        ])
        validate_hook_response(default, expected_op="identity")
        validate_hook_response(changed, expected_op="identity")
        self.assertEqual(len(default["physics_identity"]), 64)
        self.assertNotEqual(default["physics_identity"], changed["physics_identity"])
        self.assertEqual(default["tool_identity"], changed["tool_identity"])
        self.assertEqual(
            default["tool_identity"], default["source"]["tool_closure_sha256"]
        )
        for key in ("shared_c_sha256", "shared_h_sha256", "integration_sha256", "math_sha256"):
            self.assertEqual(len(default["source"][key]), 64)

    def test_tool_closure_generation_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hook-identity-") as directory:
            first = Path(directory) / "first.h"
            second = Path(directory) / "second.h"
            for output in (first, second):
                subprocess.run(
                    ["python3", "tools/gen_hook_identity.py", ".", str(output)],
                    cwd=ROOT,
                    check=True,
                )
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertIn(b"Q2_HOOK_TOOL_CLOSURE_SHA256", first.read_bytes())

    def test_zero_distance_is_exact_zero(self) -> None:
        response = invoke([{
            "id": "zero", "op": "pull", "owner_origin": [1, 2, 3],
            "hook_origin": [1, 2, 3], "prior_velocity": [9, 9, 9],
        }])[0]
        self.assertEqual(response["distance"], 0)
        self.assertEqual(response["velocity"], [0, 0, 0])


if __name__ == "__main__":
    unittest.main()
