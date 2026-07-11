/* rune_bits.h — shared rune bitmask constants.
   Single source of truth for edict_t.rune flags, referenced by l_rune.c
   (definition site) and the ML bridge (g_combat.c reward shaping, ml_obs.c
   observation packing). Keep in sync with the legacy #defines in l_rune.c. */
#ifndef RUNE_BITS_H
#define RUNE_BITS_H

#define RUNE_RESIST   1
#define RUNE_STRENGTH 2
#define RUNE_HASTE    4
#define RUNE_REGEN    8
#define RUNE_VAMPIRE  16

#endif /* RUNE_BITS_H */
