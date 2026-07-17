#ifndef ML_QUALIFICATION_DRAIN_H
#define ML_QUALIFICATION_DRAIN_H

#define ML_DRAIN_SIGKILL_HOLD_MS 3000

/* The hold exists only for the sealed qualification fault.  Production,
 * ordinary epoch-drain, and any partially enabled test configuration remain
 * at zero. */
static inline int
ML_QualificationDrainHoldMs(int barrier_enabled, int test_mode,
	int drain_sigkill)
{
	return barrier_enabled && test_mode && drain_sigkill
		? ML_DRAIN_SIGKILL_HOLD_MS : 0;
}

#endif
