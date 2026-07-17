#include "../ml_client_lifecycle.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    zgcl_t before;
    zgcl_t after;
    ml_client_lifecycle_echo_t echo;

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    memset(&echo, 0, sizeof(echo));

    before.ml_move_forward = 0.75f;
    before.ml_move_right = -0.25f;
    before.ml_look_yaw = 12.5f;
    before.ml_look_pitch = -3.0f;
    before.ml_vertical_intent = ML_VERTICAL_UP_OR_JUMP;
    before.ml_applied_upmove = 320;
    before.ml_fire = 0;
    before.ml_fire_suppressed = 1;
    before.ml_hook = 3;
    before.ml_weapon = 7;
    before.ml_action_generation = 0; /* valid modulo generation, wire value 1 */
    before.ml_action_generation_valid = 1;
    before.ml_last_action_tick = 192;
    before.ml_last_action_ok = 1;

    assert(!ML_ClientLifecycleCaptureEcho(
        &echo, &before, qfalse, qtrue));
    assert(!ML_ClientLifecycleCaptureEcho(
        &echo, &before, qtrue, qfalse));
    assert(ML_ClientLifecycleCaptureEcho(
        &echo, &before, qtrue, qtrue));
    assert((unsigned int)echo.action_generation + 1u == 1u);

    /* The new life starts cleared.  Restore only exact command attribution;
       unrelated prior-life state must stay cleared. */
    ML_ClientLifecycleRestoreEcho(&after, &echo);
    assert(after.ml_move_forward == before.ml_move_forward);
    assert(after.ml_move_right == before.ml_move_right);
    assert(after.ml_look_yaw == before.ml_look_yaw);
    assert(after.ml_look_pitch == before.ml_look_pitch);
    assert(after.ml_vertical_intent == before.ml_vertical_intent);
    assert(after.ml_applied_upmove == before.ml_applied_upmove);
    assert(after.ml_fire == before.ml_fire);
    assert(after.ml_fire_suppressed == before.ml_fire_suppressed);
    assert(after.ml_hook == before.ml_hook);
    assert(after.ml_weapon == before.ml_weapon);
    assert(after.ml_action_generation == 0);
    assert(after.ml_action_generation_valid == 1);
    assert(after.ml_last_action_tick == 192);
    assert(after.ml_last_action_ok == 1);
    assert(after.ml_reward_death == 0.0f);
    assert(after.ml_causal_target_edict == 0);
    assert(after.ml_death_obs_sent == 0);

    before.ml_action_generation = ML_ACTION_GENERATION_COUNT;
    assert(!ML_ClientLifecycleCaptureEcho(
        &echo, &before, qtrue, qtrue));
    before.ml_action_generation = 1;
    before.ml_last_action_tick = 0;
    assert(!ML_ClientLifecycleCaptureEcho(
        &echo, &before, qtrue, qtrue));
    before.ml_last_action_tick = 193;
    before.ml_vertical_intent = -1;
    assert(!ML_ClientLifecycleCaptureEcho(
        &echo, &before, qtrue, qtrue));
    return 0;
}
