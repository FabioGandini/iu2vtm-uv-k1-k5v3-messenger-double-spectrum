# uv-k1-k5v3-firmware-custom — note di progetto per Claude Code

Firmware custom per Quansheng UV-K1 (MCU PY32F071, RF chip BK4829 — variante del BK4819)
e UV-K5v3. Branch di lavoro: `feature_messenger` — port del modulo messenger FSK
dal firmware kamilsss655 (UV-K5) nella base armel UV-K1-K5v3.
Target/preset principale: `Custom` (EXE `iu2vtm`). Autore: Fabio, IU2VTM.

## Stato al 6 luglio 2026 (HEAD abac989, branch main)

- **TX messenger (K1→K5): funziona.** AFSK1200 only — FSK450/700 rimossi perché
  inaffidabili sul BK4829 (commit 5d33b35).
- **RX messenger sul K1: funziona** dopo la catena di fix di giugno 2026.
  Chiave: sul BK4829 `AF_MUTE` spegne anche il demodulatore FM che alimenta lo
  slicer FSK, quindi in idle l'AF viene forzato su `AF_FM` (inudibile: il GPIO
  dell'ampli speaker resta spento a squelch chiuso).
- **I due fix sotto sono COMMITTATI il 6 lug 2026**: Fix 1 = `f77324d`,
  Fix 2 = `abac989`. NB: il patch originale (`fix_fm_e_squelch_agc.patch`,
  scritto il 2 lug contro `5d33b35` in un altro checkout) NON applicava più
  su main: nel frattempo main aveva la linea alternativa 24-26 giu
  (`39333c4`…`1fbe15a`, "togli AF=FM + reset FSK") mai validata sul campo.
  Il 6 lug la strategia del 2 lug (AF=FM tenuto + pin AGC) è stata riportata
  SOPRA quella linea: restano validi `1557901` (drop write-cache REG_30/47,
  difetto reale), `5efdeb3` (timer RX), `942d5e2` (spectrum AM) e il reset
  REG_58/59 in `MSG_EnableRX` (`1fbe15a`, igiene motore FSK, compatibile).
  Il blocco "GOGUFW-principle, niente AF=FM" è stato rimosso: contraddiceva
  la verifica hardware dell'11 giu (senza AF=FM il modem è sordo a squelch
  chiuso). Build post-commit: FLASH 82.75% (99988 B), RAM 82.03%, zero
  warning. `fix_fm_e_squelch_agc.patch` e il bin
  `iu2vtm_K1_FM+fix-squelch-AGC_5d33b35.bin` sono SUPERATI (storici).
- **DA FARE: test hardware** della build `abac989` (vedi "Bug aperti").

## Fix 1 — FM broadcast sparita dal build Custom

**Sintomo:** l'app FM commerciale (chip BK1080) non esiste nel firmware flashato;
il tasto FM non fa nulla.

**Causa:** il commit upstream `fd35d21` (muzkr, "Re-structure source code",
ott 2025) ha introdotto `"ENABLE_FMRADIO": false` nel preset `default` di
`CMakePresets.json`. Il preset `Custom` eredita da `default`
(`"inherits": "default"`) e non ridefiniva il flag → FM silenziosamente esclusa.

**Fix:** aggiungere `"ENABLE_FMRADIO": true` nei `cacheVariables` del preset
`Custom`. Verificato: con FM + messenger la flash è al 83.33% (100692 B / 118 KB),
RAM 82% — nessun overflow.

## Fix 2 — Squelch che si incastra aperto in idle (messenger RX attivo)

**Sintomo:** dopo minuti di idle con messenger receive abilitato, lo squelch si
apre sul rumore e resta incastrato (fruscio), anche cambiando canale; in
precedenza anche false detection DTMF.

**Causa radice:** l'AF tenuto permanentemente su FM (necessario per lo slicer
FSK, vedi sopra) mantiene vivo il demodulatore sul rumore. L'AGC automatico del
BK4829 deriva il guadagno verso l'alto finché il rumore amplificato supera la
soglia squelch (REG_78) → interrupt `sqlLost` → squelch aperto sul nulla.

**Perché il vecchio tampone non funzionava** (commit `086ee4f`, "periodic AGC
kick"): il toggle `SetAGC(false)+SetAGC(true)` commuta solo il bit 15 di REG_7E
(fixed/auto) e **non azzera il guadagno accumulato dal loop AGC** — tornando in
auto riparte da dov'era derivato. In più il kick era condizionato a
`!g_SquelchLost`, quindi si fermava esattamente quando lo squelch si bloccava,
lasciando solo il fallback pesante da 5 s (raffiche di fruscio ricorrenti).

**Fix applicato** in `App/app/messenger.c`, funzione `MSG_CheckRxTimeout()`
(tick da 10 ms): sostituito il kick con un **pin dell'AGC in modalità guadagno
fisso** (fix index 3 in REG_7E — è il default di boot del chip, preloadato da
`BK4819_Init` con REG_7E=0x303E) finché il ricevitore è genuinamente in idle:
`msgStatus == READY && !g_SquelchLost && FUNCTION_FOREGROUND && FM`. A guadagno
costante la deriva è fisicamente impossibile. Su attività reale (INCOMING =
valutazione CTCSS, RECEIVE, MONITOR, o squelch aperto) si ripristina l'auto.
Lo stato desiderato è **riasserito a ogni tick** invece di essere cacheato in
un flag: `BK4819_SetAGC()` legge REG_7E e ritorna subito se già nello stato
richiesto (1 lettura registro/10 ms, stesso costo del check AF adiacente), il
che rende il fix immune a qualunque altro codice che riscriva REG_7E alle sue
spalle (`RADIO_SetupAGC` ha una cache propria e può rimettere auto senza
avvisare). Su AM non si tocca nulla (AM fix). Il fallback 5 s è rimasto come
rete di sicurezza.

**Verifiche fatte (review intensiva, 2 lug 2026):** dedup di SetAGC corretta
in entrambe le direzioni; unico writer runtime concorrente di REG_7E bit15 è
RADIO_SetupAGC (coperto dal re-assert per tick; ExitBypass tocca solo i bit
5:3, 0x303E solo al boot); nessun conflitto con il lockAGC dello spectrum
(il suo loop bloccante non esegue APP_TimeSlice10ms → il tick messenger non
gira durante lo spectrum — catena verificata: APP_TimeSlice10ms →
CheckRadioInterrupts → MSG_CheckRxTimeout); timing sicuro per l'RX FSK: il
preambolo TX è 15 byte ≈ 100 ms a 1200 baud, l'un-pin avviene entro 10 ms da
sqlLost → ~90 ms di margine prima del sync word; POWER_SAVE non scrive (il
chip resta fisso anche in sleep = anti-drift pure lì); disattivando il
messenger receive dal menu l'auto viene riasserito (nessun rischio di restare
inchiodati a gain fisso); zero warning di compilazione.

## Bug aperti / punti di attenzione

- **Test su campo del Fix 2**: verificare che lo squelch non si incastri più
  dopo idle prolungato (>10 min) e che l'RX messenger K5→K1 funzioni ancora a
  distanza/segnale debole.
- Il vecchio kick è stato rimosso, non commentato: la storia è in git
  (`086ee4f` per il tampone, questo patch per la sostituzione).
- `RADIO_SetupAGC()` in `App/radio.c` ha una cache statica (`lastSettings`):
  non chiamarla per "ripristinare" l'AGC dal messenger, potrebbe fare
  early-return. Il messenger gestisce il pin/unpin direttamente via
  `BK4819_SetAGC()` (che deduplica internamente leggendo REG_7E).

## Build

Toolchain senza Docker (il registry ghcr.io/armel può essere irraggiungibile):

```bash
apt install gcc-arm-none-eabi libnewlib-arm-none-eabi \
            libstdc++-arm-none-eabi-newlib cmake ninja-build
cmake --preset Custom
cmake --build --preset Custom -j
# output: build/Custom/iu2vtm.bin (raw, ~100 KB)
```

In alternativa: `./compile-with-docker.sh Custom` (toolchain ARM 13.3.rel1
ufficiale; la 13.2.rel1 dei repo Ubuntu è equivalente per Cortex-M0+).

## Flash

UVTools2 (https://armel.github.io/uvtools2/), WebSerial da Chrome/Edge desktop:
radio in DFU mode, selezionare il `.bin` raw — il packing lo fa UVTools2.
Buona pratica: dump della calibrazione prima di riflashare.
Cavo Kenwood, UART 38400 baud.
