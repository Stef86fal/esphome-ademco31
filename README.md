# ESP32 Ademco Keypad (Address 31) Bus Interface — Non-Addressable Panels

Interface legacy, non-addressable Ademco/Honeywell alarm panels (keypad **Address 31**) with **Home Assistant** via an ESP32 — **without altering your alarm's hardware security**.

Unlike modern Vista panels (which use ECP addressable keypads), older generations drive a continuous **2400-baud** keybus that projects like AlarmDecoder or Envisalink cannot read. This firmware reads that bus directly and reconstructs the full panel state from the three keybus wires.

> ⚠️ **Status:** actively developed and **fully tested on an Ademco 4120 with a 4127 fixed-word keypad.** Firmware is at **v19**, which handles mains-loss (blackout) behaviour correctly. Other panels/keypads are listed as *likely* compatible but are **untested** — see the compatibility and protocol-landscape sections.

---

## 📑 Table of contents

- [Which quadrant are you in?](#-which-quadrant-are-you-in-protocol-landscape)
- [Features](#-features)
- [How it works — the polling trick](#-how-it-works--the-polling-trick)
- [Key protocol discoveries](#-key-protocol-discoveries-ademco-4120--4127)
- [Mains loss (blackout) handling](#-mains-loss-blackout-handling-v19)
- [Why B0 is not used for state](#-why-b0-the-display-byte-is-not-used-for-state)
- [Cross-check against gregrenda](#-cross-check-against-gregrendas-6150-mapping)
- [Keypad-specific mapping](#-keypad-specific-mapping-read-this-before-porting)
- [Hardware](#-hardware)
- [Powering the ESP32](#-powering-the-esp32)
- [Compatibility](#-compatibility)
- [Contributing](#-contributing-help-build-the-universal-map)
- [Limitations / to-do](#-known-limitations--to-do)
- [License](#-license)
- [Credits](#-credits)

---

## 🧭 Which quadrant are you in? (protocol landscape)

Ademco non-addressable panels speak (at least) **two different dialects** on the same physical bus, depending on the **keypad type** in use. This matters more than the panel model:

| | Fixed-word keypad (4127, 4137, 5137, 6128) | Alphanumeric keypad (6139, 6160, 5330) |
|---|---|---|
| How state is sent | encoded in status bits (LED/segment words) | sent as display text to be parsed |
| This project | yes, covers this | no |
| gregrenda/ademco | partial | yes, text-parsing approach |
| AlarmDecoder / VistaECP | no (addressable ECP only) | no (addressable ECP only) |

This project fills the **fixed-word / non-addressable** quadrant, which existing tools don't cover. If you have an alphanumeric keypad, the *method* here still helps, but the specific byte values won't match — see [gregrenda's project](https://github.com/gregrenda/ademco) for the text-parsing route.

> **Official confirmation:** the Ademco 4120XM program sheet states the panel supports 6128/6150 keypads and explicitly does **not** support 6139/6160 alphanumeric keypads — the same fixed-word vs alphanumeric split this project is built around. The keypad address procedure documented by Ademco sets the keypad to **address 31**, i.e. the non-addressable mode targeted here.

---

## ✨ Features

Exposed to Home Assistant:

- **Arming state**: disarmed / armed total / armed partial (Stay)
- **MAX and INSTANT** modes correctly recognised (not mistaken for an alarm)
- **Alarm** detection, with a robustness filter that never suppresses a real alarm
- **Triggered zone on alarm**: the zone that set off the alarm is decoded and named, using a voting window for reliability
- **Open-zone scan** on demand (simulates the `*` keypress)
- **Chime**, **Ready / Not Ready**, **Bypass (zone excluded)** status
- **Mains (AC 220 V) presence** — detected via the dedicated "NO AC" bus frame, with hysteresis
- **Low-battery** flag (see the important caveat below)
- **Named zones** instead of bare numbers (e.g. "TAPPARELLE PT" instead of "4")
- A custom **Lovelace keypad card** (mobile-friendly) mirroring the physical keypad, with RETE (mains) and BATT badges

---

## 🚀 How it works — the polling trick

Legacy Address-31 panels do **not** broadcast individual zone states in real time; they only show open zones on the keypad display when a user interacts with it. To read open zones on demand, the firmware:

1. On request from Home Assistant, briefly simulates a press of the `*` key.
2. The panel forces a display refresh, cycling the open zones onto the bus.
3. The firmware decodes each open zone from the **separator byte** that precedes the `0x0C` status frame, using `zone = (0xFC − separator) / 8`.
4. After a fixed **14-second** window the scan closes automatically and the firmware exits display mode by sending `code + 1`, returning the panel to idle.

The triggered-zone-on-alarm feature uses the **same separator decoding**, read automatically while an alarm is active — no keypress needed.

### Why a voting window for the alarm zone?

During an alarm the separator can cycle and occasionally include a spurious zone. Instead of trusting a single frame, the firmware collects separators over a **4-second window** and publishes the **most frequent** zone (ties go to the first seen). This is robust against a single misread frame — important on an ESP32 synchronising on a hardware clock pin.

---

## 🧠 Key protocol discoveries (Ademco 4120 / 4127)

All values captured empirically on a real bus. Frames are 4 bytes (B0-B3) preceded by a separator byte. **State is read from B1/B2/B3 and the status frame; B0 is the display byte and is not used for state** (see its own section below).

### Status frame `0x0C` — state matrix (with and without mains)

| State | Mains | B0 | B1 | B2 | B3 | Read as |
|---|---|---|---|---|---|---|
| Total / Instant | ON | 9B | 6C | D6 | 5D | armed_total |
| Partial (Stay) | ON | 9B | 6C | D6 | 5D | armed_partial * |
| Total / Partial | OFF | 5B | 6C | D6 | 5D | same state, only B0 differs |
| MAX | ON | 19 | DF | D6 | D6 | armed (B3 bit1+bit7) |
| ALARM (from any state) | ON | 1F | 16 | DF | 2B | alarm |
| Disarmed, chime ON | ON | D6 | CD | 17 | 6C | disarmed |
| Disarmed, chime OFF | ON | 16 | CD | 17 | 6C | disarmed (only B0 differs) |
| NO-AC message frame | OFF | var | DF | 6C | 5E | mains absent (not a state) |

`*` Total vs Partial is **not** in the `0x0C` frame (identical): it comes from bit6 of the second `0x04` frame. **Key point:** in armed states B1/B2/B3 are identical with and without mains, so armed/alarm remain readable during a blackout.

### Alarm vs MAX discrimination

| Condition | B3 bit1 (0x02) | B3 bit7 (0x80) | Meaning |
|---|---|---|---|
| Normal armed | 0 | 0 | no alarm |
| MAX armed | 1 | 1 | MAX, not alarm |
| Real alarm | 1 | 0 | ALARM |

```c
// Separates MAX from a genuine alarm; never suppresses a real alarm:
alarm = ((b3 & 0x02) != 0) && ((b3 & 0x80) == 0);
```

B3 bit7 is the INSTANT bit. MAX = "armed away + instant", so it sets bit7; a genuine alarm always produces `0x2B` (bit7 = 0), confirmed by triggering a real alarm from MAX.

### Zone mapping (separator before `0x0C`)

| Separator | Zone | Example name |
|---|---|---|
| F4 | 1 | BASCULANTI |
| EC | 2 | PORTA INGRESSO |
| E4 | 3 | VOLUMETRICO PT |
| DC | 4 | TAPPARELLE PT |
| D4 | 5 | VOLUMETRICO 1P |
| CC | 6 | TAPPARELLE 1P |
| C4 | 7 | TAMPER |
| BC | 8 | TAMPER COMB |

Passive zone reading (without `*`) is **not** reliable: with everything closed, the separator cycles on its own. Always use the `*` scan or read during an actual alarm.

### State bit sources (reliable)

| Signal | Source | Logic |
|---|---|---|
| Armed | 0x0C, B2 bit7 (0x80) | active high |
| Total / Partial | second 0x04, bit6 | partial_seen |
| Alarm | 0x0C, B3 bit1 and not bit7 | see above |
| Ready | status frame, B2 bit1 (0x02) | active low |
| Bypass | status frame, B2 bit7 (0x80) | active low |
| Chime | status frame, B3 bit1 (0x02) | inverted when bypass active |
| Low battery | status frame, B2 bit6 (0x40) | active low |
| Mains 220 V | presence of NO-AC frame | see blackout section |

---

## 🔌 Mains loss (blackout) handling (v19)

Mains presence is **not** a status bit. When the 220 V fails, the panel **alternates** the real state frame with a dedicated message frame that turns off the RETE (mains) segment on the LCD. That message frame is recognised by:

```
B1 == 0xDF && B2 == 0x6C && B3 == 0x5E     (B0 ignored)
```

This is unique — the only other frame with B1=0xDF is MAX, which has B2/B3 = D6/D6. **Do not** recognise this frame by B0: in real logs it appeared as both `D9 DF 6C 5E` and `19 DF 6C 5E` (the display bits in B0 change with chime state), and B0 with B2=0x6C would otherwise make an *armed* system look *disarmed* during a blackout.

Firmware behaviour in blackout:

| Aspect | Behaviour |
|---|---|
| Mains 220 V sensor | ASSENTE on first NO-AC frame; PRESENTE after 25 s with no NO-AC frame (hysteresis) |
| Why hysteresis | in blackout the frames alternate every ~5-10 s; without it the sensor would oscillate |
| armed / alarm | always live: the real state frame keeps coming, B1/B2/B3 unchanged |
| chime / ready / bypass | frozen at last known value (the blackout frame sequence makes them unreliable) |
| NO-AC frame | discarded: never enters state logic |

This means **an alarm during a blackout is still detected** — the alarm frame (`1F 16 DF 2B`) is distinct from the NO-AC frame and continues to be processed.

---

## 🖥️ Why B0 (the display byte) is not used for state

B0 drives the two-digit display plus the surrounding segment words. Per the panel manual the LCD shows: digits + ALARM/CHECK/FIRE (left), AWAY/STAY/INSTANT/BYPASS (centre), NO AC/CHIME/BAT (top right), NOT READY (bottom right).

B0 is **multiplexed**: the same high bits drive different segments depending on which refresh frame you're looking at. Proof from real logs: `B0 = 0xD6` appears both with "chime ON, bypass OFF" and with "chime OFF, bypass ON" — the same byte value for two different real states. A byte that returns the same value for different conditions cannot be decoded frame-by-frame. Every indicator B0 shows is already available cleanly elsewhere (chime/bypass from the status frame, mains from the NO-AC frame, armed/alarm from B2/B3), so B0 is documented here only as a **warning**, never used for state. Independently, gregrenda's 6150 mapping also assigns **no** bits to B0 — two separate reverse-engineering efforts both leave it alone.

---

## 🔬 Cross-check against gregrenda's 6150 mapping

[gregrenda/ademco](https://github.com/gregrenda/ademco) reverse-engineered the same non-addressable bus but for **alphanumeric 6150** keypads. Converting his bit constants to byte.bit positions:

| Signal | This project (4127) | gregrenda (6150) | Match |
|---|---|---|---|
| ALARM | B3 bit1 (0x02) | B3 bit1 (0x02) | identical |
| INSTANT (MAX marker) | B3 bit7 (0x80) | B3 bit7 (0x80) | identical |
| STAY / ARMED | B2 bit7 (0x80) | B2 bit7 (0x80) | identical |
| BAT (low battery) | B2 bit6 (0x40) | B2 bit6 (0x40) | identical |
| CHIME | (4127-specific) | B3 bit5 (0x20) | diverges |
| BYPASS | (4127-specific) | B3 bit4 (0x10) | diverges |
| READY | (4127-specific) | B2 bit4 (0x10) | diverges |

The four "deep" state bits match exactly — they're keypad-independent. The diverging ones are exactly the bits gregrenda marks "6150 only": the panel drives them differently for a fixed-word keypad. Timing also converges: gregrenda uses `FRAME_MS = 176` with a sync threshold of `(176−5) = 171 ms`, identical to this firmware's `SYNC_MIN_US = 171000`.

---

## 🔀 Keypad-specific mapping (read this before porting)

The byte mappings here were reverse-engineered with a **4127 fixed-word keypad**. The panel adapts its bus output to the keypad type, so:

- **4137** — sister of the 4127 (same fixed-word display, backlit + larger). Almost certainly the same frames; likely works unmodified, unconfirmed.
- **5137** — also fixed-word, listed alongside the 4127/4137 on the panel's connection diagram (current draw 90 mA vs 20 mA for the 4127). Same family, likely compatible, unconfirmed.
- **6128** — fixed-word, same family; grouped with 6150 for addressing in the program sheet.
- **6139 / 6160 / 5330 (alphanumeric)** — state is sent as display text, not these bits. The method transfers, the constants do not. See gregrenda's approach.

> **Localised variants:** Ademco produced country-localised keypads (e.g. an Italian version showing RETE / PRONTO / NON PRONTO instead of AC / READY / NOT READY). These are electrically identical — the bus sends segment bits, not text, so the silk-screened label on the glass is the only difference. The bit mapping is the same.

If you have a different keypad, capture a bus log with `diagnostic_mode: true` — the approach still works, only the constants change. **PRs adding a keypad column are welcome.**

---

## 🛠️ Hardware

| Item | Detail |
|---|---|
| MCU | ESP32 NodeMCU-32S |
| RX | GPIO21 (inverted, via 4N35 optocoupler) |
| TX | GPIO22 (RMT, key transmission) |
| Sync | GPIO35 (inverted) |
| Bus | 2400 baud, 8E2 |

An optocoupler on the RX line safely level-shifts and isolates the keybus from the ESP32. The bus interface circuit is inspired by gregrenda's optocoupler approach, redrawn for this wiring.

---

## 🔌 Powering the ESP32

**A) Separate USB supply (simplest, safest).** No extra load on a 30-year-old board, no switching noise on the keybus. Downside: if mains fails, the ESP32 dies exactly when the panel goes to battery — but if you already monitor mains elsewhere in HA, that's covered.

**B) From the panel's 12 V (keeps the ESP32 alive on the panel's battery during a blackout).** The 4120XM aux output is rated **700 mA total** (official spec) — subtract whatever your sensors *and keypads* already draw (per the panel's connection diagram: 4127 = 20 mA, 4137 = 60 mA, 5137 = 90 mA) and check the margin; the ESP32 pulls roughly **100-160 mA at 12 V** at WiFi peak via a buck converter. On an old board, protect it:

- **Reverse-polarity diode** in series (a plain `1N4007` is fine — 1 A / 1000 V, wildly over-spec; the ~0.7 V drop is irrelevant because the buck regulates anyway).
- **Reservoir electrolytic (470-1000 µF)** on the buck's 12 V input — a local buffer that absorbs the ESP's current peaks so they aren't demanded from the panel's aged supply.
- **Inline fuse (200-300 mA)** so an ESP fault never takes down the panel.
- A **good low-ripple buck**, not the cheapest module, to avoid injecting switching noise onto the 12 V line.

> Reference battery for these panels is a **12 V 7 Ah** SLA (Ademco "UB1270"). A fresh 7 Ah easily covers the panel + a few sensors + the ESP32 through a normal blackout. Powering order (from the manual): connect the transformer first, then the battery.

---

## 📡 Compatibility

**Tested:** Ademco **4120** with **4127** fixed-word keypad.

**Likely compatible but UNTESTED** — feedback and logs welcome:

- Ademco 4110, 4110XM, 4110DL
- Ademco 4120EC, 4120XM, 4140XMP
- Ademco 5110XM
- Vista-10 / Vista-10SE
- Vista-15 (early models)
- Vista-20 / Vista-20SE *in Address-31 mode*

...**with a fixed-word keypad (4127 / 4137 / 5137 / 6128).** With an alphanumeric keypad the byte values differ — see the keypad section.

**Discoverability keywords:** non-addressable Ademco, Address 31, Ademco 4110/4120/4120XM Home Assistant, Vista-10 Vista-10SE Home Assistant, AlarmDecoder alternative for older panels, fixed-word keypad 4127 6128, ESPHome Ademco keybus.

If you try this on a panel/keypad not in the tested list, please open an issue with a bus log — your data helps map other models.

---

## 🤝 Contributing (help build the "universal" map)

There is no single universal decode for these panels — each keypad type is a column in a table that only gets filled by people with different hardware. This repo is meant to be that collection point:

1. Flash the firmware with `diagnostic_mode: true`.
2. Exercise your system (arm total / stay / max, trigger a test alarm, open zones).
3. Capture the raw frames from the log.
4. Open an issue or PR with your **panel model + keypad model + frames**.

Cheap second-hand boards (4110/4120 series) are widely available and make a safe bench rig for this — no need to experiment on your live panel.

---

## 🚧 Known limitations / to-do

- **Mains (AC) detection**: confirmed via the dedicated NO-AC frame (`B1=0xDF B2=0x6C B3=0x5E`); B0 is ignored (multiplexed display byte). During a blackout chime/ready/bypass are frozen at their last known value; armed/alarm remain live and accurate.
- **Low-battery** flag is a coarse fault indicator, not a health gauge. On these old panels it's set only when a periodic weak load-test fails — it flags a dead/disconnected battery, not a degrading one. Treat it as a fault indicator, not a health readout; for real health, measure battery voltage directly.
- **CHECK indicator** (loop trouble / faulty sensor wiring) is visible on the keypad display but **not yet mapped** on the bus. Unlike mains/battery it can't be safely toggled on demand — it reflects a real loop fault (open wiring, bad EOLR). A candidate exists from the gregrenda cross-check (B2 bit1 of the 0x0C frame) but it may instead be a message frame carrying the faulty zone number, like the NO-AC and zone frames. To be mapped opportunistically: with `diagnostic_mode: true`, open an EOLR-supervised zone loop (e.g. disconnect the HI wire of a supervised zone for a minute) and capture the log, or wait for a real fault.
- **Alarm during blackout**: logic is safe by construction (the alarm frame is distinct from the NO-AC frame and always processed), but not yet captured live. To confirm: arm the system, disconnect mains, trigger a test alarm.
- Compatibility beyond the 4120/4127 is theoretical.
- Zone names are hard-coded (edit the `ZONE_NAMES` array to match your installation).
- Firmware comments/logs are in Italian; protocol documentation (this README + the PDF) is in English.

---

## 📄 License

Released under the **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0)**.

- **Free for personal/home use** — replicate, modify and use on your own system.
- **Share and adapt** — with attribution, under the same license.
- **No commercial use** — you may not sell pre-assembled modules nor commercialise this software or forks of it.

Full text: https://creativecommons.org/licenses/by-nc-sa/4.0/

> Creative Commons licenses were designed for creative works rather than software, but are used here intentionally: the goal is a clear non-commercial restriction, which standard OSS licenses (MIT, GPL, etc.) do not provide.

---

## 🙏 Credits

Protocol reverse-engineering builds on insights from [gregrenda/ademco](https://github.com/gregrenda), whose work on non-addressable Ademco panels (alphanumeric-keypad, text-parsing approach) provided cross-checks for several state bits and the bus timing, and inspired the optocoupler bus-interface circuit. This project extends that work into the **fixed-word keypad** case through empirical bus capture on a 4120 / 4127.

Panel behaviour cross-referenced against Ademco's own 4120SP/4120XM connection diagram and program sheet (keypad families and current draw, Address-31 procedure, 700 mA aux rating, NO AC / BAT semantics, 12 V 7 Ah battery, display segment layout).
