/* IU2VTM: one-time migration of the key-action IDs stored in EEPROM.
 *
 * Upstream F4HWN v6 made the ACTION_OPT_ IDs fixed (they are persisted, see
 * enum ACTION_OPT_t in settings.h) but did not remap values written by older
 * firmware. Older IU2VTM builds numbered the actions by position in a
 * build-dependent enum, so after the update every programmed key (side keys
 * and M long press) pointed to a different action: the messenger became
 * REMOVE_OFFSET, FM became the 1750 Hz tone, RX MODE became PTT, ...
 *
 * The config is flagged with a two byte marker in the (otherwise unused)
 * bytes 0-1 of the 0xA158 block, which older firmware filled with the build
 * options bitmap. No marker + programmed keys = legacy numbering: remap once.
 * Erased keys (fresh radio / after a reset) just get the marker.
 *
 * Pure logic, no flash access, so it can be unit tested on the host.
 */
#ifndef HELPER_ACTION_MIGRATE_H
#define HELPER_ACTION_MIGRATE_H

#include <stdint.h>
#include <stdbool.h>

/* 'V','6' = 0x56,0x36. As a legacy build-options bitmap this would mean
 * NOAA+VOICE+ALARM+PWRON_PASSWORD, which no IU2VTM build ever enabled. */
#define ACTION_MIGRATE_MARK0   0x56
#define ACTION_MIGRATE_MARK1   0x36

#define ACTION_MIGRATE_INVALID 0xFF
#define ACTION_MIGRATE_LEGACY_COUNT 23

/* Legacy ACTION_OPT_ position (IU2VTM build: F4HWN on, BEAM and RESCUE_OPS
 * off, MESSENGER on, RXTX_LOG optional) -> fixed v6 ID.
 * Actions that no longer exist upstream become NONE (0). */
static const uint8_t k_legacyActionToV6[ACTION_MIGRATE_LEGACY_COUNT] = {
    0,   /*  0 NONE            -> NONE            */
    1,   /*  1 FLASHLIGHT      -> FLASHLIGHT      */
    2,   /*  2 POWER           -> POWER           */
    3,   /*  3 MONITOR         -> MONITOR         */
    4,   /*  4 SCAN            -> SCAN            */
    5,   /*  5 VOX             -> VOX             */
    0,   /*  6 ALARM           -> (removed)       */
    6,   /*  7 FM              -> FM              */
    7,   /*  8 1750            -> 1750            */
    8,   /*  9 KEYLOCK         -> KEYLOCK         */
    9,   /* 10 A_B             -> A_B             */
    10,  /* 11 VFO_MR          -> VFO_MR          */
    11,  /* 12 SWITCH_DEMODUL  -> SWITCH_DEMODUL  */
    0,   /* 13 BLMIN_TMP_OFF   -> (removed)       */
    12,  /* 14 RXMODE          -> RXMODE          */
    13,  /* 15 MAINONLY        -> MAINONLY        */
    14,  /* 16 PTT             -> PTT             */
    15,  /* 17 WN              -> WN              */
    0,   /* 18 BACKLIGHT       -> (removed)       */
    16,  /* 19 MUTE            -> MUTE            */
    17,  /* 20 RXA             -> RXA             */
    24,  /* 21 MESSENGER       -> MESSENGER       */
    18,  /* 22 RXTX_LOG        -> RXTX_LOG        */
};

/* Out-of-range input stays "invalid" so the loader applies its defaults. */
static inline uint8_t ActionMigrate_MapId(uint8_t legacyId)
{
    return (legacyId < ACTION_MIGRATE_LEGACY_COUNT)
               ? k_legacyActionToV6[legacyId]
               : ACTION_MIGRATE_INVALID;
}

/* keys[] = the first five bytes of the 0xA0A8 block:
 *   [0] bit0 BEEP_CONTROL, bits7..1 KEY_M_LONG_PRESS_ACTION
 *   [1] KEY_1_SHORT  [2] KEY_1_LONG  [3] KEY_2_SHORT  [4] KEY_2_LONG */
static inline bool ActionMigrate_KeysErased(const uint8_t keys[5])
{
    for (int i = 0; i < 5; i++)
        if (keys[i] != 0xFF)
            return false;
    return true;
}

static inline void ActionMigrate_RemapKeys(uint8_t keys[5])
{
    uint8_t m = ActionMigrate_MapId((uint8_t)(keys[0] >> 1));
    if (m == ACTION_MIGRATE_INVALID)
        m = 0x7F;                       /* field is 7 bits wide */
    keys[0] = (uint8_t)((keys[0] & 1u) | (m << 1));

    for (int i = 1; i < 5; i++)
        keys[i] = ActionMigrate_MapId(keys[i]);
}

#endif /* HELPER_ACTION_MIGRATE_H */
