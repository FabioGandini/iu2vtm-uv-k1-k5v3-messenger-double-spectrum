/* See si473x.h for provenance. */

#include "driver/si473x.h"
#include "driver/i2c.h"
#include "driver/system.h"
#include "driver/systick.h"
#include "driver/si4732-patch.h"
#include "misc.h"

/* 8-bit I2C write address (SEN pulled low on the IOTCU board: 0x11 << 1) */
static const uint8_t SI47XX_I2C_ADDR = 0x22;

enum {
    CMD_POWER_UP = 0x01,
    CMD_POWER_DOWN = 0x11,
    CMD_SET_PROPERTY = 0x12,
    CMD_FM_TUNE_FREQ = 0x20,
    CMD_FM_SEEK_START = 0x21,
    CMD_FM_TUNE_STATUS = 0x22,
    CMD_FM_RSQ_STATUS = 0x23,
    CMD_FM_AGC_OVERRIDE = 0x28,
    CMD_AM_TUNE_FREQ = 0x40,
    CMD_AM_SEEK_START = 0x41,
    CMD_AM_TUNE_STATUS = 0x42,
    CMD_AM_RSQ_STATUS = 0x43,
    CMD_AM_AGC_OVERRIDE = 0x48,
};

enum {
    FLG_XOSCEN = 0x10,
    FLG_PATCH = 0x20,
    FUNC_FM = 0x00,
    FUNC_AM = 0x01,
    OUT_ANALOG = 0x05,
    FLG_SEEKUP = 0x08,
    FLG_WRAP = 0x04,
    STATUS_CTS = 0x80,
    STATUS_VALID = 0x01,
};

enum {
    PROP_SSB_BFO = 0x0100,
    PROP_SSB_MODE = 0x0101,
    PROP_FM_SEEK_BAND_BOTTOM = 0x1400,
    PROP_FM_SEEK_BAND_TOP = 0x1401,
    PROP_FM_SEEK_FREQ_SPACING = 0x1402,
    PROP_AM_CHANNEL_FILTER = 0x3102,
    PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN = 0x3103,
    PROP_AM_SOFT_MUTE_MAX_ATTENUATION = 0x3302,
    PROP_SSB_SOFT_MUTE_MAX_ATTENUATION = 0x3302,
    PROP_AM_SEEK_BAND_BOTTOM = 0x3400,
    PROP_AM_SEEK_BAND_TOP = 0x3401,
    PROP_AM_SEEK_FREQ_SPACING = 0x3402,
    PROP_AM_AGC_RELEASE_RATE = 0x3703,
    PROP_RX_VOLUME = 0x4000,
    PROP_RX_HARD_MUTE = 0x4001,
};

RSQStatus rsqStatus;

SI47XX_MODE si4732mode = SI47XX_FM;
uint16_t siCurrentFreq = 0;
bool isSi4732On = false;

static uint16_t fDiv(void) { return si4732mode == SI47XX_FM ? 1000 : 100; }

static bool SI47XX_ReadBuffer(uint8_t *buf, uint8_t size)
{
    uint8_t retries = 5;
    while (retries--) {
        I2C_Start();
        if (I2C_Write(SI47XX_I2C_ADDR + 1) == 0) {
            if (I2C_ReadBuffer(buf, size) == 0) {
                I2C_Stop();
                return true;
            }
        }
        I2C_Stop();
        SYSTICK_DelayUs(10);
    }
    return false;
}

static bool SI47XX_WriteBuffer(const uint8_t *buf, uint8_t size)
{
    I2C_Start();
    if (I2C_Write(SI47XX_I2C_ADDR) != 0) {
        I2C_Stop();
        return false;
    }
    if (I2C_WriteBuffer(buf, size) != 0) {
        I2C_Stop();
        return false;
    }
    I2C_Stop();
    return true;
}

bool SI47XX_IsSSB(void)
{
    return si4732mode == SI47XX_USB || si4732mode == SI47XX_LSB;
}

/* Bounded CTS wait: worst observed latency is the STC of an AM tune
 * (~80 ms); 200 ms covers it. Returns false when the chip is absent or
 * wedged so callers degrade to no-ops instead of hanging the firmware. */
static bool waitToSend(void)
{
    for (uint16_t i = 0; i < 10000; i++) {
        uint8_t tmp = 0;
        if (!SI47XX_ReadBuffer(&tmp, 1))
            return false; // no I2C ACK: chip absent (stock board) or bus stuck
        if (tmp & STATUS_CTS)
            return true;
        SYSTICK_DelayUs(20);
    }
    return false;
}

void SI47XX_SetProperty(uint16_t prop, uint16_t value)
{
    if (!waitToSend())
        return;
    uint8_t tmp[6] = {
        CMD_SET_PROPERTY, 0,
        prop >> 8, prop & 0xff,
        value >> 8, value & 0xff,
    };
    SI47XX_WriteBuffer(tmp, 6);
    SYSTEM_DelayMs(8); // SET_PROPERTY completes in 10 ms wall time, no CTS
}

void RSQ_GET(void)
{
    uint8_t cmd[2] = {CMD_FM_RSQ_STATUS, 0x01};
    if (si4732mode != SI47XX_FM)
        cmd[0] = CMD_AM_RSQ_STATUS;

    if (!waitToSend())
        return;
    SI47XX_WriteBuffer(cmd, 2);
    SI47XX_ReadBuffer(rsqStatus.raw, si4732mode == SI47XX_FM ? 8 : 6);
}

void SI47XX_SetVolume(uint8_t volume)
{
    if (volume > 63)
        volume = 63;
    SI47XX_SetProperty(PROP_RX_VOLUME, volume);
}

static void setAvcAmMaxGain(uint8_t gain)
{
    SI47XX_SetProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, gain * 340);
}

void SI47XX_SetAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX)
{
    uint8_t cmd = (si4732mode == SI47XX_FM) ? CMD_FM_AGC_OVERRIDE
                                            : CMD_AM_AGC_OVERRIDE; // AM + SSB
    if (!waitToSend())
        return;
    uint8_t buf[3] = {cmd, (uint8_t)(AGCDIS & 1), AGCIDX};
    SI47XX_WriteBuffer(buf, 3);
}

static void SI47XX_SetFreq(uint16_t freq)
{
    if (freq == 0 || siCurrentFreq == freq)
        return;

    uint8_t size = 5;
    uint8_t cmd[6] = {CMD_FM_TUNE_FREQ, 0x00,
                      (uint8_t)(freq >> 8), (uint8_t)(freq & 0xFF), 0, 0};

    if (si4732mode == SI47XX_FM || si4732mode == SI47XX_AM) {
        cmd[1] = 0x01; // FAST
    } else {
        cmd[1] = si4732mode == SI47XX_USB ? 0b10000000 : 0b01000000;
    }

    if (si4732mode != SI47XX_FM) {
        cmd[0] = CMD_AM_TUNE_FREQ;
        size = 6;
    }

    // shortwave: let the chip pick the antenna capacitor
    if (si4732mode == SI47XX_AM && freq > 1800)
        cmd[5] = 1;

    if (!waitToSend())
        return;
    SI47XX_WriteBuffer(cmd, size);
    siCurrentFreq = freq;
}

void SI47XX_PowerUp(void)
{
    uint8_t cmd[3] = {CMD_POWER_UP, FLG_XOSCEN | FUNC_FM, OUT_ANALOG};
    if (si4732mode != SI47XX_FM)
        cmd[1] = FLG_XOSCEN | FUNC_AM;

    waitToSend();
    if (!SI47XX_WriteBuffer(cmd, 3))
        return;
    SYSTEM_DelayMs(500); // 32.768 kHz crystal start-up

    isSi4732On = true;

    SI47XX_SetVolume(63);

    if (si4732mode == SI47XX_AM) {
        SI47XX_SetAutomaticGainControl(1, 0);
        SI47XX_SetProperty(PROP_AM_SOFT_MUTE_MAX_ATTENUATION, 0);
        SI47XX_SetProperty(PROP_AM_AGC_RELEASE_RATE, 20);
        setAvcAmMaxGain(40);
    }

    if (siCurrentFreq != 0) {
        uint16_t f = siCurrentFreq;
        siCurrentFreq = 0; // force retune after the power cycle
        SI47XX_SetFreq(f);
    }
}

typedef union {
    struct {
        uint8_t AUDIOBW : 4;
        uint8_t SBCUTFLT : 4;
        uint8_t AVC_DIVIDER : 4;
        uint8_t AVCEN : 1;
        uint8_t SMUTESEL : 1;
        uint8_t DUMMY1 : 1;
        uint8_t DSP_AFCDIS : 1;
    } param;
    uint8_t raw[2];
} SsbMode;

static void SI47XX_SsbSetup(SI47XX_SsbFilterBW AUDIOBW, uint8_t SBCUTFLT,
                            uint8_t AVC_DIVIDER, uint8_t AVCEN,
                            uint8_t SMUTESEL, uint8_t DSP_AFCDIS)
{
    SsbMode m = {0};
    m.param.AUDIOBW = AUDIOBW;
    m.param.SBCUTFLT = SBCUTFLT;
    m.param.AVC_DIVIDER = AVC_DIVIDER;
    m.param.AVCEN = AVCEN;
    m.param.SMUTESEL = SMUTESEL;
    m.param.DSP_AFCDIS = DSP_AFCDIS;
    SI47XX_SetProperty(PROP_SSB_MODE, (m.raw[1] << 8) | m.raw[0]);
}

/* Push the compressed SSB patch (si4732-patch.h): 7 payload bytes per
 * record, command byte 0x16 unless the record index is in cmd_0x15[]. */
static void SI47XX_downloadPatch(void)
{
    uint16_t idx0x15 = 0;

    for (uint16_t line = 0, offset = 0; offset < sizeof(ssb_patch_content);
         line++, offset += 7) {
        uint8_t rec[8];

        rec[0] = 0x16;
        if (idx0x15 < ARRAY_SIZE(cmd_0x15) && cmd_0x15[idx0x15] == line) {
            rec[0] = 0x15;
            idx0x15++;
        }
        for (uint8_t i = 0; i < 7; i++)
            rec[i + 1] = ssb_patch_content[offset + i];

        if (!waitToSend())
            return;
        SI47XX_WriteBuffer(rec, 8);
    }
}

void SI47XX_PatchPowerUp(void)
{
    uint8_t cmd[3] = {CMD_POWER_UP, FLG_PATCH | FLG_XOSCEN | FUNC_AM,
                      OUT_ANALOG};

    waitToSend();
    if (!SI47XX_WriteBuffer(cmd, 3))
        return;
    SYSTEM_DelayMs(500); // crystal start-up, same as the plain power-up

    isSi4732On = true;

    SI47XX_downloadPatch();

    SI47XX_SsbSetup(SI47XX_SSB_BW_3_kHz, 1, 0, 1, 0, 1);
    setAvcAmMaxGain(42);

    SI47XX_SetVolume(63);

    if (siCurrentFreq != 0) {
        uint16_t f = siCurrentFreq;
        siCurrentFreq = 0;
        SI47XX_SetFreq(f);
    }
    SI47XX_SetProperty(PROP_SSB_SOFT_MUTE_MAX_ATTENUATION, 0);
    SI47XX_SetProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, 0x7800);
}

void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw)
{
    SI47XX_SsbSetup(bw, 1, 0, 1, 0, 1);
}

void SI47XX_Seek(bool up, bool wrap)
{
    uint8_t seekOpt = (up ? FLG_SEEKUP : 0) | (wrap ? FLG_WRAP : 0);
    uint8_t cmd[6] = {CMD_FM_SEEK_START, seekOpt, 0x00, 0x00, 0x00, 0x00};

    if (si4732mode == SI47XX_AM) {
        cmd[0] = CMD_AM_SEEK_START;
        cmd[5] = (siCurrentFreq > 1800) ? 1 : 0;
    }

    if (!waitToSend())
        return;
    SI47XX_WriteBuffer(cmd, si4732mode == SI47XX_FM ? 2 : 6);
}

uint32_t SI47XX_getFrequency(bool *valid)
{
    uint8_t response[4] = {0};
    uint8_t cmd[2] = {CMD_FM_TUNE_STATUS, 0x00};

    if (si4732mode != SI47XX_FM)
        cmd[0] = CMD_AM_TUNE_STATUS;

    if (!waitToSend()) {
        if (valid)
            *valid = false;
        return 0;
    }
    SI47XX_WriteBuffer(cmd, 2);
    SI47XX_ReadBuffer(response, 4);

    if (valid)
        *valid = (response[1] & STATUS_VALID);

    return (uint32_t)((response[2] << 8) | response[3]) * fDiv();
}

/* Unconditional (not gated on isSi4732On): also used at boot to silence a
 * chip left running across a warm MCU reset. Fails fast when no chip ACKs. */
void SI47XX_PowerDown(void)
{
    uint8_t cmd[1] = {CMD_POWER_DOWN};

    waitToSend();
    SI47XX_WriteBuffer(cmd, 1);
    SYSTICK_DelayUs(10);
    isSi4732On = false;
    siCurrentFreq = 0;
}

void SI47XX_SwitchMode(SI47XX_MODE mode)
{
    if (si4732mode == mode)
        return;

    bool wasSSB = SI47XX_IsSSB();
    si4732mode = mode;

    if (SI47XX_IsSSB()) {
        if (!wasSSB) {
            SI47XX_PowerDown();
            SI47XX_PatchPowerUp();
        }
        // USB<->LSB is just a different USBLSB bit on the next tune
        siCurrentFreq = 0;
    } else {
        SI47XX_PowerDown();
        SI47XX_PowerUp();
    }
}

void SI47XX_SetBFO(int16_t bfo) { SI47XX_SetProperty(PROP_SSB_BFO, bfo); }

/* Bring the chip up in the requested mode, whatever state it is in. */
void SI47XX_PowerOn(SI47XX_MODE mode)
{
    if (isSi4732On) {
        SI47XX_SwitchMode(mode);
        return;
    }
    si4732mode = mode;
    if (SI47XX_IsSSB())
        SI47XX_PatchPowerUp();
    else
        SI47XX_PowerUp();
}

void SI47XX_SetMute(bool mute)
{
    SI47XX_SetProperty(PROP_RX_HARD_MUTE, mute ? 0x0003 : 0x0000);
}

void SI47XX_TuneTo(uint32_t f)
{
    if (SI47XX_IsSSB()) {
        // keep the chip on a whole-kHz channel and put the sub-kHz
        // remainder (and small QSY steps) on the BFO, +-16 kHz max
        int32_t bfo = ((int32_t)siCurrentFreq * 100 - (int32_t)f) * 10; // Hz
        if (siCurrentFreq != 0 && bfo > -16000 && bfo < 16000) {
            SI47XX_SetBFO((int16_t)bfo);
            return;
        }
        SI47XX_SetFreq((uint16_t)(f / 100));          // kHz channel
        SI47XX_SetBFO((int16_t)(-(f % 100) * 10));    // sub-kHz remainder
        return;
    }

    uint32_t chip = f / fDiv();
    if (si4732mode == SI47XX_FM)
        chip -= chip % 5; // FM chip step is 50 kHz
    SI47XX_SetFreq((uint16_t)chip);
}

void SI47XX_SetBandwidth(SI47XX_FilterBW AMCHFLT, bool AMPLFLT)
{
    uint16_t v = ((uint16_t)(AMPLFLT ? 1 : 0) << 8) | (AMCHFLT & 0x0F);
    SI47XX_SetProperty(PROP_AM_CHANNEL_FILTER, v);
}
