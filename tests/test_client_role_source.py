from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_H = (ROOT / "ml_bridge.h").read_text(encoding="utf-8")
WIRE_H = (ROOT / "ml_client_wire.h").read_text(encoding="utf-8")
BRIDGE_C = (ROOT / "ml_bridge.c").read_text(encoding="utf-8")
ROLE_H = (ROOT / "ml_client_role.h").read_text(encoding="utf-8")
TELEMETRY_C = (ROOT / "ml_client_telemetry.c").read_text(encoding="utf-8")


def test_role_fact_has_a_new_one_way_wire_generation():
    assert "#define ML_CLIENT_WIRE_VERSION 8u" in WIRE_H
    assert "#define ML_CAUSAL_VERSION 2u" in BRIDGE_H
    assert "ML_CAUSAL_ROLE_PLAYING             (1u << 20)" in BRIDGE_H
    assert "ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL    (1u << 21)" in BRIDGE_H
    assert "ML_CAUSAL_FLAGS_MASK              ((1u << 22) - 1u)" in BRIDGE_H


def test_positive_role_requires_ordinary_network_player_state():
    for proof in (
        "valid_client_slot",
        "!inprocess_ml_bot",
        "!monster_slot",
        "lithium_flags & LITHIUM_PLAYING",
        "!(lithium_flags & LITHIUM_OBSERVER)",
        "movetype != MOVETYPE_NOCLIP",
        "!has_chase_target",
        "!pers_spectator",
        "!resp_spectator",
    ):
        assert proof in ROLE_H
    assert "pm_type == PM_NORMAL && solid != SOLID_NOT" in ROLE_H
    assert "MoveClientToIntermission" in ROLE_H


def test_network_causal_admission_requires_role_and_normal_for_trainability():
    pack = BRIDGE_C.split("void ML_PackCausalTelemetry", 1)[1]
    assert "ML_ClientRoleCausalFlags(ent)" in pack
    assert "!(role_flags & ML_CAUSAL_ROLE_PLAYING)" in pack
    assert "role_flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL" in pack
    assert pack.index("ML_ClientRoleCausalFlags(ent)") < pack.index(
        "ML_CAUSAL_FACTS_COMPLETE"
    )
    assert "role_playing=%d role_public_pm_normal=%d pm_type=%d" in TELEMETRY_C


def test_routed_role_is_bound_to_slot_client_id_and_life_identity():
    for proof in (
        "if (!route->active)",
        "ML_ClientIdEqual(current_id, route->client_id)",
        "packet.client_slot = (uint32_t)slot",
        "packet.client_id, route->client_id",
    ):
        assert proof in TELEMETRY_C
    assert "causal->client_life_epoch = ML_ClientLifeEpoch(ent)" in BRIDGE_C
    assert "causal->client_life_epoch != 0" in BRIDGE_C
