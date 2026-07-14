from __future__ import annotations

from copy import deepcopy
import json
import subprocess
import unittest
from pathlib import Path

from tools.hook_parity_contract import (
    ContractError,
    build_attestation,
    canonical_bytes,
    source_closure_digest,
    validate_attestation,
    validate_hook_response,
)


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tools" / "q2-hook-oracle"
ATTESTOR_FILES = (
    "tests/run_q2ded_hook_parity.py",
    "tools/hook_parity_contract.py",
    "tools/schemas/q2-hook-oracle-v1.response.schema.json",
    "tools/schemas/q2-hook-parity-attestation-v1.schema.json",
)
FIXTURE_SHA = "11" * 32


def hook_identity(pullspeed: float = 1700.0) -> dict:
    request = {
        "id": "identity",
        "op": "identity",
        "hook_speed": 900,
        "hook_pullspeed": pullspeed,
        "hook_sky": False,
        "hook_maxtime": 5,
    }
    completed = subprocess.run(
        [str(ORACLE)],
        input=json.dumps(request) + "\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return validate_hook_response(json.loads(completed.stdout), expected_op="identity")


def fixture_attestation() -> tuple[dict, dict, dict]:
    parameters = {
        "hook_speed": 900,
        "hook_pullspeed": 1700,
        "hook_sky": False,
        "hook_maxtime": 5,
        "full_velocity_overwrite": True,
    }
    identity = hook_identity()
    identities = {
        "hook": {
            "schema": identity["schema"],
            "physics_identity": identity["physics_identity"],
            "tool_identity": identity["tool_identity"],
            "source": identity["source"],
        },
        "collision": {
            "schema": "q2-cm-oracle-v1",
            "physics_identity": "22" * 32,
            "map_sha256": FIXTURE_SHA,
        },
        "pmove": {
            "schema": "q2-pmove-oracle-v1",
            "physics_identity": "33" * 32,
            "map_sha256": FIXTURE_SHA,
        },
    }
    binaries = {
        "q2ded_sha256": "44" * 32,
        "probe_game_module_sha256": "55" * 32,
        "hook_oracle_sha256": "66" * 32,
        "cm_oracle_sha256": "77" * 32,
        "pmove_oracle_sha256": "88" * 32,
    }
    evidence = {
        "server_samples": [{"id": "pull", "velocity": [0, 0, -1700]}],
        "oracle_results": [{"id": "pull", "velocity": [0, 0, -1700]}],
    }
    attestation = build_attestation(
        parameters=parameters,
        fixture={
            "name": "hookprobe-v1",
            "bsp_sha256": FIXTURE_SHA,
            "bsp_bytes": 512,
            "ibsp_version": 38,
        },
        identities=identities,
        binaries=binaries,
        evidence=evidence,
        checks={"hook_vector_parity": True, "pmove_landing_parity": True},
        attestor_closure_sha256=source_closure_digest(ROOT, ATTESTOR_FILES),
    )
    return attestation, evidence, parameters


class HookParityContractTests(unittest.TestCase):
    def test_canonical_1700_attestation_round_trips(self) -> None:
        attestation, evidence, parameters = fixture_attestation()

        restored = json.loads(canonical_bytes(attestation))
        validate_attestation(
            restored, expected_parameters=parameters, evidence=evidence
        )
        self.assertTrue(restored["passed"])
        self.assertEqual(restored["parameters"]["hook_pullspeed"], 1700)

    def test_pullspeed_and_vector_mutations_are_rejected(self) -> None:
        attestation, evidence, parameters = fixture_attestation()
        changed_parameter = deepcopy(attestation)
        changed_parameter["parameters"]["hook_pullspeed"] = 1701
        with self.assertRaisesRegex(ContractError, "parameter block|physics identity"):
            validate_attestation(
                changed_parameter,
                expected_parameters=parameters,
                evidence=evidence,
            )

        changed_evidence = deepcopy(evidence)
        changed_evidence["oracle_results"][0]["velocity"][2] = -1699
        with self.assertRaisesRegex(ContractError, "vector/results"):
            validate_attestation(
                attestation,
                expected_parameters=parameters,
                evidence=changed_evidence,
            )

    def test_response_and_attestation_reject_unknown_fields(self) -> None:
        response = hook_identity()
        response["unexpected"] = True
        with self.assertRaisesRegex(ContractError, "extra"):
            validate_hook_response(response)

        attestation, evidence, parameters = fixture_attestation()
        attestation["unexpected"] = True
        with self.assertRaisesRegex(ContractError, "extra"):
            validate_attestation(
                attestation, expected_parameters=parameters, evidence=evidence
            )

    def test_attestation_schema_is_strict(self) -> None:
        schema = json.loads(
            (
                ROOT
                / "tools/schemas/q2-hook-parity-attestation-v1.schema.json"
            ).read_text()
        )
        self.assertEqual(
            schema["$id"], "urn:q2-ml:q2-hook-parity-attestation-v1"
        )
        self.assertFalse(schema["additionalProperties"])


if __name__ == "__main__":
    unittest.main()
