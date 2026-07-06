/* Original work Copyright 2023 joaquimorg
 * https://github.com/joaquimorg
 *
 * Modified work Copyright 2024 kamilsss655
 * https://github.com/kamilsss655
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

#ifdef ENABLE_MESSENGER

#include <string.h>
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/bk4819.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "radio.h"
#include "app.h"
#include "audio.h"
#include "functions.h"
#include "frequencies.h"
#include "driver/system.h"
#include "app/messenger.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "ui/ui.h"
#ifdef ENABLE_ENCRYPTION
	#include "helper/crypto.h"
#endif
#ifdef ENABLE_MESSENGER_UART
    #include "driver/uart.h"
#endif

const uint8_t MSG_BUTTON_STATE_HELD = 1 << 1;

const uint8_t MSG_BUTTON_EVENT_SHORT =  0;
const uint8_t MSG_BUTTON_EVENT_LONG =  MSG_BUTTON_STATE_HELD;

const uint8_t MAX_MSG_LENGTH = PAYLOAD_LENGTH - 1;

uint16_t TONE2_FREQ;

#define NEXT_CHAR_DELAY 100 // 10ms tick

char T9TableLow[9][4] = { {',', '.', '?', '!'}, {'a', 'b', 'c', '\0'}, {'d', 'e', 'f', '\0'}, {'g', 'h', 'i', '\0'}, {'j', 'k', 'l', '\0'}, {'m', 'n', 'o', '\0'}, {'p', 'q', 'r', 's'}, {'t', 'u', 'v', '\0'}, {'w', 'x', 'y', 'z'} };
char T9TableUp[9][4] = { {',', '.', '?', '!'}, {'A', 'B', 'C', '\0'}, {'D', 'E', 'F', '\0'}, {'G', 'H', 'I', '\0'}, {'J', 'K', 'L', '\0'}, {'M', 'N', 'O', '\0'}, {'P', 'Q', 'R', 'S'}, {'T', 'U', 'V', '\0'}, {'W', 'X', 'Y', 'Z'} };
unsigned char numberOfLettersAssignedToKey[9] = { 4, 3, 3, 3, 3, 3, 4, 3, 4 };

char T9TableNum[9][4] = { {'1', '\0', '\0', '\0'}, {'2', '\0', '\0', '\0'}, {'3', '\0', '\0', '\0'}, {'4', '\0', '\0', '\0'}, {'5', '\0', '\0', '\0'}, {'6', '\0', '\0', '\0'}, {'7', '\0', '\0', '\0'}, {'8', '\0', '\0', '\0'}, {'9', '\0', '\0', '\0'} };
unsigned char numberOfNumsAssignedToKey[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };

char cMessage[PAYLOAD_LENGTH];
char lastcMessage[PAYLOAD_LENGTH];
char rxMessage[MSG_LINES][PAYLOAD_LENGTH + 2];
unsigned char cIndex = 0;
unsigned char prevKey = 0, prevLetter = 0;
KeyboardType keyboardType = UPPERCASE;

MsgStatus msgStatus = READY;

union DataPacket dataPacket;

uint16_t gErrorsDuringMSG;

// recovery watchdog: if FSK_RX_SYNC fires but FSK_RX_FINISHED never
// follows (demod gets stuck mid-packet), force the FSK RX path back
// to idle after MSG_RX_TIMEOUT_10MS so the squelch doesn't stay open
#define MSG_RX_TIMEOUT_10MS 100  // 1 second
uint8_t gMsgRxTimeout10ms;

// deferred ACK: armed by MSG_HandleReceive, fired from the 10ms tick so
// the received message is drawn before the radio keys up for the reply
uint16_t gMsgAckCountdown10ms;

// range check: deferred PONG reply (armed on PING RX, with random jitter so
// multiple radios don't all answer at once) and the RSSI of the last packet
uint16_t gMsgPongCountdown10ms;
static int16_t gMsgLastRxRssiDbm;
static void MSG_SendPong(void);

// small xorshift PRNG for the ACK/PONG reply jitter. An AFSK1200 packet
// occupies the air for ~0.5s (15B preamble + sync + payload at 1200 bit/s),
// so anti-collision windows must be several times that: with the old 0.5s
// PONG window two repliers overlapped almost every time, and the ACK had no
// jitter at all (fixed 700ms - two radios acknowledging the same broadcast
// keyed up in the same instant, guaranteed collision).
//
// The stream must differ per radio, or the jitter is useless: a fixed seed
// makes two identical radios with the same event history draw the SAME
// delays and collide systematically. So the callsign (unique per station)
// is mixed in at init, and every draw stirs in the instantaneous raw RSSI,
// whose low bits are thermal-noise entropy that differs between radios
// even in the same room.
static uint16_t s_msgRng = 0xACE1;

static uint16_t MSG_Rand(void) {
	s_msgRng ^= BK4819_GetRSSI();
	s_msgRng ^= s_msgRng << 7; s_msgRng ^= s_msgRng >> 9; s_msgRng ^= s_msgRng << 8;
	return s_msgRng;
}

uint8_t hasNewMessage = 0;

uint8_t keyTickCounter = 0;

// message-history scroll offset: 0 shows the newest lines at the bottom,
// higher values scroll back into older history (UP/DOWN in the messenger)
uint8_t gMsgScroll = 0;

// true while editing the station callsign on-radio (F+SIDE2), reusing the T9
// message-input machinery instead of composing a message
bool gMsgEditCallsign = false;

// Build the "CALLSIGN:" prefix auto-prepended to outgoing messages for ID
// compliance. Sanitizes the EEPROM callsign (printable ASCII only, up to 8
// chars; erased flash 0xFF or NUL ends it). Returns prefix length (0 if no
// callsign set). dst must hold at least sizeof(CALLSIGN)+2 bytes.
static uint8_t MSG_BuildCallPrefix(char *dst) {
	uint8_t n = 0;
	for (uint8_t i = 0; i < sizeof(gEeprom.CALLSIGN); i++) {
		char c = gEeprom.CALLSIGN[i];
		if (c == 0 || (uint8_t)c == 0xFF || c < 0x20 || c > 0x7E)
			break;
		dst[n++] = c;
	}
	if (n > 0)
		dst[n++] = ':';
	dst[n] = 0;
	return n;
}

// Max characters the user may type, leaving room for the callsign prefix so
// prefix + text still fit in PAYLOAD_LENGTH.
static uint8_t MSG_MsgInputMax(void) {
	if (gMsgEditCallsign)
		return sizeof(gEeprom.CALLSIGN);  // editing callsign: up to 8 chars
	char tmp[sizeof(gEeprom.CALLSIGN) + 2];
	uint8_t p = MSG_BuildCallPrefix(tmp);
	return (MAX_MSG_LENGTH > p) ? (uint8_t)(MAX_MSG_LENGTH - p) : 1;
}

// --- on-radio callsign editor (F+SIDE2), reuses the T9 message input ---

static void MSG_EnterCallsignEdit(void) {
	uint8_t n = 0;
	memset(cMessage, 0, sizeof(cMessage));
	for (uint8_t i = 0; i < sizeof(gEeprom.CALLSIGN); i++) {
		char c = gEeprom.CALLSIGN[i];
		if (c == 0 || (uint8_t)c == 0xFF || c < 0x20 || c > 0x7E)
			break;
		cMessage[n++] = c;
	}
	cIndex = n;
	prevKey = 0;
	prevLetter = 0;
	gMsgEditCallsign = true;
}

static void MSG_ExitCallsignEdit(void) {
	gMsgEditCallsign = false;
	memset(cMessage, 0, sizeof(cMessage));
	cIndex = 0;
	prevKey = 0;
	prevLetter = 0;
}

static void MSG_SaveCallsignEdit(void) {
	// store the typed callsign into the EEPROM field (NUL-padded; empty clears
	// it), persist, then leave edit mode
	memset(gEeprom.CALLSIGN, 0, sizeof(gEeprom.CALLSIGN));
	for (uint8_t i = 0; i < sizeof(gEeprom.CALLSIGN) && cMessage[i]; i++)
		gEeprom.CALLSIGN[i] = cMessage[i];
	SETTINGS_SaveCallsign();
	MSG_ExitCallsignEdit();
}

// Rough RSSI -> distance estimate (log-distance path-loss model, integer math,
// no libm). VERY approximate: ignores TX power/antenna/terrain/obstacles. Two
// knobs to calibrate against real field measurements:
//   MSG_DIST_REF_RSSI : the RSSI (dBm) you read at MSG_DIST_REF_M
//   MSG_DIST_DB_PER_2X: dB of extra path loss per doubling of distance
//                       (~6 = free space n=2, ~9 = n=3, ~12 = n=4 obstructed)
#define MSG_DIST_REF_RSSI   (-95)
#define MSG_DIST_REF_M      (1000)
#define MSG_DIST_DB_PER_2X  (9)

static uint16_t MSG_EstimateDistanceM(int16_t rssi_dbm) {
	// distance = REF_M * 2^((REF_RSSI - rssi) / DB_PER_2X)
	int32_t excess = (int32_t)MSG_DIST_REF_RSSI - rssi_dbm;  // dB beyond reference
	int32_t whole  = excess / MSG_DIST_DB_PER_2X;            // integer doublings
	int32_t frac   = excess - whole * MSG_DIST_DB_PER_2X;
	if (frac < 0) { frac += MSG_DIST_DB_PER_2X; whole -= 1; } // floor toward -inf
	int32_t m = MSG_DIST_REF_M;
	if (whole >= 0) {
		if (whole > 8) whole = 8;     // clamp before shifting
		m <<= whole;
	} else {
		if (whole < -10) whole = -10;
		m >>= (-whole);
	}
	// fractional doubling, linearly interpolated 1.0..2.0 over the remainder
	m += (int32_t)((int64_t)m * frac / MSG_DIST_DB_PER_2X);
	if (m < 0) m = 0;
	if (m > 99000) m = 99000;         // clamp ~99 km
	return (uint16_t)m;
}

// Format the estimate as "320m" or "1.5km" into dst (>=8 bytes)
static void MSG_FormatDistance(char *dst, uint8_t dstSize, int16_t rssi_dbm) {
	uint16_t m = MSG_EstimateDistanceM(rssi_dbm);
	if (m < 1000)
		snprintf(dst, dstSize, "%um", m);
	else
		snprintf(dst, dstSize, "%u.%ukm", m / 1000, (m % 1000) / 100);
}

// -----------------------------------------------------

void MSG_FSKSendData() {

	// turn off CTCSS/CDCSS during FFSK
	const uint16_t css_val = BK4819_ReadRegister(BK4819_REG_51);
	BK4819_WriteRegister(BK4819_REG_51, 0);

	// set the FM deviation level
	const uint16_t dev_val = BK4819_ReadRegister(BK4819_REG_40);

	{
		uint16_t deviation;
		switch (gEeprom.VfoInfo[gEeprom.TX_VFO].CHANNEL_BANDWIDTH)
		{
			case BK4819_FILTER_BW_WIDE:            deviation =  1300; break; // 20k // measurements by kamilsss655
			case BK4819_FILTER_BW_NARROW:          deviation =  1200; break; // 10k
			// case BK4819_FILTER_BW_NARROWAVIATION:  deviation =  850; break;  // 5k
			// case BK4819_FILTER_BW_NARROWER:        deviation =  850; break;  // 5k
			// case BK4819_FILTER_BW_NARROWEST:	      deviation =  850; break;  // 5k
			default:                               deviation =  850;  break;  // 5k
		}

		//BK4819_WriteRegister(0x40, (3u << 12) | (deviation & 0xfff));
		BK4819_WriteRegister(BK4819_REG_40, (dev_val & 0xf000) | (deviation & 0xfff));
	}

	// REG_2B   0
	//
	// <15> 1 Enable CTCSS/CDCSS DC cancellation after FM Demodulation   1 = enable 0 = disable
	// <14> 1 Enable AF DC cancellation after FM Demodulation            1 = enable 0 = disable
	// <10> 0 AF RX HPF 300Hz filter     0 = enable 1 = disable
	// <9>  0 AF RX LPF 3kHz filter      0 = enable 1 = disable
	// <8>  0 AF RX de-emphasis filter   0 = enable 1 = disable
	// <2>  0 AF TX HPF 300Hz filter     0 = enable 1 = disable
	// <1>  0 AF TX LPF filter           0 = enable 1 = disable
	// <0>  0 AF TX pre-emphasis filter  0 = enable 1 = disable
	//
	// disable the 300Hz HPF and FM pre-emphasis filter
	//
	const uint16_t filt_val = BK4819_ReadRegister(BK4819_REG_2B);
	BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));
	
	MSG_ConfigureFSK(false);



	SYSTEM_DelayMs(100);

	{	// load the entire packet data into the TX FIFO buffer
		for (size_t i = 0, j = 0; i < sizeof(dataPacket.serializedArray); i += 2, j++) {
        	BK4819_WriteRegister(BK4819_REG_5F, (dataPacket.serializedArray[i + 1] << 8) | dataPacket.serializedArray[i]);
    	}
	}

	// enable FSK TX
	BK4819_FskEnableTx();

	{
		// allow up to 310ms for the TX to complete
		// if it takes any longer then somethings gone wrong, we shut the TX down
		unsigned int timeout = 1000 / 5;

		while (timeout-- > 0)
		{
			SYSTEM_DelayMs(5);
			if (BK4819_ReadRegister(BK4819_REG_0C) & (1u << 0))
			{	// we have interrupt flags
				BK4819_WriteRegister(BK4819_REG_02, 0);
				if (BK4819_ReadRegister(BK4819_REG_02) & BK4819_REG_02_FSK_TX_FINISHED)
					timeout = 0;       // TX is complete
			}
		}
	}
	//BK4819_WriteRegister(BK4819_REG_02, 0);

	SYSTEM_DelayMs(100);

	// disable TX
	MSG_ConfigureFSK(true);

	// restore FM deviation level
	BK4819_WriteRegister(BK4819_REG_40, dev_val);

	// restore TX/RX filtering
	BK4819_WriteRegister(BK4819_REG_2B, filt_val);

	// restore the CTCSS/CDCSS setting
	BK4819_WriteRegister(BK4819_REG_51, css_val);

}

// deferred FSK-RX arming (GOGUFW principle): never manipulate the BK4829's
// FSK/DSP chain while a carrier is present. GOGUFW, on the identical chip,
// never latches its squelch - and the one discipline it follows that we
// didn't is gating every FSK arm/re-arm on "channel free" (REG_0C bit1).
// Our latch signature ("stuck open after a real RX, only a REG_00 soft
// reset clears it") matches arming the FSK engine on top of an active
// carrier: RADIO_SetupRegisters() calls MSG_EnableRX(true) on every channel
// change (possibly onto an occupied frequency) and at the tail of every RX.
// When the channel is busy we set this flag instead and the 10ms tick
// (MSG_CheckRxTimeout) performs the arm as soon as the squelch closes.
// Nothing is lost while deferred: FSK can't be decoded under voice anyway.
static bool gMsgRxArmPending;

void MSG_EnableRX(const bool enable) {

	if (enable) {
		if (gEeprom.MESSENGER_CONFIG.data.receive &&
		    (BK4819_ReadRegister(BK4819_REG_0C) & (1u << 1))) {
			// carrier present: defer the whole bring-up (see above)
			gMsgRxArmPending = true;
			return;
		}
		gMsgRxArmPending = false;

		// Fully reset the FSK engine before reconfiguring it. REG_59 (FSK
		// control: RX/TX enable, FIFO-clear, scramble bits) is only ever OR'd
		// into elsewhere - FskEnableRx() sets bit12, FskClearFifo() sets
		// bit15|bit14 - and never written back to 0, so the engine state
		// accumulates and is never reset short of a REG_00 soft-reset (i.e. a
		// power-cycle). After demodulating a real signal that residual FSK
		// state keeps the BK4829 RX/demod path engaged on noise, which is what
		// latches the squelch open ("stuck in RX, recovers only on power-cycle",
		// survives channel/VFO change). Since RADIO_SetupRegisters() calls us on
		// every channel change and at the end of every RX, clearing REG_58/59 to
		// zero here re-arms the FSK engine from a clean state each time and
		// recovers the latch deterministically - without the heavier full-chip
		// BK4819_Init(). The AGC is exonerated: the messenger re-inits the AGC
		// table MORE than a normal RX (extra InitAGC below) yet still sticks.
		BK4819_WriteRegister(BK4819_REG_58, 0);
		BK4819_WriteRegister(BK4819_REG_59, 0);

		MSG_ConfigureFSK(true);

		if(gEeprom.MESSENGER_CONFIG.data.receive) {
			// the K1 chip variant needs an RX DSP restart after the FSK
			// config is changed, or the demodulator never detects sync;
			// aircopy (working on this hardware) does the same via
			// BK4819_PrepareFSKReceive()
			BK4819_Idle();
			BK4819_RX_TurnOn();
			BK4819_FskEnableRx();

			// Force a true AGC re-init, bypassing RADIO_SetupAGC()'s static
			// `lastSettings` cache (which no-ops on an FM->FM retune, i.e. on
			// every channel change and every end-of-RX). Root cause of the
			// "stuck in RX, only power-cycle recovers" bug on the BK4829: with
			// messenger RX the AGC drifts its gain up on noise after a strong
			// signal until the RSSI sits above the squelch threshold; sqlLost
			// is level-triggered (see CheckRadioInterrupts loopGuard), so it
			// keeps re-asserting and the squelch latches open. RADIO_SetupAGC
			// would normally re-arm the AGC, but its cache skips it whenever
			// the modulation is unchanged - which is why changing channel did
			// NOT recover. RADIO_SetupRegisters() runs at the end of every RX
			// and on every channel change and calls us here, so resetting the
			// AGC to the stock profile at this point closes the drifted-open
			// squelch deterministically (the SetAGC false->true toggle forces
			// InitAGC to actually run past SetAGC's own no-change guard).
			BK4819_SetAGC(false);
			BK4819_InitAGC(false);
			BK4819_SetAGC(true);
			// BK4829: AF_MUTE also gates the FM demod that feeds the FSK
			// slicer, so the modem only hears with the AF path open (this
			// is why RX only worked with monitor on). Inaudible: the
			// speaker amp GPIO stays off while the squelch is closed.
			BK4819_SetAF(BK4819_AF_FM);
		}
	} else {
		gMsgRxArmPending = false;
		BK4819_WriteRegister(BK4819_REG_70, 0);
		BK4819_WriteRegister(BK4819_REG_58, 0);
	}
}


// -----------------------------------------------------

void moveUP(char (*rxMessages)[PAYLOAD_LENGTH + 2]) {
    // Shift existing lines up through the whole history buffer
    for (uint8_t i = 0; i < MSG_LINES - 1; i++)
        strcpy(rxMessages[i], rxMessages[i + 1]);

    // Insert the new line at the last (newest) position
    memset(rxMessages[MSG_LINES - 1], 0, sizeof(rxMessages[MSG_LINES - 1]));

    // a new line arrived: jump the view back to the newest
    gMsgScroll = 0;
}

void MSG_SendPacket() {

	if ( msgStatus != READY ) return;

	RADIO_PrepareTX();

	if(VfoState[gEeprom.TX_VFO] != VFO_STATE_NORMAL){
		gRequestDisplayScreen = DISPLAY_MAIN;
		return;
	} 

	if ( strlen((char *)dataPacket.data.payload) > 0) {

		msgStatus = SENDING;

		RADIO_SetVfoState(VFO_STATE_NORMAL);
		BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
		BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);

		// display sent message (before encryption); only for real messages,
		// not ACK/PING/PONG control packets
		if (dataPacket.data.header == MESSAGE_PACKET || dataPacket.data.header == ENCRYPTED_MESSAGE_PACKET) {
			moveUP(rxMessage);
			sprintf(rxMessage[MSG_LINES - 1], "> %s", dataPacket.data.payload);
			memset(lastcMessage, 0, sizeof(lastcMessage));
			memcpy(lastcMessage, dataPacket.data.payload, PAYLOAD_LENGTH);
			cIndex = 0;
			prevKey = 0;
			prevLetter = 0;
			memset(cMessage, 0, sizeof(cMessage));
		}

		#ifdef ENABLE_ENCRYPTION
			if(dataPacket.data.header == ENCRYPTED_MESSAGE_PACKET){

				CRYPTO_Random(dataPacket.data.nonce, NONCE_LENGTH);

				CRYPTO_Crypt(
					dataPacket.data.payload,
					PAYLOAD_LENGTH,
					dataPacket.data.payload,
					&dataPacket.data.nonce,
					gEncryptionKey,
					256
				);
			}
		#endif

		BK4819_DisableDTMF();

		// mute the mic during TX
		BK4819_MuteMic();

		SYSTEM_DelayMs(50);

		MSG_FSKSendData();

		SYSTEM_DelayMs(50);

		APP_EndTransmission();
		// this must be run after end of TX, otherwise radio will still TX transmit without even RED LED on
		FUNCTION_Select(FUNCTION_FOREGROUND);

		RADIO_SetVfoState(VFO_STATE_NORMAL);

		// disable mic mute after TX
		BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);

		BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

		MSG_EnableRX(true);

		// clear packet buffer
		MSG_ClearPacketBuffer();

		msgStatus = READY;

	} else {
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
	}
}

uint8_t validate_char( uint8_t rchar ) {
	if ( (rchar == 0x1b) || (rchar >= 32 && rchar <= 127) ) {
		return rchar;
	}
	return 32;
}

void MSG_StorePacket(const uint16_t interrupt_bits) {

	//const uint16_t rx_sync_flags   = BK4819_ReadRegister(BK4819_REG_0B);

	const bool rx_sync             = (interrupt_bits & BK4819_REG_02_FSK_RX_SYNC) ? true : false;
	const bool rx_fifo_almost_full = (interrupt_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) ? true : false;
	const bool rx_finished         = (interrupt_bits & BK4819_REG_02_FSK_RX_FINISHED) ? true : false;

#ifdef ENABLE_UART
	printf("\nMSG : S%i, F%i, E%i | %i", rx_sync, rx_fifo_almost_full, rx_finished, gFSKWriteIndex);
#endif

	if (rx_sync) {
		#ifdef ENABLE_MESSENGER_FSK_MUTE
			// prevent listening to fsk data and squelch (kamilsss655)
			// CTCSS codes seem to false trigger the rx_sync.
			// Only mute when the analog squelch is closed: on the BK4829 the
			// AFSK demod false-triggers rx_sync on real voice/noise, and muting
			// a genuinely-open signal both chops live audio and is the kind of
			// squelch-path interference GOGUFW avoids (it never touches the FSK
			// path while the channel is busy).
			if(gCurrentCodeType == CODE_TYPE_OFF && !g_SquelchLost)
				AUDIO_AudioPathOff();
		#endif
		gFSKWriteIndex = 0;
		MSG_ClearPacketBuffer();
		msgStatus = RECEIVING;
		gMsgRxTimeout10ms = 0;
	}

	if (rx_fifo_almost_full && msgStatus == RECEIVING) {

		const uint16_t count = BK4819_ReadRegister(BK4819_REG_5E) & (7u << 0);  // almost full threshold
		for (uint16_t i = 0; i < count; i++) {
			const uint16_t word = BK4819_ReadRegister(BK4819_REG_5F);
			if (gFSKWriteIndex < sizeof(dataPacket.serializedArray))
				dataPacket.serializedArray[gFSKWriteIndex++] = (word >> 0) & 0xff;
			if (gFSKWriteIndex < sizeof(dataPacket.serializedArray))
				dataPacket.serializedArray[gFSKWriteIndex++] = (word >> 8) & 0xff;
		}

		SYSTEM_DelayMs(10);

	}

	if (rx_finished) {
		// snapshot the signal strength of the packet just received (used by
		// the range-check PONG display)
		gMsgLastRxRssiDbm = BK4819_GetRSSI_dBm();
		// turn off green LED
		BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, 0);
		// do NOT clear g_SquelchLost here: the hardware sqlLost/sqlFound
		// interrupts own the squelch flag. Forcing it false could desync the
		// software squelch state from a still-open analog signal (sqlLost only
		// re-fires on a closed->open edge), which is the residual stuck-open
		// seen after a real RX. GOGUFW never writes g_SquelchLost.
		//
		// Re-arm the FSK engine only if the channel is already free: on a
		// real voice RX the AFSK demod fires phantom rx_finished too, and
		// OR-writing REG_59 (ClearFifo/EnableRx) under an open carrier is
		// exactly the FSK-chain-under-carrier manipulation that latches the
		// BK4829 squelch (the path the rc4 arm-gating missed). When busy,
		// defer to the pending-arm retry in MSG_CheckRxTimeout, which does a
		// full clean bring-up once the squelch closes.
		if (g_SquelchLost) {
			gMsgRxArmPending = true;
		} else {
			BK4819_FskClearFifo();
			BK4819_FskEnableRx();
		}
		msgStatus = READY;
		gMsgRxTimeout10ms = 0;

		if (gFSKWriteIndex > 2) {
			MSG_HandleReceive();
		}
		gFSKWriteIndex = 0;
	}
}

// called every 10ms from CheckRadioInterrupts(); recovers from a stuck
// FSK reception (sync detected but FSK_RX_FINISHED never arrives), which
// otherwise leaves msgStatus == RECEIVING and the squelch open forever
void MSG_CheckRxTimeout(void) {
#ifdef ENABLE_FMRADIO
	// while the broadcast FM radio is playing, the speaker is fed by the
	// BK1080 and the audio amp GPIO is ON: forcing the BK4819 AF path open
	// here (the idle AF=FM block below, whose "inaudible" premise assumes a
	// muted speaker) pumps the BK4819's demod noise straight into the FM
	// audio, and arming FSK / re-initing the AGC is pointless with the
	// receiver parked. Leave the chip alone; RADIO_SetupRegisters re-arms
	// the messenger when the FM radio exits.
	if (gFmRadioMode) {
		// ...but not touching it is not enough: on FM entry the AF is
		// typically ALREADY forced open by the idle block below, nobody
		// re-mutes it, and its demod noise (hiss) buries the broadcast
		// audio. Undo our own forcing while the FM radio plays normally
		// (skip while a VFO reception legitimately owns the speaker).
		if (gCurrentFunction == FUNCTION_FOREGROUND) {
			const uint8_t af = (BK4819_ReadRegister(BK4819_REG_47) >> 8) & 0xFu;
			if (af == BK4819_AF_FM)
				BK4819_SetAF(BK4819_AF_MUTE);
		}
		return;
	}
#endif
	// deferred ACK / PONG transmission (see MSG_HandleReceive). Listen-
	// before-talk: if the channel is busy when the countdown expires
	// (typically another radio's reply already in the air), re-draw a fresh
	// back-off instead of transmitting on top of it.
	if (gMsgAckCountdown10ms && --gMsgAckCountdown10ms == 0) {
		if (g_SquelchLost)
			gMsgAckCountdown10ms = 30 + MSG_Rand() % 120;  // busy: retry in 0.3..1.5s
		else
			MSG_SendAck();
	}

	if (gMsgPongCountdown10ms && --gMsgPongCountdown10ms == 0) {
		if (g_SquelchLost)
			gMsgPongCountdown10ms = 30 + MSG_Rand() % 120;  // busy: retry in 0.3..1.5s
		else
			MSG_SendPong();
	}

	// deferred FSK-RX arm (see gMsgRxArmPending in MSG_EnableRX): the arm was
	// requested while a carrier was open; retry as soon as the squelch has
	// closed. g_SquelchLost gates the (SPI) hardware re-check inside
	// MSG_EnableRX so the tick stays free of register reads while a signal
	// is being received.
	if (gMsgRxArmPending &&
	    gEeprom.MESSENGER_CONFIG.data.receive &&
	    gCurrentFunction != FUNCTION_TRANSMIT &&
	    !g_SquelchLost)
		MSG_EnableRX(true);

	// BK4829 quirk: many code paths (squelch setup, beeps, tones) leave the
	// AF path on AF_MUTE, which also silences the FM demod feeding the FSK
	// slicer - the modem then misses every packet until something reopens
	// the AF (e.g. monitor). While idle with messenger RX enabled, force the
	// AF path back to FM; the write is deduplicated by the driver cache and
	// is inaudible because the speaker amp GPIO stays off.
	if (gEeprom.MESSENGER_CONFIG.data.receive &&
	    gCurrentFunction == FUNCTION_FOREGROUND &&
	    msgStatus == READY) {
		const uint8_t af = (BK4819_ReadRegister(BK4819_REG_47) >> 8) & 0xFu;
		if (af == BK4819_AF_MUTE)
			BK4819_SetAF(BK4819_AF_FM);
	}

	// anti-drift: holding AF=FM continuously while idle (see above) lets
	// the BK4829's auto AGC drift its gain upward on noise over MINUTES
	// until the RSSI crosses the squelch-open threshold and the squelch
	// opens on nothing. An earlier fix pinned the AGC to fixed-gain mode
	// while idle - drift-proof, but it also capped the RX gain: a weak
	// signal arriving during idle never got the auto AGC ramp (and if it
	// stayed below the squelch threshold the un-pin never triggered), so
	// the K1 received measurably worse than the K5, which listens with
	// auto AGC. Instead keep the AGC in auto (full sensitivity, same as
	// the K5) and periodically do a FULL AGC re-init while genuinely idle:
	// unlike the old REG_7E bit15 "kick" (086ee4f, which only toggled the
	// fixed/auto mode bit), the SetAGC(false)+InitAGC+SetAGC(true) toggle
	// rewrites the gain table and restarts the loop from its baseline, so
	// the accumulated drift is discarded every 15s - far too short a
	// window for the minutes-scale drift to reach the squelch threshold.
	// Gated on genuine idle only (READY, squelch closed, FOREGROUND, FM):
	// any activity resets the countdown, so a reception in progress is
	// never disturbed. AM listening has its own AGC management (AM fix)
	// and is left alone. Worst case if drift ever slips through anyway:
	// the squelch opens on noise and the 5s recovery below closes it.
	{
		static uint16_t agcReinit10ms = 0;
		const bool idle = gEeprom.MESSENGER_CONFIG.data.receive &&
		                  gCurrentFunction == FUNCTION_FOREGROUND &&
		                  msgStatus == READY && !g_SquelchLost &&
		                  gRxVfo != NULL && gRxVfo->Modulation == MODULATION_FM;
		if (!idle) {
			agcReinit10ms = 0;
		} else if (++agcReinit10ms >= 1500) {  // every 15s of genuine idle
			agcReinit10ms = 0;
			BK4819_SetAGC(false);
			BK4819_InitAGC(false);
			BK4819_SetAGC(true);
		}
	}

	// recovery: BK4819_REG_02's sqlLost bit is level-triggered off RSSI vs
	// the REG_78 thresholds. Field testing (giu/lug 2026) proved that after
	// a real RX the squelch can latch open INSIDE the chip: rewriting the
	// squelch thresholds, the AGC table/mode, REG_58/59 and toggling the RX
	// DSP - everything RADIO_SetupRegisters does, and it runs on every
	// channel change - does NOT clear it; only the REG_00 soft-reset done
	// at power-up does (which is why only a power-cycle recovered). So if
	// the squelch reports "open" continuously for 5s while no FSK packet is
	// in progress, redo the chip's boot init (BK4819_Init = REG_00 soft
	// reset + stock register table) and then the full RX setup to re-tune
	// and re-arm everything. During a genuine long voice RX this costs a
	// brief audio hiccup every 5s (as the previous, weaker recovery already
	// did); in the latched state it recovers within 5s instead of needing a
	// power-cycle.
	// The counter must keep running while msgStatus == RECEIVING: in the
	// latched state the armed FSK engine can fire phantom fskRxSync
	// interrupts on noise, flipping msgStatus to RECEIVING for up to 1s at
	// a time (the RX timeout below), which would starve a READY-gated
	// counter almost indefinitely. So: count whenever the squelch reports
	// open; fire at 5s if no FSK packet is in progress, or force at 8s
	// regardless - after 8s of continuously-open squelch any genuine
	// packet (~1s at AFSK1200) finished long ago, so what looks like a
	// reception can only be the phantom kind.
	if (gEeprom.MESSENGER_CONFIG.data.receive &&
	    gCurrentFunction != FUNCTION_TRANSMIT) {
		static uint16_t squelchStuck10ms = 0;
		if (g_SquelchLost) {
			if (squelchStuck10ms < 800)
				squelchStuck10ms++;
			if ((squelchStuck10ms >= 500 && msgStatus == READY) ||
			    squelchStuck10ms >= 800) {
				squelchStuck10ms = 0;
#ifdef ENABLE_UART
				// diagnostic snapshot of the latched state, taken right before
				// the re-init wipes it: log with a serial terminal (38400 8N1,
				// Kenwood cable) to identify WHICH register actually latches
				printf("\nSQLSTUCK 02=%04X 0C=%04X 67=%04X 7E=%04X 58=%04X 59=%04X 70=%04X 72=%04X 30=%04X 47=%04X 3F=%04X",
				       BK4819_ReadRegister(BK4819_REG_02),
				       BK4819_ReadRegister(BK4819_REG_0C),
				       BK4819_ReadRegister(BK4819_REG_67),
				       BK4819_ReadRegister(BK4819_REG_7E),
				       BK4819_ReadRegister(BK4819_REG_58),
				       BK4819_ReadRegister(BK4819_REG_59),
				       BK4819_ReadRegister(BK4819_REG_70),
				       BK4819_ReadRegister(BK4819_REG_72),
				       BK4819_ReadRegister(BK4819_REG_30),
				       BK4819_ReadRegister(BK4819_REG_47),
				       BK4819_ReadRegister(BK4819_REG_3F));
#endif
				BK4819_Init();
				RADIO_SetupRegisters(false);
			}
		} else {
			squelchStuck10ms = 0;
		}
	}

	if (msgStatus != RECEIVING) {
		gMsgRxTimeout10ms = 0;
		return;
	}

	if (++gMsgRxTimeout10ms > MSG_RX_TIMEOUT_10MS) {
		gMsgRxTimeout10ms = 0;
		msgStatus = READY;
		gFSKWriteIndex = 0;
		// this timeout fires when a sync was detected but no packet followed -
		// i.e. a phantom sync from a real voice/noise carrier that is still
		// open. Re-arming the FSK engine right here would OR-write REG_59
		// under that carrier (the latch trigger); defer to the pending-arm
		// retry instead, which brings the engine up cleanly once the squelch
		// closes. When the channel is already free, re-arm directly.
		if (g_SquelchLost) {
			gMsgRxArmPending = true;
		} else {
			BK4819_FskClearFifo();
			BK4819_FskEnableRx();
		}
		// recovery only resets the messenger's own FSK state; the squelch flag
		// and green LED stay owned by the hardware sqlLost/sqlFound interrupts.
		// Forcing g_SquelchLost=false here could clear a legitimately-open
		// squelch mid-signal (a spurious rx_sync on a real >1s RX), desyncing
		// software/hardware squelch - the residual stuck-open-after-RX bug.
	}
}

void MSG_Init() {
	memset(rxMessage, 0, sizeof(rxMessage));
	memset(cMessage, 0, sizeof(cMessage));
	memset(lastcMessage, 0, sizeof(lastcMessage));
	hasNewMessage = 0;
	msgStatus = READY;
	prevKey = 0;
    prevLetter = 0;
	cIndex = 0;
	#ifdef ENABLE_ENCRYPTION
		// ENC_KEY only changes via CHIRP (followed by a reboot), so derive
		// the 256-bit session key once here instead of polling a flag
		CRYPTO_Generate256BitKey(gEeprom.ENC_KEY, gEncryptionKey, sizeof(gEeprom.ENC_KEY));
	#endif
	// personalize the jitter PRNG stream (see MSG_Rand): mix the station
	// callsign into the seed so identical radios never share delay sequences
	for (uint8_t i = 0; i < sizeof(gEeprom.CALLSIGN); i++)
		s_msgRng = (uint16_t)(s_msgRng * 31u) + (uint8_t)gEeprom.CALLSIGN[i];
	if (s_msgRng == 0)  // xorshift must never run on an all-zero state
		s_msgRng = 0xACE1;
	MSG_EnableRX(gEeprom.MESSENGER_CONFIG.data.receive);
}

void MSG_SendAck() {
	// in the future we might reply with received payload and then the sending radio
	// could compare it and determine if the messegage was read correctly (kamilsss655)
	MSG_ClearPacketBuffer();
	dataPacket.data.header = ACK_PACKET;
	// sending only empty header seems to not work, so set few bytes of payload to increase reliability (kamilsss655)
	memset(dataPacket.data.payload, 255, 5);
	MSG_SendPacket();
}

// fill the packet payload with our (sanitized) callsign so PING/PONG peers can
// identify us; placeholder '?' if no callsign is set (payload must be non-empty)
static void MSG_FillCallsignPayload(void) {
	uint8_t n = 0;
	for (uint8_t i = 0; i < sizeof(gEeprom.CALLSIGN); i++) {
		char c = gEeprom.CALLSIGN[i];
		if (c == 0 || (uint8_t)c == 0xFF || c < 0x20 || c > 0x7E)
			break;
		dataPacket.data.payload[n++] = c;
	}
	if (n == 0)
		dataPacket.data.payload[n++] = '?';
}

void MSG_SendPing(void) {
	// broadcast a range-check probe; nearby iu2vtm radios reply with a PONG
	MSG_ClearPacketBuffer();
	dataPacket.data.header = PING_PACKET;
	MSG_FillCallsignPayload();
	moveUP(rxMessage);
	snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "PING >");
	gUpdateDisplay = true;
	MSG_SendPacket();
}

static void MSG_SendPong(void) {
	// reply to a received PING with our callsign
	MSG_ClearPacketBuffer();
	dataPacket.data.header = PONG_PACKET;
	MSG_FillCallsignPayload();
	MSG_SendPacket();
}

void MSG_HandleReceive(){
	if (dataPacket.data.header == ACK_PACKET) {
	#ifdef ENABLE_MESSENGER_DELIVERY_NOTIFICATION
		#ifdef ENABLE_MESSENGER_UART
			UART_printf("SVC<RCPT\r\n");
		#endif
		rxMessage[MSG_LINES - 1][0] = '+';
		gUpdateStatus = true;
		gUpdateDisplay = true;
	#endif
	} else if (dataPacket.data.header == PING_PACKET) {
		// range check: someone is probing. Log who pinged, then schedule a
		// PONG reply with random jitter so multiple radios don't all answer
		// at the same instant.
		moveUP(rxMessage);
		snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "PING< %s", dataPacket.data.payload);
		if (gScreenToDisplay != DISPLAY_MSG) hasNewMessage = 1;
		gUpdateStatus = true;
		gUpdateDisplay = true;
		// window sized for the ~0.5s packet airtime (see MSG_Rand); the
		// listen-before-talk in MSG_CheckRxTimeout handles residual overlaps
		gMsgPongCountdown10ms = 50 + MSG_Rand() % 250;  // 0.5..3.0 s
		return;
	} else if (dataPacket.data.header == PONG_PACKET) {
		// a station answered our ping: show its callsign, estimated distance
		// (rough RSSI model) and the measured RSSI
		moveUP(rxMessage);
		{
			char ds[8];
			MSG_FormatDistance(ds, sizeof(ds), gMsgLastRxRssiDbm);
			snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "%s %s %d",
			         dataPacket.data.payload, ds, gMsgLastRxRssiDbm);
		}
		if (gScreenToDisplay != DISPLAY_MSG) hasNewMessage = 1;
		gUpdateStatus = true;
		gUpdateDisplay = true;
		return;
	} else {
		moveUP(rxMessage);
		if (dataPacket.data.header >= INVALID_PACKET) {
			snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "ERROR: INVALID PACKET.");
		}
		else
		{
			#ifdef ENABLE_ENCRYPTION
				if(dataPacket.data.header == ENCRYPTED_MESSAGE_PACKET)
				{
					CRYPTO_Crypt(dataPacket.data.payload,
						PAYLOAD_LENGTH,
						dataPacket.data.payload,
						&dataPacket.data.nonce,
						gEncryptionKey,
						256);
				}
				snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "< %s", dataPacket.data.payload);
			#else
				snprintf(rxMessage[MSG_LINES - 1], PAYLOAD_LENGTH + 2, "< %s", dataPacket.data.payload);
			#endif
			#ifdef ENABLE_MESSENGER_UART
				UART_printf("SMS<%s\r\n", dataPacket.data.payload);
			#endif
		}

		if ( gScreenToDisplay != DISPLAY_MSG ) {
			hasNewMessage = 1;
			gUpdateStatus = true;
			gUpdateDisplay = true;
	#ifdef ENABLE_MESSENGER_NOTIFICATION
			gPlayMSGRing = true;
	#endif
		}
		else {
			gUpdateDisplay = true;
		}
	}

	// Reply with an ACK after a delay so the correspondent radio can get
	// back to RX. Scheduled on the 10ms tick instead of blocking here:
	// a 700ms delay + full TX inside the interrupt-processing path froze
	// the UI before the received message was even drawn
	if (dataPacket.data.header == MESSAGE_PACKET ||
		dataPacket.data.header == ENCRYPTED_MESSAGE_PACKET)
	{
		if(gEeprom.MESSENGER_CONFIG.data.ack)
			// jittered: two radios acknowledging the same broadcast must not
			// key up together (the old fixed 700ms collided deterministically).
			// Min 0.5s keeps the original margin for the sender to re-arm its
			// RX after TX; listen-before-talk in MSG_CheckRxTimeout handles
			// residual overlaps.
			gMsgAckCountdown10ms = 50 + MSG_Rand() % 150;  // 0.5..2.0 s
	}
}

// ---------------------------------------------------------------------------------

void insertCharInMessage(uint8_t key) {
	if ( key == KEY_0 ) {
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = '0';
		} else {
			cMessage[cIndex] = ' ';
		}
		if ( cIndex < MSG_MsgInputMax() ) {
			cIndex++;
		}
	} else if (prevKey == key)
	{
		cIndex = (cIndex > 0) ? cIndex - 1 : 0;
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = T9TableNum[key - 1][(++prevLetter) % numberOfNumsAssignedToKey[key - 1]];
		} else if ( keyboardType == LOWERCASE ) {
			cMessage[cIndex] = T9TableLow[key - 1][(++prevLetter) % numberOfLettersAssignedToKey[key - 1]];
		} else {
			cMessage[cIndex] = T9TableUp[key - 1][(++prevLetter) % numberOfLettersAssignedToKey[key - 1]];
		}
		if ( cIndex < MSG_MsgInputMax() ) {
			cIndex++;
		}
	}
	else
	{
		prevLetter = 0;
		if ( cIndex >= MSG_MsgInputMax() ) {
			cIndex = (cIndex > 0) ? cIndex - 1 : 0;
		}
		if ( keyboardType == NUMERIC ) {
			cMessage[cIndex] = T9TableNum[key - 1][prevLetter];
		} else if ( keyboardType == LOWERCASE ) {
			cMessage[cIndex] = T9TableLow[key - 1][prevLetter];
		} else {
			cMessage[cIndex] = T9TableUp[key - 1][prevLetter];
		}
		if ( cIndex < MSG_MsgInputMax() ) {
			cIndex++;
		}

	}
	cMessage[cIndex] = '\0';
	if ( keyboardType == NUMERIC ) {
		prevKey = 0;
		prevLetter = 0;
	} else {
		prevKey = key;
	}
}

void processBackspace() {
	cIndex = (cIndex > 0) ? cIndex - 1 : 0;
	cMessage[cIndex] = '\0';
	prevKey = 0;
    prevLetter = 0;
}

void  MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
	uint8_t state = bKeyPressed + 2 * bKeyHeld;

	if (state == MSG_BUTTON_EVENT_SHORT) {

		switch (Key)
		{
			case KEY_0:
			case KEY_1:
			case KEY_2:
			case KEY_3:
			case KEY_4:
			case KEY_5:
			case KEY_6:
			case KEY_7:
			case KEY_8:
			case KEY_9:
				if ( keyTickCounter > NEXT_CHAR_DELAY) {
					prevKey = 0;
    				prevLetter = 0;
				}
				insertCharInMessage(Key);
				keyTickCounter = 0;
				break;
			case KEY_STAR:
				keyboardType = (KeyboardType)((keyboardType + 1) % END_TYPE_KBRD);
				break;
			case KEY_F:
				processBackspace();
				break;
			case KEY_UP:
				// scroll back into older history (disabled while editing callsign)
				if (!gMsgEditCallsign && gMsgScroll < MSG_LINES - MSG_VISIBLE_LINES)
					gMsgScroll++;
				gUpdateDisplay = true;
				break;
			case KEY_DOWN:
				// scroll toward the newest
				if (!gMsgEditCallsign && gMsgScroll > 0)
					gMsgScroll--;
				gUpdateDisplay = true;
				break;
			case KEY_MENU:
				if (gMsgEditCallsign)
					MSG_SaveCallsignEdit();   // save callsign
				else
					MSG_Send(cMessage);       // send message
				break;
			case KEY_EXIT:
				if (gMsgEditCallsign)
					MSG_ExitCallsignEdit();   // cancel callsign edit
				else
					gRequestDisplayScreen = DISPLAY_MAIN;
				break;

			default:
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				break;
		}

	} else if (state == MSG_BUTTON_EVENT_LONG) {

		switch (Key)
		{
			case KEY_F:
				MSG_Init();
				break;
			case KEY_UP:
				// hold UP = send a range-check ping (tap UP = scroll)
				if (!gMsgEditCallsign)
					MSG_SendPing();
				break;
			case KEY_DOWN:
				// hold DOWN = edit the station callsign (tap DOWN = scroll)
				if (!gMsgEditCallsign)
					MSG_EnterCallsignEdit();
				break;
			default:
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				break;
		}
	}

}

void MSG_ClearPacketBuffer()
{
	memset(dataPacket.serializedArray, 0, sizeof(dataPacket.serializedArray));
}

void MSG_Send(const char *cMessage){
	MSG_ClearPacketBuffer();
	#ifdef ENABLE_ENCRYPTION
		if(gEeprom.MESSENGER_CONFIG.data.encrypt)
		{
			dataPacket.data.header=ENCRYPTED_MESSAGE_PACKET;
		}
		else
		{
			dataPacket.data.header=MESSAGE_PACKET;
		}
	#else
		dataPacket.data.header=MESSAGE_PACKET;
	#endif
	// auto-prepend the station callsign ("CALLSIGN:msg") for ID compliance;
	// the combined string is truncated to PAYLOAD_LENGTH. Note: when the
	// message is encrypted the callsign is encrypted too, but encryption is
	// not permitted on amateur bands anyway (use clear text there).
	{
		char prefix[sizeof(gEeprom.CALLSIGN) + 2];
		uint8_t plen = MSG_BuildCallPrefix(prefix);
		memset(dataPacket.data.payload, 0, sizeof(dataPacket.data.payload));
		if (plen > 0)
			snprintf((char *)dataPacket.data.payload, sizeof(dataPacket.data.payload), "%s%s", prefix, cMessage);
		else
			memcpy(dataPacket.data.payload, cMessage, sizeof(dataPacket.data.payload));
	}
	MSG_SendPacket();
}

void MSG_ConfigureFSK(bool rx)
{
	// REG_70
	//
	// <15>   0 Enable TONE1
	//        1 = Enable
	//        0 = Disable
	//
	// <14:8> 0 TONE1 tuning gain
	//        0 ~ 127
	//
	// <7>    0 Enable TONE2
	//        1 = Enable
	//        0 = Disable
	//
	// <6:0>  0 TONE2/FSK tuning gain
	//        0 ~ 127
	//
	BK4819_WriteRegister(BK4819_REG_70,
		( 0u << 15) |    // 0
		( 0u <<  8) |    // 0
		( 1u <<  7) |    // 1
		(96u <<  0));    // 96

	// AFSK1200 only: FSK450/700 don't work reliably on the K1/BK4829 and were
	// removed. The modem is always configured for AFSK1200 (Tone2 = 1200 baud).
	switch(MOD_AFSK_1200)
	{
		case MOD_AFSK_1200:
			TONE2_FREQ = 12389u;
			break;
		case MOD_FSK_700:
			TONE2_FREQ = 7227u;
			break;
		case MOD_FSK_450:
			TONE2_FREQ = 4646u;
			break;
	}

	BK4819_WriteRegister(BK4819_REG_72, TONE2_FREQ);
	
	switch(MOD_AFSK_1200)   // AFSK1200 only (FSK450/700 removed)
	{
		case MOD_FSK_700:
		case MOD_FSK_450:
			BK4819_WriteRegister(BK4819_REG_58,
				(0u << 13) |		// 1 FSK TX mode selection
									//   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
									//   1 = FFSK 1200 / 1800 TX
									//   2 = ???
									//   3 = FFSK 1200 / 2400 TX
									//   4 = ???
									//   5 = NOAA SAME TX
									//   6 = ???
									//   7 = ???
									//
				(0u << 10) |		// 0 FSK RX mode selection
									//   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
									//   1 = ???
									//   2 = ???
									//   3 = ???
									//   4 = FFSK 1200 / 2400 RX
									//   5 = ???
									//   6 = ???
									//   7 = FFSK 1200 / 1800 RX
									//
				(3u << 8) |			// FSK RX gain (0~3): bumped to MAX. The
									//   K1/BK4829 is less sensitive in RX than
									//   the K5/BK4819 - with gain 0 it hears the
									//   carrier but never locks the FSK sync.
									//
				(3u << 6) |			// 3 ??? .. aircopy (the only FSK RX
									//   path verified working on the K1
									//   chip variant) sets this field to 3
									//   with RX gain 0 (REG_58 = 0x00C1)
									//
				(0u << 4) |			// 0 FSK preamble type selection
									//   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
									//   1 = ???
									//   2 = 0x55
									//   3 = 0xAA
									//
				(0u << 1) |			// 1 FSK RX bandwidth setting
									//   0 = FSK 1.2K .. no tones, direct FM
									//   1 = FFSK 1200 / 1800
									//   2 = NOAA SAME RX
									//   3 = ???
									//   4 = FSK 2.4K and FFSK 1200 / 2400
									//   5 = ???
									//   6 = ???
									//   7 = ???
									//
				(1u << 0));			// 1 FSK enable
									//   0 = disable
									//   1 = enable
		break;
		case MOD_AFSK_1200:
			BK4819_WriteRegister(BK4819_REG_58,
				(1u << 13) |		// 1 FSK TX mode selection
									//   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
									//   1 = FFSK 1200 / 1800 TX
									//   2 = ???
									//   3 = FFSK 1200 / 2400 TX
									//   4 = ???
									//   5 = NOAA SAME TX
									//   6 = ???
									//   7 = ???
									//
				(5u << 10) |		// FSK RX mode: 5 = the value the stock K1
									//   aircopy uses (REG_58=0x37C3, RX mode 5).
									//   Was 7 (FFSK 1200/1800 RX) = kamilsss655
									//   K5/BK4819 tuning. On the K1/BK4829 the
									//   factory-proven aircopy RX path uses 5.
									//   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX
									//   4 = FFSK 1200 / 2400 RX
									//   7 = FFSK 1200 / 1800 RX
									//
				(3u << 8) |			// 0 FSK RX gain
									//   0 ~ 3
									//
				(3u << 6) |			// 3 = aircopy field <7:6> (the only FSK RX
									//   path verified working on the K1/BK4829
									//   chip variant). Was 0 (K5 tuning).
									//   0 ~ 3
									//
				(0u << 4) |			// 0 FSK preamble type selection
									//   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
									//   1 = ???
									//   2 = 0x55
									//   3 = 0xAA
									//
				(1u << 1) |			// 1 FSK RX bandwidth setting
									//   0 = FSK 1.2K .. no tones, direct FM
									//   1 = FFSK 1200 / 1800
									//   2 = NOAA SAME RX
									//   3 = ???
									//   4 = FSK 2.4K and FFSK 1200 / 2400
									//   5 = ???
									//   6 = ???
									//   7 = ???
									//
				(1u << 0));			// 1 FSK enable
									//   0 = disable
									//   1 = enable
		break;
	}

	// REG_5A .. bytes 0 & 1 sync pattern
	//
	// <15:8> sync byte 0
	// < 7:0> sync byte 1
	BK4819_WriteRegister(BK4819_REG_5A, 0x3072);

	// REG_5B .. bytes 2 & 3 sync pattern
	//
	// <15:8> sync byte 2
	// < 7:0> sync byte 3
	BK4819_WriteRegister(BK4819_REG_5B, 0x576C);

	// disable CRC
	BK4819_WriteRegister(BK4819_REG_5C, 0x5625);

	// set the almost full threshold
	if(rx)
		BK4819_WriteRegister(BK4819_REG_5E, (64u << 3) | (1u << 0));  // 0 ~ 127, 0 ~ 7

	// packet size .. sync + packet - size of a single packet

	uint16_t size = sizeof(dataPacket.serializedArray);
	// size -= (fsk_reg59 & (1u << 3)) ? 4 : 2;
	if(rx)
		size = (((size + 1) / 2) * 2) + 2;             // round up to even, else FSK RX doesn't work

	BK4819_WriteRegister(BK4819_REG_5D, (size << 8));
	// BK4819_WriteRegister(BK4819_REG_5D, ((sizeof(dataPacket.serializedArray)) << 8));

	// clear FIFO's
	BK4819_FskClearFifo();

	// configure main FSK params
	BK4819_WriteRegister(BK4819_REG_59,
				(0u        <<       15) |   // 0/1     1 = clear TX FIFO
				(0u        <<       14) |   // 0/1     1 = clear RX FIFO
				(0u        <<       13) |   // 0/1     1 = scramble
				(0u        <<       12) |   // 0/1     1 = enable RX
				(0u        <<       11) |   // 0/1     1 = enable TX
				(0u        <<       10) |   // 0/1     1 = invert data when RX
				(0u        <<        9) |   // 0/1     1 = invert data when TX
				(0u        <<        8) |   // 0/1     ???
				((rx ? 6u : 15u) <<  4) |   // 0 ~ 15  preamble length .. bit toggling
				                            // RX: 6 like aircopy on the K1 chip
				                            // (0 here may prevent bit-clock lock)
				(1u        <<        3) |   // 0/1     sync length
				(0u        <<        0)     // 0 ~ 7   ???
				
	);

	// clear interupts
	BK4819_WriteRegister(BK4819_REG_02, 0);
}

#endif
