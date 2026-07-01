ESP32 Ademco Keypad (Address 31) Bus Interface — Non-Addressable Panels
Interface legacy, non-addressable Ademco/Honeywell alarm panels (Keypad Address 31) with Home Assistant via an ESP32 — without altering your alarm's hardware security.
Unlike modern Vista panels (which use ECP addressable keypads), older generations drive a continuous 2400-baud keybus that projects like AlarmDecoder or Envisalink cannot read. This firmware reads that bus directly and reconstructs the full panel state from the three keybus wires.
> ⚠️ **Status:** actively developed and **fully tested on an Ademco 4120 with a 4127 fixed-word (fixed-English) keypad.** Other panels/keypads are listed as *likely* compatible but are **untested** — see the compatibility and protocol-landscape sections below.
---
📑 Table of contents
Which quadrant are you in?
Features
How it works — the polling trick
Key protocol discoveries
Cross-check against gregrenda's mapping
Keypad-specific mapping (read before porting)
Hardware
Powering the ESP32
Compatibility
Limitations / to-do
License
Credits
---
🧭 Which quadrant are you in? (protocol landscape)
Ademco non-addressable panels speak (at least) two different dialects on the same physical bus, depending on the keypad type in use. This matters more than the panel model:
	Fixed-word keypad (4127, 4137, 6128)	Alphanumeric keypad (6139, 6160, 5330)
How state is sent	encoded in status bits (LED words)	sent as display text to be parsed
This project	✅ covers this	❌ not this
gregrenda/ademco	partial	✅ text-parsing approach
AlarmDecoder / VistaECP	❌ (addressable ECP only)	❌ (addressable ECP only)
This project fills the fixed-word / non-addressable quadrant, which existing tools don't cover. If you have an alphanumeric keypad, the method here still helps, but the specific byte values won't match — see gregrenda's project for the text-parsing route.
> **Official confirmation:** the Ademco 4120XM program sheet states the panel supports **6128/6150** keypads and explicitly **does not support 6139/6160 alphanumeric** keypads — the same fixed-word vs alphanumeric split this project is built around. The keypad address procedure documented by Ademco sets the keypad to **address 31**, i.e. the non-addressable mode targeted here.
---
✨ Features
Exposed to Home Assistant:
Arming state: disarmed / armed total / armed partial (Stay)
MAX and INSTANT modes correctly recognised (not mistaken for an alarm)
Alarm detection, with a robustness filter that never suppresses a real alarm
Triggered zone on alarm: the zone that set off the alarm is decoded and named, using a voting window for reliability
Open-zone scan on demand (simulates the `*` keypress)
Chime, Ready / Not Ready, Bypass (zone excluded) status
Mains (AC 220 V) presence — empirical bit, see notes
Low-battery flag (see the important caveat below)
Named zones instead of bare numbers (e.g. "TAPPARELLE PT" instead of "4")
A custom Lovelace keypad card (mobile-friendly) mirroring the physical keypad
---
🚀 How it works — the polling trick
Legacy Address-31 panels do not broadcast individual zone states in real time; they only show open zones on the keypad display when a user interacts with it. To read open zones on demand, the firmware:
On request from Home Assistant, briefly simulates a press of the `*` key.
The panel forces a display refresh, cycling the open zones onto the bus.
The firmware decodes each open zone from the separator byte that precedes the `0x0C` status frame, using `zone = (0xFC − separator) / 8`.
After a fixed 14-second window the scan closes automatically and the firmware exits display mode by sending `code + 1`, returning the panel to idle.
The triggered-zone-on-alarm feature uses the same separator decoding, read automatically while an alarm is active — no keypress needed.
Why a voting window for the alarm zone?
During an alarm the separator can cycle and occasionally include a spurious zone. Instead of trusting a single frame, the firmware collects separators over a 4-second window and publishes the most frequent zone (ties go to the first seen). This is robust against a single misread frame — important on an ESP32 synchronising on a hardware clock pin.
---
🧠 Key protocol discoveries (Ademco 4120 / 4127)
All values were captured empirically on a real bus.
Status frame `0x0C`
State	B0	B1	B2	B3	Read as
Armed total / instant / partial	`0x9B`	`0x6C`	`0xD6`	`0x5D`	armed
MAX	`0x19`	`0xDF`	`0xD6`	`0xD6`	armed (NOT alarm)
ALARM (from any state)	`0x1F`	`0x16`	`0xDF`	`0x2B`	alarm
Important: total, instant and partial share the same `0x0C` frame. The total/partial distinction comes from bit6 of the second `0x04` frame, not from `0x0C`. MAX is the only armed state with a different frame, and it sets B3 bit1 (`0x02`) — which naively looks like an alarm.
Alarm vs MAX discrimination
Condition	B3 bit1 (`0x02`)	B3 bit7 (`0x80`)	Meaning
Normal armed states	0	0	no alarm
MAX armed	1	1	MAX, not alarm
Real alarm (even from MAX)	1	0	ALARM
```c
// The rule that separates MAX from a genuine alarm:
alarm = ((b3 & 0x02) != 0) && ((b3 & 0x80) == 0);
```
B3 bit7 is the INSTANT bit (confirmed by gregrenda's mapping — see below). MAX = "armed away + instant", so it sets bit7; a genuine alarm always produces `0x2B` (bit7 = 0) regardless of the starting state — confirmed by triggering a real alarm while armed in MAX. The rule therefore excludes MAX without ever suppressing a real alarm.
> MAX also lights the **NIGHT** bit (B1 bit4): in the MAX frame `B1 = 0xDF` (bit4 = 1), while a normal armed state has `B1 = 0x6C` (bit4 = 0). This gives an optional redundant marker for MAX.
Zone mapping (separator before `0x0C`)
Separator	Zone	Example name
`0xF4`	1	BASCULANTI
`0xEC`	2	PORTA INGRESSO
`0xE4`	3	VOLUMETRICO PT
`0xDC`	4	TAPPARELLE PT
`0xD4`	5	VOLUMETRICO 1P
`0xCC`	6	TAPPARELLE 1P
`0xC4`	7	TAMPER
`0xBC`	8	TAMPER COMB
> Passive zone reading (without `*`) is **not** reliable: with everything closed, the separator cycles on its own and would report false open zones. Always use the `*` scan or read during an actual alarm.
Mains (AC 220 V) presence — empirical
Candidate bit: B2 bit2 (`0x04`), active-high on mains present. Found by elimination — it is the only bit that stays `1` across every captured frame (armed / disarmed / alarm / MAX / partial, plus status frames closed / open / bypass), consistent with mains being present throughout every capture. gregrenda places AC on B3 bit3, but on the 4127 that position is used for something else (it would read "mains absent in MAX", which is nonsense), so his AC bit is 6150-specific. Verify by briefly disconnecting 220 V at the board and watching this bit drop to 0.
Low-battery flag — with a caveat
Read from B2 bit6 (`0x40`), active-low, in the status frame. This position is confirmed by gregrenda's mapping (`bat = B2 bit6`), an independent cross-check.
> ⚠️ **Do not fully trust the low-battery flag.** On these older Ademco panels it is set only when a periodic, weak load-test fails — it flags a *dead/disconnected* battery, not a *degrading* one. A tired battery can pass the test yet fail to hold up the system during a real blackout. Treat this flag as a **fault indicator** (something is clearly wrong when it fires), not as a battery health gauge. For real health, measure battery voltage directly.
---
🔬 Cross-check against gregrenda's 6150 mapping
gregrenda/ademco reverse-engineered the same non-addressable bus but for alphanumeric 6150 keypads (parsing display text). Converting his bit constants to byte.bit positions and applying them to the 4127 frames gives a very informative split:
Signal	This project (4127)	gregrenda (6150)	Match
ALARM	B3 bit1 (`0x02`)	B3 bit1 (`0x02`)	✅ identical
INSTANT (= MAX marker)	B3 bit7 (`0x80`)	B3 bit7 (`0x80`)	✅ identical
STAY / ARMED	B2 bit7 (`0x80`)	B2 bit7 (`0x80`)	✅ identical
BAT (low battery)	B2 bit6 (`0x40`)	B2 bit6 (`0x40`)	✅ identical
CHIME	(4127-specific)	B3 bit5 (`0x20`)	❌ diverges
BYPASS	(4127-specific)	B3 bit4 (`0x10`)	❌ diverges
READY	(4127-specific)	B2 bit4 (`0x10`)	❌ diverges
AC (mains)	B2 bit2 (`0x04`) (candidate)	B3 bit3 (`0x08`)	❌ diverges
The four "deep" state bits (ALARM, INSTANT, STAY, BAT) match exactly — these are keypad-independent and the most trustworthy. The diverging ones (CHIME, BYPASS, READY, AC) are exactly the bits gregrenda marks as "6150 only": the panel drives them differently for a fixed-word keypad. Timing also converges independently: gregrenda uses `FRAME_MS = 176` with a sync threshold of `(176−5) = 171 ms`, identical to this firmware's `SYNC_MIN_US = 171000`.
---
🔀 Keypad-specific mapping (read this before porting)
The byte mappings here were reverse-engineered with a 4127 fixed-word keypad. The panel adapts its bus output to the keypad type, so:
4137 — sister of the 4127 (same fixed-word display, just backlit + larger). Almost certainly uses the same frames; likely works unmodified, but unconfirmed.
6128 — fixed-word, same family; the panel's own program sheet groups 6128/6150 together for addressing.
6139 / 6160 / 5330 (alphanumeric) — state is sent as display text, not these bits. The method transfers, but the constants do not. See gregrenda's text-parsing approach.
If you have a different keypad, capture a bus log with `diagnostic_mode: true` — the approach still works, only the constants change. PRs adding a keypad column are very welcome (see below).
---
🛠️ Hardware
Item	Detail
MCU	ESP32 NodeMCU-32S
RX	GPIO21 (inverted, via 4N35 optocoupler)
TX	GPIO22 (RMT, key transmission)
Sync	GPIO35 (inverted)
Bus	2400 baud, 8E2
An optocoupler on the RX line safely level-shifts and isolates the keybus from the ESP32. The bus interface circuit is inspired by gregrenda's optocoupler approach, redrawn here for this wiring.
---
🔌 Powering the ESP32
Two options, with real trade-offs:
A) Separate USB power supply (simplest, safest). No extra load on a 30-year-old board, no switching noise on the keybus. Downside: if mains fails, the ESP32 dies exactly when the panel goes to battery — but if you already monitor mains elsewhere in HA, that's covered.
B) From the panel's 12 V (keeps the ESP32 alive on the panel's battery during a blackout). The 4120XM aux output is rated 700 mA total (official spec) — subtract whatever your sensors already draw and check the margin; the ESP32 pulls roughly 100–160 mA at 12 V at WiFi peak via a buck converter. On an old board, protect it:
Reverse-polarity diode in series (a plain `1N4007` is fine — 1 A / 1000 V, wildly over-spec here; the ~0.7 V drop is irrelevant because the buck regulates anyway).
Reservoir electrolytic (470–1000 µF) on the buck's 12 V input — a local buffer that absorbs the ESP's current peaks so they aren't demanded from the panel's aged supply.
Inline fuse (200–300 mA) so an ESP fault never takes down the panel.
A good low-ripple buck, not the cheapest module, to avoid injecting switching noise onto the 12 V line.
> The reference battery for these panels is a **12 V 7 Ah** SLA (Ademco "UB1270"). A fresh 7 Ah easily covers the panel + a few sensors + the ESP32 through a normal blackout.
---
📡 Compatibility
Tested: Ademco 4120 with 4127 fixed-word keypad.
Likely compatible but UNTESTED — feedback and logs very welcome:
Ademco 4110, 4110XM, 4110DL
Ademco 4120EC, 4120XM, 4140XMP
Ademco 5110XM
Vista-10 / Vista-10SE
Vista-15 (early models)
Vista-20 / Vista-20SE in Address-31 mode
...with a fixed-word keypad (4127 / 4137 / 6128). With an alphanumeric keypad the byte values will differ — see the keypad section.
Discoverability keywords (so people with this problem can find the project): non-addressable Ademco, Address 31, Ademco 4110/4120/4120XM Home Assistant, Vista-10 Vista-10SE Home Assistant, AlarmDecoder alternative for older panels, fixed-word keypad 4127 6128, ESPHome Ademco keybus.
If you try this on a panel/keypad not in the tested list, please open an issue with a bus log — your data helps map other models.
---
🤝 Contributing (help build the "universal" map)
There is no single universal decode for these panels — each keypad type is a column in a table that only gets filled by people with different hardware. This repo is meant to be that collection point:
Flash the firmware with `diagnostic_mode: true`.
Exercise your system (arm total / stay / max, trigger a test alarm, open zones).
Capture the raw frames from the log.
Open an issue or PR with your panel model + keypad model + frames.
Cheap second-hand boards (4110/4120 series) are widely available and make a safe bench rig for this — no need to experiment on your live panel.
---
🚧 Known limitations / to-do
Mains (AC) bit is an empirical candidate (B2 bit2) pending a physical 220 V-disconnect test.
Low-battery flag is a coarse fault indicator, not a health gauge (see caveat).
Compatibility beyond the 4120/4127 is theoretical.
Zone names are hard-coded (edit the `ZONE_NAMES` array to match your installation).
Firmware comments/logs are in Italian; the protocol documentation (this README + the protocol PDF) is in English.
---
📄 License
Released under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0).
✅ Free for personal/home use — replicate, modify and use on your own system.
✅ Share and adapt — with attribution, under the same license.
❌ No commercial use — you may not sell pre-assembled modules nor commercialise this software or forks of it.
Full text: https://creativecommons.org/licenses/by-nc-sa/4.0/
> Creative Commons licenses were designed for creative works rather than software, but are used here intentionally: the goal is a clear non-commercial restriction, which standard OSS licenses (MIT, GPL, etc.) do **not** provide — the GPL in particular explicitly permits commercial sale.
---
🙏 Credits
Protocol reverse-engineering builds on insights from gregrenda/ademco, whose work on non-addressable Ademco panels (alphanumeric-keypad, text-parsing approach) provided cross-checks for several state bits and the bus timing, and inspired the optocoupler bus-interface circuit. This project extends that work into the fixed-word keypad case through empirical bus capture on a 4120 / 4127.
Panel behaviour cross-referenced against Ademco's own 4120XM program sheet and user documentation (keypad families, Address-31 procedure, 700 mA aux rating, NO AC / BAT semantics, 12 V 7 Ah battery).
