from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "g_main.c").read_text(encoding="utf-8")
HOLD = (ROOT / "ml_qualification_drain.h").read_text(encoding="utf-8")


def test_injection_is_test_mode_fenced_and_faults_are_distinct():
    fenced = MAIN.split(
        'if (gi.cvar("sv_ml_frame_barrier", "0", 0)->value &&', 1
    )[1].split("/* Bot-only ML servers", 1)[0]
    assert 'gi.cvar("sv_ml_frame_barrier_test_mode", "0", 0)->value' in fenced
    assert '!Q_stricmp(fault->string, "epoch-drain")' in fenced
    assert '!Q_stricmp(fault->string, "drain-sigkill")' in fenced
    assert "ML_QualificationDrainHoldMs(1, 1," in fenced
    assert 'drain_hold_ms ? "3" : "0"' in fenced


def test_hold_is_sealed_in_process_and_evidenced():
    assert "#define ML_DRAIN_SIGKILL_HOLD_MS 3000" in HOLD
    assert "barrier_enabled && test_mode && drain_sigkill" in HOLD
    assert "static float ml_qualification_drain_hold_until" in MAIN
    assert "level.time < ml_qualification_drain_hold_until" in MAIN
    assert "level.time >= ml_qualification_drain_hold_until" in MAIN
    assert "level.exitintermission = qtrue" in MAIN
    assert "ml_qualification_drain_hold_until = 0" in MAIN
    injected = MAIN.split("event=intermission_injected", 1)[1].split(
        "ML_GateEpochDrainExit();", 1
    )[0]
    assert "drain_hold_ms=%d" in injected
    assert "drain_hold_ms" in injected
