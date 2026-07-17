#include "../ml_qualification_drain.h"

#include <assert.h>

int main(void)
{
	assert(ML_DRAIN_SIGKILL_HOLD_MS == 3000);
	assert(ML_DRAIN_SIGKILL_HOLD_MS > 1500);
	assert(ML_QualificationDrainHoldMs(1, 1, 0) == 0);
	assert(ML_QualificationDrainHoldMs(1, 1, 1) == 3000);
	assert(ML_QualificationDrainHoldMs(0, 1, 1) == 0);
	assert(ML_QualificationDrainHoldMs(1, 0, 1) == 0);
	assert(ML_QualificationDrainHoldMs(0, 0, 1) == 0);
	return 0;
}
