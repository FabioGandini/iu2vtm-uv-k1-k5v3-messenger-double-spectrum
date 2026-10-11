# IU2VTM firmware for the UV-K1 and UV-K5 V3

##########################################
COMPATIBLE WITH LABS APPS (OPEN WITH F+7)
MESSENGER (LONG PRESS MENU)
SPECTRUM F4HWN ( F+5)
SPECTRUM KAMILL (F+8)
###########################################


This is the **IU2VTM** fork of the [armel/muzkr UV-K1/K5V3 firmware](https://github.com/armel/uv-k1-k5v3-firmware-custom)
(F4HWN, itself derived from Egzumer's UV-K5 firmware). It adds a text
**messenger** (with callsign ID, delivery ACK, range-check ping and optional
encryption), a second **spectrum** analyzer with channel-scan, and a handful
of other extras layered on top of the upstream project below -- **every
IU2VTM-specific feature is explained simply (ELI5) in its own section**,
["IU2VTM fork additions"](#iu2vtm-fork-additions), near the end of this file.

**Download:** grab the latest `iu2vtm.bin` from the
[Releases](https://github.com/FabioGandini/iu2vtm-uv-k1-k5v3-messenger-double-spectrum/releases)
page of this fork.

For everything else (F4HWN menus, Multiboot, scan lists, overlay apps,
AirCopy, power settings, and so on) the rest of this document is armel's own
upstream README, kept in full below.


### IU2VTM-specific credits

Parts of the BK4829 Messenger RX/re-arm strategy and channel-busy handling were
informed by [GOGUFW](https://github.com/Gogu-Qs/GOGUFW-UV-K1-Messenger).
GOGUFW's earlier Messenger and multi-radio Range Check implementation also
served as a reference during development. The messenger, crypto and
channel-scan spectrum build on work by
[@joaquimorg](https://github.com/joaquimorg) and
[@kamilsss655](https://github.com/kamilsss655); see the upstream Credits below
for everyone else.

## Credits

Many thanks to various people:

* [Muzkr](https://github.com/muzkr)
* [Mrkusypl](https://github.com/mrkusypl)
* [Andrej](https://github.com/Tunas1337)
* [Egzumer](https://github.com/egzumer)
* [OneOfEleven](https://github.com/OneOfEleven)
* [DualTachyon](https://github.com/DualTachyon)
* [Mikhail](https://github.com/fagci)
* [Manuel](https://github.com/manujedi)
* @wagner
* @Lohtse Shar
* [@Matoz](https://github.com/spm81)
* @Davide
* @Ismo OH2FTG
* [OneOfEleven](https://github.com/OneOfEleven)
* @d1ced95
* and others I forget

## License

Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
