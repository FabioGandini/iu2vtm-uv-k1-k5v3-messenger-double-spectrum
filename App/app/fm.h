/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef APP_FM_H
#define APP_FM_H

#ifdef ENABLE_FMRADIO

#include "driver/keyboard.h"
#include "misc.h"

#define FM_CHANNEL_UP   0x01
#define FM_CHANNEL_DOWN 0xFF

#ifdef ENABLE_SI4732
#include "driver/si473x.h"

/* HF band plan for the Si4732. Entry 0 is the FM-broadcast pseudo band:
 * with gHF_Band == 0 the app runs the stock FM code paths (channels, scan,
 * 100 kHz units) just with the Si4732 as tuner instead of the BK1080. */
typedef struct {
    char     name[5];
    uint32_t lo;   /* 10 Hz units */
    uint32_t hi;   /* 10 Hz units */
    uint8_t  mode; /* default SI47XX_MODE for the band */
} HF_Band_t;

extern const HF_Band_t gHF_Bands[];
extern const uint8_t   gHF_BandCount;

extern uint32_t gHF_Freq;      /* 10 Hz units */
extern uint8_t  gHF_Band;      /* index into gHF_Bands, 0 = FM broadcast */
extern uint8_t  gHF_Mode;      /* SI47XX_MODE while on an HF band */
extern uint8_t  gHF_StepIndex;
extern uint8_t  gHF_BwIndex;

#define HF_ACTIVE (gHF_Band > 0)

void HF_ApplyBand(uint8_t band);
void HF_Tune(void);
uint32_t HF_GetStep(void);
const char *HF_ModeName(void);
const char *HF_StepName(void);
const char *HF_BwName(void);
#endif

enum {
    FM_SCAN_OFF = 0U,
};

extern uint16_t          gFM_Channels[FM_CHANNELS_MAX];
extern bool              gFmRadioMode;
extern uint8_t           gFmRadioCountdown_500ms;
extern volatile uint16_t gFmPlayCountdown_10ms;
extern volatile int8_t   gFM_ScanState;
extern bool              gFM_AutoScan;
extern uint8_t           gFM_ChannelPosition;
// Doubts about          whether this should be signed or not
extern uint16_t          gFM_FrequencyDeviation;
extern bool              gFM_FoundFrequency;
extern uint16_t          gFM_RestoreCountdown_10ms;

bool    FM_CheckValidChannel(uint8_t Channel);
// returns first valid channel starting at Channel
uint8_t FM_FindNextChannel(uint8_t Channel, uint8_t Direction);
int     FM_ConfigureChannelState(void);
void    FM_SetFrequency(void);
void    FM_TurnOff(void);
void    FM_EraseChannels(void);

void    FM_Tune(uint16_t Frequency, int8_t Step, bool bFlag);
void    FM_PlayAndUpdate(void);
int     FM_CheckFrequencyLock(uint16_t Frequency, uint16_t LowerLimit);

void    FM_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

void    FM_Play(void);
void    FM_Start(void);

#endif

#endif
