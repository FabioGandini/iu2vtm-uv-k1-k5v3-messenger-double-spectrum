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

#ifdef ENABLE_FMRADIO

#include <string.h>

#include "app/action.h"
#include "app/fm.h"
#include "app/generic.h"
#include "audio.h"
#include "driver/bk1080.h"
#include "driver/bk4819.h"
#include "driver/py25q16.h"
#include "driver/gpio.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

uint16_t          gFM_Channels[FM_CHANNELS_MAX];
bool              gFmRadioMode;
uint8_t           gFmRadioCountdown_500ms;
volatile uint16_t gFmPlayCountdown_10ms;
volatile int8_t   gFM_ScanState;
bool              gFM_AutoScan;
uint8_t           gFM_ChannelPosition;
bool              gFM_FoundFrequency;
uint16_t          gFM_RestoreCountdown_10ms;

#ifdef ENABLE_SI4732

/* frequencies in 10 Hz units (kHz * 100) */
const HF_Band_t gHF_Bands[] = {
    {"FM",   6400000, 10800000, SI47XX_FM},
    {"LW",     15300,    52000, SI47XX_AM},
    {"MW",     52200,   171000, SI47XX_AM},
    {"120m",  230000,   249500, SI47XX_AM},
    {"90m",   320000,   340000, SI47XX_AM},
    {"80m",   350000,   380000, SI47XX_LSB},
    {"60m",   475000,   506000, SI47XX_AM},
    {"49m",   590000,   620000, SI47XX_AM},
    {"40m",   700000,   720000, SI47XX_LSB},
    {"41m",   720000,   745000, SI47XX_AM},
    {"31m",   940000,   990000, SI47XX_AM},
    {"25m",  1160000,  1210000, SI47XX_AM},
    {"22m",  1357000,  1387000, SI47XX_AM},
    {"20m",  1400000,  1435000, SI47XX_USB},
    {"19m",  1510000,  1583000, SI47XX_AM},
    {"16m",  1748000,  1790000, SI47XX_AM},
    {"17m",  1806800,  1816800, SI47XX_USB},
    {"15m",  2100000,  2145000, SI47XX_USB},
    {"13m",  2145000,  2185000, SI47XX_AM},
    {"11m",  2567000,  2610000, SI47XX_AM},
    {"CB",   2696500,  2740500, SI47XX_AM},
    {"10m",  2800000,  2970000, SI47XX_USB},
    {"SW",     15000,  3000000, SI47XX_AM},
};
const uint8_t gHF_BandCount = ARRAY_SIZE(gHF_Bands);

uint32_t gHF_Freq      = 700000; /* 7000 kHz */
uint8_t  gHF_Band      = 0;
uint8_t  gHF_Mode      = SI47XX_AM;
uint8_t  gHF_StepIndex = 1;
uint8_t  gHF_BwIndex   = 0;

/* per-band frequency memory for this power cycle */
static uint32_t sHF_BandFreq[ARRAY_SIZE(gHF_Bands)];

static const uint16_t sHF_Steps[] = {10, 100, 500, 900, 1000}; /* 10 Hz units */

uint32_t HF_GetStep(void)
{
    return sHF_Steps[gHF_StepIndex % ARRAY_SIZE(sHF_Steps)];
}

void HF_Tune(void)
{
    SI47XX_PowerOn((SI47XX_MODE)gHF_Mode);
    SI47XX_TuneTo(gHF_Freq);
    if (HF_ACTIVE)
        sHF_BandFreq[gHF_Band] = gHF_Freq;
}

const char *HF_ModeName(void)
{
    static const char *names[4] = {"FM", "AM", "LSB", "USB"};
    return names[gHF_Mode & 3];
}

const char *HF_StepName(void)
{
    static const char *names[5] = {".1k", "1k", "5k", "9k", "10k"};
    return names[gHF_StepIndex % 5];
}

const char *HF_BwName(void)
{
    if (gHF_Mode == SI47XX_LSB || gHF_Mode == SI47XX_USB) {
        static const char *names[4] = {"3.0k", "2.2k", "1.2k", "4.0k"};
        return names[gHF_BwIndex % 4];
    }
    static const char *names[4] = {"6k", "4k", "3k", "2k"};
    return names[gHF_BwIndex % 4];
}

void HF_ApplyBand(uint8_t band)
{
    if (band >= gHF_BandCount)
        band = 0;
    gHF_Band = band;

    if (!HF_ACTIVE) {
        FM_SetFrequency(); /* back to FM broadcast, legacy state */
        return;
    }

    const HF_Band_t *b = &gHF_Bands[band];
    gHF_Mode    = b->mode;
    gHF_BwIndex = 0;
    gHF_Freq    = sHF_BandFreq[band];
    if (gHF_Freq < b->lo || gHF_Freq > b->hi)
        gHF_Freq = b->lo;
    gHF_StepIndex = (gHF_Mode == SI47XX_AM) ? 2 : 1; /* 5 kHz / 1 kHz */
    HF_Tune();
}
#endif



const uint8_t BUTTON_STATE_PRESSED = 1 << 0;
const uint8_t BUTTON_STATE_HELD = 1 << 1;

const uint8_t BUTTON_EVENT_PRESSED = BUTTON_STATE_PRESSED;
const uint8_t BUTTON_EVENT_HELD = BUTTON_STATE_PRESSED | BUTTON_STATE_HELD;
const uint8_t BUTTON_EVENT_SHORT =  0;
const uint8_t BUTTON_EVENT_LONG =  BUTTON_STATE_HELD;


static void Key_FUNC(KEY_Code_t Key, uint8_t state);

bool FM_CheckValidChannel(uint8_t Channel)
{
    return  Channel < ARRAY_SIZE(gFM_Channels) && 
            gFM_Channels[Channel] >= BK1080_GetFreqLoLimit(gEeprom.FM_Band) && 
            gFM_Channels[Channel] < BK1080_GetFreqHiLimit(gEeprom.FM_Band);
}

uint8_t FM_FindNextChannel(uint8_t Channel, uint8_t Direction)
{
    for (unsigned i = 0; i < ARRAY_SIZE(gFM_Channels); i++) {
        if (Channel == 0xFF)
            Channel = ARRAY_SIZE(gFM_Channels) - 1;
        else if (Channel >= ARRAY_SIZE(gFM_Channels))
            Channel = 0;
        if (FM_CheckValidChannel(Channel))
            return Channel;
        Channel += Direction;
    }

    return 0xFF;
}

int FM_ConfigureChannelState(void)
{
    gEeprom.FM_FrequencyPlaying = gEeprom.FM_SelectedFrequency;

    if (gEeprom.FM_IsMrMode) {
        const uint8_t Channel = FM_FindNextChannel(gEeprom.FM_SelectedChannel, FM_CHANNEL_UP);
        if (Channel == 0xFF) {
            gEeprom.FM_IsMrMode = false;
            return -1;
        }
        gEeprom.FM_SelectedChannel  = Channel;
        gEeprom.FM_FrequencyPlaying = gFM_Channels[Channel];
    }

    return 0;
}

void FM_SetFrequency(void)
{
#ifdef ENABLE_SI4732
    /* FM_FrequencyPlaying is in 100 kHz units -> 10 Hz units */
    SI47XX_PowerOn(SI47XX_FM);
    SI47XX_TuneTo((uint32_t)gEeprom.FM_FrequencyPlaying * 10000);
#else
    BK1080_SetFrequency(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band/*, gEeprom.FM_Space*/);
#endif
}

void FM_TurnOff(void)
{
    gFmRadioMode              = false;
    gFM_ScanState             = FM_SCAN_OFF;
    gFM_RestoreCountdown_10ms = 0;

    AUDIO_AudioPathOff();
    gEnableSpeaker = false;

#ifdef ENABLE_SI4732
    SI47XX_PowerDown();
#else
    BK1080_Init0();
#endif

    // Enable relevant LNA based on VFO frequency
    BK4819_PickRXFilterPathBasedOnFrequency(gRxVfo->freq_config_RX.Frequency);


    gUpdateStatus  = true;

    #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        gEeprom.CURRENT_STATE = 0;
        SETTINGS_WriteCurrentState();
    #endif
}

void FM_EraseChannels(void)
{
    //PY25Q16_SectorErase(0x003000);
    
    uint8_t clearBuf[128];
    memset(clearBuf, 0xFF, sizeof(clearBuf));
    PY25Q16_WriteBuffer(0x00A028, clearBuf, sizeof(clearBuf), false);

    memset(gFM_Channels, 0xFF, sizeof(gFM_Channels));
}

uint16_t FM_WrapFrequency(uint16_t Frequency) {
    const uint16_t freqLoLimit = BK1080_GetFreqLoLimit(gEeprom.FM_Band);
    const uint16_t freqHiLimit = BK1080_GetFreqHiLimit(gEeprom.FM_Band);

    if (Frequency < freqLoLimit)
        return freqHiLimit;
    else if (Frequency > freqHiLimit)
        return freqLoLimit;

    return Frequency;
}

void FM_Tune(uint16_t Frequency, int8_t Step, bool bFlag)
{
    AUDIO_AudioPathOff();

    gEnableSpeaker = false;

    gFmPlayCountdown_10ms = (gFM_ScanState == FM_SCAN_OFF) ? fm_play_countdown_noscan_10ms : fm_play_countdown_scan_10ms;

    gScheduleFM                 = false;
    gFM_FoundFrequency          = false;
    gAskToSave                  = false;
    gAskToDelete                = false;
    gEeprom.FM_FrequencyPlaying = Frequency;

    if (!bFlag) {
        Frequency += Step;
        Frequency = FM_WrapFrequency(Frequency);

        gEeprom.FM_FrequencyPlaying = Frequency;
    }

    gFM_ScanState = Step;

    FM_SetFrequency();
}

void FM_AudioPathOn(void) {
    AUDIO_AudioPathOn();
    gEnableSpeaker = true;
}

void FM_PlayAndUpdate(void)
{
    gFM_ScanState = FM_SCAN_OFF;

    if (gFM_AutoScan) {
        gEeprom.FM_IsMrMode        = true;
        gEeprom.FM_SelectedChannel = 0;
    }

    FM_ConfigureChannelState();
    FM_SetFrequency();
    SETTINGS_SaveFM();

    gFmPlayCountdown_10ms = 0;
    gScheduleFM           = false;
    gAskToSave            = false;

    BACKLIGHT_TurnOn();
    FM_AudioPathOn();
}

#ifdef ENABLE_SI4732
int FM_CheckFrequencyLock(uint16_t Frequency, uint16_t LowerLimit)
{
    (void)Frequency;
    (void)LowerLimit;

    RSQ_GET();
    return (rsqStatus.resp.VALID && rsqStatus.resp.SNR >= 2) ? 0 : -1;
}
#else
int FM_CheckFrequencyLock(uint16_t Frequency, uint16_t LowerLimit)
{
    const uint16_t Test2     = BK1080_ReadRegister(BK1080_REG_07);
    const uint16_t Deviation = BK1080_REG_07_GET_FREQD(Test2);

    // Helper macro to update globals and return
    #define RETURN(val) \
        do { \
            BK1080_FrequencyDeviation = Deviation; \
            BK1080_BaseFrequency      = Frequency; \
            return (val); \
        } while (0)

    if (BK1080_REG_07_GET_SNR(Test2) <= 2)
        RETURN(-1);

    const uint16_t Status = BK1080_ReadRegister(BK1080_REG_10);
    if ((Status & BK1080_REG_10_MASK_AFCRL) != BK1080_REG_10_AFCRL_NOT_RAILED ||
        BK1080_REG_10_GET_RSSI(Status) < 10)
        RETURN(-1);

    if (Deviation >= 280 && Deviation <= 3815)
        RETURN(-1);

    // Scanning upward: previous deviation was negative (bit 11 set) or near zero
    if (Frequency > LowerLimit && (Frequency - BK1080_BaseFrequency) == 1) {
        if (BK1080_FrequencyDeviation & 0x800 || BK1080_FrequencyDeviation < 20)
            RETURN(-1);
    }

    // Scanning downward: previous deviation was positive or saturated high
    if (Frequency >= LowerLimit && (BK1080_BaseFrequency - Frequency) == 1) {
        if ((BK1080_FrequencyDeviation & 0x800) == 0 || BK1080_FrequencyDeviation > 4075)
            RETURN(-1);
    }

    #undef RETURN

    BK1080_FrequencyDeviation = Deviation;
    BK1080_BaseFrequency      = Frequency;
    return 0;
}
#endif

static void Key_DIGITS(KEY_Code_t Key, uint8_t state)
{
    enum { STATE_FREQ_MODE, STATE_MR_MODE, STATE_SAVE };

    if (state == BUTTON_EVENT_SHORT && !gWasFKeyPressed) {
        uint8_t State;

        if (gAskToDelete) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        if (gAskToSave) {
            State = STATE_SAVE;
        }
        else {
            if (gFM_ScanState != FM_SCAN_OFF) {
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                return;
            }

            State = gEeprom.FM_IsMrMode ? STATE_MR_MODE : STATE_FREQ_MODE;
        }

        INPUTBOX_Append(Key);
        gKeyInputCountdown = key_input_timeout_500ms;

        gRequestDisplayScreen = DISPLAY_FM;

#ifdef ENABLE_SI4732
        if (HF_ACTIVE) {
            if (gInputBoxIndex > 4) { // 5 digits: frequency in kHz
                gInputBoxIndex = 0;
                gKeyInputCountdown = 1;

                uint32_t f = StrToUL(INPUTBOX_GetAscii()) * 100; // 10 Hz units

                if (f < SI47XX_F_MIN || f > SI47XX_F_MAX) {
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                    return;
                }

                if (f < gHF_Bands[gHF_Band].lo || f > gHF_Bands[gHF_Band].hi) {
                    // hop to the (most specific) band containing f;
                    // the trailing SW catch-all always matches
                    for (uint8_t i = 1; i < gHF_BandCount; i++) {
                        if (f >= gHF_Bands[i].lo && f <= gHF_Bands[i].hi) {
                            gHF_Band    = i;
                            gHF_Mode    = gHF_Bands[i].mode;
                            gHF_BwIndex = 0;
                            break;
                        }
                    }
                }

                gHF_Freq = f;
                HF_Tune();
                gRequestSaveFM = true;
            }
            return;
        }
#endif

        if (State == STATE_FREQ_MODE) {
            if (gInputBoxIndex == 1) {
                if (gInputBox[0] > 1) {
                    gInputBox[1] = gInputBox[0];
                    gInputBox[0] = 0;
                    gInputBoxIndex = 2;
                }
            }
            else if (gInputBoxIndex > 3) {
                uint32_t Frequency;

                gInputBoxIndex = 0;
                gKeyInputCountdown = 1;

                Frequency = StrToUL(INPUTBOX_GetAscii());

                if (Frequency < BK1080_GetFreqLoLimit(gEeprom.FM_Band) || BK1080_GetFreqHiLimit(gEeprom.FM_Band) < Frequency) {
                    gBeepToPlay           = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                    gRequestDisplayScreen = DISPLAY_FM;
                    return;
                }

                gEeprom.FM_SelectedFrequency = (uint16_t)Frequency;
#ifdef ENABLE_VOICE
                gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
                gEeprom.FM_FrequencyPlaying = gEeprom.FM_SelectedFrequency;
                FM_SetFrequency();
                gRequestSaveFM = true;
                return;
            }
        }
        else if (gInputBoxIndex == 2) {
            uint8_t Channel;

            gInputBoxIndex = 0;
            gKeyInputCountdown = 1;
            
            Channel = ((gInputBox[0] * 10) + gInputBox[1]) - 1;

            if (State == STATE_MR_MODE) {
                if (FM_CheckValidChannel(Channel)) {
#ifdef ENABLE_VOICE
                    gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
                    gEeprom.FM_SelectedChannel = Channel;
                    gEeprom.FM_FrequencyPlaying = gFM_Channels[Channel];
                    FM_SetFrequency();
                    gRequestSaveFM = true;
                    return;
                }
            }
            else if (Channel < FM_CHANNELS_MAX) {
#ifdef ENABLE_VOICE
                gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
                gRequestDisplayScreen = DISPLAY_FM;
                gInputBoxIndex = 0;
                gFM_ChannelPosition = Channel;
                return;
            }

            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
    }
    else
        Key_FUNC(Key, state);
}

static void Key_FUNC(KEY_Code_t Key, uint8_t state)
{
    if (state == BUTTON_EVENT_SHORT || state == BUTTON_EVENT_HELD) {
        bool autoScan = gWasFKeyPressed || (state == BUTTON_EVENT_HELD);

        gBeepToPlay           = BEEP_1KHZ_60MS_OPTIONAL;
        HideFKeyIcon();
        gRequestDisplayScreen = DISPLAY_FM;

        switch (Key) {
            case KEY_0:
                ACTION_FM();
                break;

            case KEY_1:
#ifdef ENABLE_SI4732
                HF_ApplyBand((uint8_t)((gHF_Band + 1) % gHF_BandCount));
                gRequestSaveFM = true;
#else
                gEeprom.FM_Band++;
                gRequestSaveFM = true;
#endif
                break;

#ifdef ENABLE_SI4732
            case KEY_2: // tuning step (HF bands only)
                if (HF_ACTIVE)
                    gHF_StepIndex = (uint8_t)((gHF_StepIndex + 1) % 5);
                else
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                break;
#endif
            // case KEY_2:
            //  gEeprom.FM_Space = (gEeprom.FM_Space + 1) % 3;
            //  gRequestSaveFM = true;
            //  break;

            case KEY_3:
#ifdef ENABLE_SI4732
                if (HF_ACTIVE) {
                    // AM -> LSB -> USB -> AM (SSB entry loads the patch, ~2 s)
                    if (gHF_Mode == SI47XX_AM)
                        gHF_Mode = SI47XX_LSB;
                    else if (gHF_Mode == SI47XX_LSB)
                        gHF_Mode = SI47XX_USB;
                    else
                        gHF_Mode = SI47XX_AM;
                    gHF_BwIndex = 0;
                    HF_Tune();
                    gRequestSaveFM = true;
                    break;
                }
#endif
                gEeprom.FM_IsMrMode = !gEeprom.FM_IsMrMode;

                if (!FM_ConfigureChannelState()) {
                    FM_SetFrequency();
                    gRequestSaveFM = true;
                }
                else
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                break;

            case KEY_8:
                ACTION_BackLightOnDemand();
                break;

            case KEY_9:
                ACTION_BackLight();
                break;

            case KEY_STAR:
#ifdef ENABLE_SI4732
                if (HF_ACTIVE) { // no scan on the HF bands (yet)
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                    break;
                }
#endif
                ACTION_Scan(autoScan);
                break;

            default:
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                break;
        }
    }
}

static void Key_EXIT(uint8_t state)
{
    if (gInputBoxIndex) {
        if (state != BUTTON_EVENT_SHORT)
            return;
    } 
    else {
        if (state != BUTTON_EVENT_PRESSED)
            return;
    }

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    if (gFM_ScanState == FM_SCAN_OFF) {
        if (gInputBoxIndex == 0) {
            if (!gAskToSave && !gAskToDelete) {
                ACTION_FM();
                return;
            }

            gAskToSave   = false;
            gAskToDelete = false;
        }
        else {
            gInputBox[--gInputBoxIndex] = 10;
            gKeyInputCountdown = key_input_timeout_500ms;

            if (gInputBoxIndex) {
                if (gInputBoxIndex != 1) {
                    gRequestDisplayScreen = DISPLAY_FM;
                    return;
                }

                if (gInputBox[0] != 0) {
                    gRequestDisplayScreen = DISPLAY_FM;
                    return;
                }
            }
            gInputBoxIndex = 0;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = VOICE_ID_CANCEL;
#endif
    }
    else {
        FM_PlayAndUpdate();
#ifdef ENABLE_VOICE
        gAnotherVoiceID = VOICE_ID_SCANNING_STOP;
#endif
    }

    gRequestDisplayScreen = DISPLAY_FM;
}

static void Key_MENU(uint8_t state)
{
    if (state == BUTTON_EVENT_HELD) {
        ACTION_Handle(KEY_MENU, true, true);
        return;
    }
    else if (state != BUTTON_EVENT_SHORT) {
        return;
    }

    gRequestDisplayScreen = DISPLAY_FM;
    gBeepToPlay           = BEEP_1KHZ_60MS_OPTIONAL;

    HideFKeyIcon();

    if (gFM_ScanState == FM_SCAN_OFF) {
        if (gInputBoxIndex) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

#ifdef ENABLE_SI4732
        if (HF_ACTIVE) { // cycle the RX filter bandwidth
            gHF_BwIndex = (uint8_t)((gHF_BwIndex + 1) % 4);
            if (gHF_Mode == SI47XX_LSB || gHF_Mode == SI47XX_USB) {
                static const uint8_t ssbBw[4] = {
                    SI47XX_SSB_BW_3_kHz, SI47XX_SSB_BW_2_2_kHz,
                    SI47XX_SSB_BW_1_2_kHz, SI47XX_SSB_BW_4_kHz};
                SI47XX_SetSsbBandwidth((SI47XX_SsbFilterBW)ssbBw[gHF_BwIndex]);
            } else {
                static const uint8_t amBw[4] = {
                    SI47XX_BW_6_kHz, SI47XX_BW_4_kHz,
                    SI47XX_BW_3_kHz, SI47XX_BW_2_kHz};
                SI47XX_SetBandwidth((SI47XX_FilterBW)amBw[gHF_BwIndex], true);
            }
            return;
        }
#endif

        if (!gEeprom.FM_IsMrMode) {
            if (gAskToSave) {
                gFM_Channels[gFM_ChannelPosition] = gEeprom.FM_FrequencyPlaying;
                gRequestSaveFM = true;
            }
            gAskToSave = !gAskToSave;
        }
        else {
            if (gAskToDelete) {
                gFM_Channels[gEeprom.FM_SelectedChannel] = 0xFFFF;

                FM_ConfigureChannelState();
                FM_SetFrequency();

                gRequestSaveFM = true;
            }
            gAskToDelete = !gAskToDelete;
        }
    }
    else {
        if (gFM_AutoScan || !gFM_FoundFrequency) {
            gBeepToPlay    = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            gInputBoxIndex = 0;
            return;
        }

        if (gAskToSave) {
            gFM_Channels[gFM_ChannelPosition] = gEeprom.FM_FrequencyPlaying;
            gRequestSaveFM = true;
        }
        gAskToSave = !gAskToSave;
    }
}

static void Key_UP_DOWN(uint8_t state, int8_t Step)
{
    HideFKeyIcon();

    if (state == BUTTON_EVENT_PRESSED) {
        if (gInputBoxIndex) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    } else if (gInputBoxIndex || state!=BUTTON_EVENT_HELD) {
        return;
    }

    // SET_NAV (exposed in CHIRP as the direction-keys layout) flips the
    // natural direction for the K1 layout, same convention as scanner/
    // aircopy/spectrum: the call site passes KEY_UP -> +1 and SET_NAV=0
    // (the UV-K1 default) inverts it to match the main VFO screen's
    // hardcoded mapping. Previously the call site passed -1 AND this
    // block flipped it again, so on a default K1 the FM tuning keys came
    // out K5-oriented (inverted).
    if (!gEeprom.SET_NAV) {
        Step = -Step;
    }

#ifdef ENABLE_SI4732
    if (HF_ACTIVE) {
        const HF_Band_t *b = &gHF_Bands[gHF_Band];
        int32_t f = (int32_t)gHF_Freq + Step * (int32_t)HF_GetStep();

        if (f < (int32_t)b->lo)
            f = (int32_t)b->hi;
        else if (f > (int32_t)b->hi)
            f = (int32_t)b->lo;

        gHF_Freq = (uint32_t)f;
        HF_Tune();
        gRequestSaveFM = true;
        gRequestDisplayScreen = DISPLAY_FM;
        return;
    }
#endif

    if (gAskToSave) {
        gRequestDisplayScreen = DISPLAY_FM;
        gFM_ChannelPosition   = NUMBER_AddWithWraparound(gFM_ChannelPosition, Step, 0, FM_CHANNELS_MAX - 1);
        return;
    }

    if (gFM_ScanState != FM_SCAN_OFF) {
        if (gFM_AutoScan) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        FM_Tune(gEeprom.FM_FrequencyPlaying, Step, false);
        gRequestDisplayScreen = DISPLAY_FM;
        return;
    }

    if (gEeprom.FM_IsMrMode) {
        const uint8_t Channel = FM_FindNextChannel(gEeprom.FM_SelectedChannel + Step, Step);
        if (Channel == 0xFF || gEeprom.FM_SelectedChannel == Channel)
            goto Bail;

        gEeprom.FM_SelectedChannel  = Channel;
        gEeprom.FM_FrequencyPlaying = gFM_Channels[Channel];
    }
    else {
        uint16_t Frequency = gEeprom.FM_SelectedFrequency + Step;

        Frequency = FM_WrapFrequency(Frequency);

        gEeprom.FM_FrequencyPlaying  = Frequency;
        gEeprom.FM_SelectedFrequency = gEeprom.FM_FrequencyPlaying;
    }

    gRequestSaveFM = true;

Bail:
    FM_SetFrequency();

    gRequestDisplayScreen = DISPLAY_FM;
}

void FM_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    uint8_t state = bKeyPressed + 2 * bKeyHeld;

    switch (Key) {
        case KEY_0...KEY_9:
            Key_DIGITS(Key, state);
            break;
        case KEY_STAR:
            Key_FUNC(Key, state);
            break;
        case KEY_MENU:
            Key_MENU(state);
            break;
        case KEY_UP:
        case KEY_DOWN:
            Key_UP_DOWN(state, Key == KEY_UP ? 1 : -1);
            break;
        case KEY_EXIT:
            Key_EXIT(state);
            break;
        case KEY_F:
            GENERIC_Key_F(bKeyPressed, bKeyHeld);
            break;
        case KEY_PTT:
            GENERIC_Key_PTT(bKeyPressed);
            break;
        case KEY_SIDE1:
        case KEY_SIDE2:
            if (state != BUTTON_EVENT_PRESSED) {
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                HideFKeyIcon();
            }
            break;
        default:
            break;
    }
}

void FM_Play(void)
{
    if (!FM_CheckFrequencyLock(gEeprom.FM_FrequencyPlaying, BK1080_GetFreqLoLimit(gEeprom.FM_Band))) {
        if (!gFM_AutoScan) {
            gFmPlayCountdown_10ms = 0;
            gFM_FoundFrequency    = true;

            if (!gEeprom.FM_IsMrMode)
                gEeprom.FM_SelectedFrequency = gEeprom.FM_FrequencyPlaying;

            BACKLIGHT_TurnOn();
            FM_AudioPathOn();

            goto Display;
        }

        if (gFM_ChannelPosition < FM_CHANNELS_MAX)
            gFM_Channels[gFM_ChannelPosition++] = gEeprom.FM_FrequencyPlaying;
        
        if (gFM_ChannelPosition >= FM_CHANNELS_MAX) {
            FM_PlayAndUpdate();

            goto Display;
        }
    }

    if (gFM_AutoScan && gEeprom.FM_FrequencyPlaying >= BK1080_GetFreqHiLimit(1))
        FM_PlayAndUpdate();
    else
        FM_Tune(gEeprom.FM_FrequencyPlaying, gFM_ScanState, false);

Display:
    GUI_SelectNextDisplay(DISPLAY_FM);
}

void FM_Start(void)
{
    gDualWatchActive          = false;
    gFmRadioMode              = true;
    gFM_ScanState             = FM_SCAN_OFF;
    gFM_RestoreCountdown_10ms = 0;

#ifdef ENABLE_SI4732
    if (HF_ACTIVE)
        HF_Tune();
    else
        FM_SetFrequency();
#else
    BK1080_Init(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band/*, gEeprom.FM_Space*/);
#endif
    // Disable UHF LNA, enable VHF LNA
    BK4819_PickRXFilterPathBasedOnFrequency(10320000); // 103.2 MHz < 280 MHz

    FM_AudioPathOn();

    gUpdateStatus        = true;

    #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        gEeprom.CURRENT_STATE = 3;
        SETTINGS_WriteCurrentState();
    #endif
}

#endif
