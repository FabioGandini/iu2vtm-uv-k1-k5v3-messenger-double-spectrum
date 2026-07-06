/* Si4732-A10 receiver driver (AM / FM / SSB with patch).
 *
 * Ported from fagci/k1-fw (si473x.c/h, LGPL-compatible fagci reborn lineage)
 * onto the UV-K1 armel/DualTachyon driver layer (bit-banged I2C shared with
 * the BK1080 footprint the IOTCU kit replaces).
 *
 * Differences from the fagci original:
 *  - waitToSend() is bounded: an absent/mute chip can no longer hang the
 *    firmware, every command wrapper just becomes a no-op.
 *  - The SSB patch is compiled into MCU flash in the PU2CLR compressed
 *    format (driver/si4732-patch.h) and pushed by SI47XX_PatchPowerUp();
 *    the fagci EEPROM download was dead code.
 *  - No audio-path or UI calls in the driver; the FM app owns those.
 */

#ifndef DRIVER_SI473X_H
#define DRIVER_SI473X_H

#include <stdbool.h>
#include <stdint.h>

/* Frequency limits in 10 Hz units */
#define SI47XX_F_MIN 15000       /* 150 kHz  */
#define SI47XX_F_MAX 3000000     /* 30 MHz   */
#define SI47XX_FM_F_MIN 6400000  /* 64 MHz   */
#define SI47XX_FM_F_MAX 10800000 /* 108 MHz  */

typedef enum {
    SI47XX_FM,
    SI47XX_AM,
    SI47XX_LSB,
    SI47XX_USB,
} SI47XX_MODE;

typedef enum {
    SI47XX_BW_6_kHz,
    SI47XX_BW_4_kHz,
    SI47XX_BW_3_kHz,
    SI47XX_BW_2_kHz,
    SI47XX_BW_1_kHz,
    SI47XX_BW_1_8_kHz,
    SI47XX_BW_2_5_kHz,
} SI47XX_FilterBW;

typedef enum {
    SI47XX_SSB_BW_1_2_kHz,
    SI47XX_SSB_BW_2_2_kHz,
    SI47XX_SSB_BW_3_kHz,
    SI47XX_SSB_BW_4_kHz,
    SI47XX_SSB_BW_0_5_kHz,
    SI47XX_SSB_BW_1_0_kHz,
} SI47XX_SsbFilterBW;

typedef union {
    struct {
        /* status ("RESP0") */
        uint8_t STCINT : 1;
        uint8_t DUMMY1 : 1;
        uint8_t RDSINT : 1;
        uint8_t RSQINT : 1;
        uint8_t DUMMY2 : 2;
        uint8_t ERR : 1;
        uint8_t CTS : 1;
        /* RESP1 */
        uint8_t RSSIILINT : 1;
        uint8_t RSSIHINT : 1;
        uint8_t SNRLINT : 1;
        uint8_t SNRHINT : 1;
        uint8_t MULTLINT : 1;
        uint8_t MULTHINT : 1;
        uint8_t DUMMY3 : 1;
        uint8_t BLENDINT : 1;
        /* RESP2 */
        uint8_t VALID : 1;
        uint8_t AFCRL : 1;
        uint8_t DUMMY4 : 1;
        uint8_t SMUTE : 1;
        uint8_t DUMMY5 : 4;
        /* RESP3 */
        uint8_t STBLEND : 7;
        uint8_t PILOT : 1;
        /* RESP4..RESP7 */
        uint8_t RSSI;    /* dBuV, 0..127 */
        uint8_t SNR;     /* dB, 0..127 */
        uint8_t MULT;    /* FM multipath */
        uint8_t FREQOFF; /* signed kHz offset */
    } resp;
    uint8_t raw[8];
} RSQStatus;

void SI47XX_PowerOn(SI47XX_MODE mode); /* power up / switch, any state */
void SI47XX_SetMute(bool mute);
void SI47XX_PowerUp(void);      /* power up in si4732mode (FM or AM) */
void SI47XX_PatchPowerUp(void); /* power up + SSB patch download (~2 s) */
void SI47XX_PowerDown(void);
void SI47XX_SwitchMode(SI47XX_MODE mode);
bool SI47XX_IsSSB(void);

/* f in 10 Hz units; SSB uses the BFO for sub-kHz offsets */
void SI47XX_TuneTo(uint32_t f);
/* returns the tuned frequency in 10 Hz units */
uint32_t SI47XX_getFrequency(bool *valid);

void SI47XX_SetVolume(uint8_t volume); /* 0..63 */
void SI47XX_SetBandwidth(SI47XX_FilterBW AMCHFLT, bool AMPLFLT);
void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw);
void SI47XX_SetBFO(int16_t bfo);
void SI47XX_SetProperty(uint16_t prop, uint16_t value);
void SI47XX_SetAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX);
void SI47XX_Seek(bool up, bool wrap);
void RSQ_GET(void);

extern SI47XX_MODE si4732mode;
extern RSQStatus rsqStatus;
extern uint16_t siCurrentFreq; /* chip units: 10 kHz in FM, 1 kHz otherwise */
extern bool isSi4732On;

#endif
