# Button, Switch and LED

**[README](../README.md)** > **Button, Switch and LED** · [Report an issue](../../../issues/new)

The button on the device and its LED are the device's only local interface. This page is the complete specification: what every input does in every state, and what every LED pattern means. It applies to every device and variant this firmware supports. Where a variant differs, the tables say so.

> **Quick card.** Tap: on/off · Press 3×: pairing · Hold 5 s: Wi-Fi mode · Hold 10 s: factory reset · Keep holding past 20 s: cancel · 2PM: tap for relay 1, press 2× for relay 2
>
> Nothing happens while you hold. The LED shows what letting go will do, and the action happens when you release.

The gestures and their timings match stock Shelly firmware ([Shelly KB](https://kb.shelly.cloud/knowledge-base/shelly-1pm-gen4)): hold 5 s for the device's own Wi-Fi network, hold 10 s to factory reset, press 3 times to pair.

## Contents

- [Gestures](#gestures)
- [While the button is held](#while-the-button-is-held)
- [What the button does](#what-the-button-does)
- [Wall switch](#wall-switch)
- [What each action does](#what-each-action-does)
- [Power-on and restarts](#power-on-and-restarts)
- [LED](#led)
- [Timings](#timings)
- [Keeping this page accurate](#keeping-this-page-accurate)

---

## Gestures

The button recognizes these gestures: a tap, 2 taps, 3 or more taps, and holds. A press shorter than 1 s is a tap. Taps less than 300 ms apart count together. Everything else is a hold, timed from press to release.

| ID | You do | Gesture |
|---|---|---|
| G-01 | 1 press, released before 1 s | Tap |
| G-02 | 2 presses less than 300 ms apart | 2 taps |
| G-03 | 3 presses less than 300 ms apart | 3+ taps |
| G-04 | 5 presses less than 300 ms apart | 3+ taps |
| G-05 | 2 presses 500 ms apart | Tap, Tap |
| G-06 | Press held 950 ms | Tap |
| G-07 | Hold 1 s | Nothing |
| G-08 | Hold 3 s | Nothing |
| G-09 | Hold 5 s | Hold 5-10 s |
| G-10 | Hold 8 s | Hold 5-10 s |
| G-11 | Hold 10 s | Hold 10-20 s |
| G-12 | Hold 15 s | Hold 10-20 s |
| G-13 | Hold 20 s or longer | Nothing |
| G-14 | 2 presses, then a 6 s hold less than 300 ms later | Hold 5-10 s |
| G-15 | 2 presses less than 300 ms apart, a third 500 ms later | 2 taps, Tap |

- A tap acts about 0.3 s after you let go, once no further press follows. That wait is what lets 3 presses open pairing without switching the load three times.
- 2 quick taps toggle relay 2 on the 2PM. On every other device they toggle once, like a tap, not twice.
- A hold of 1-5 s does nothing, so a slow press never switches or resets anything by accident.
- Holding past 20 s cancels whatever the hold was about to do.
- A hold that follows taps discards the taps.

## While the button is held

The LED stops showing the device state and shows what releasing now would do. It goes back to normal as soon as you let go.

| ID | Held for | LED shows |
|---|---|---|
| H-01 | Less than 1 s | Normal pattern |
| H-02 | 1 s to 5 s | Off |
| H-03 | 5 s to 10 s | Hold for Wi-Fi mode |
| H-04 | 10 s to 20 s | Hold for factory reset |
| H-05 | 20 s or longer | Normal pattern |

"Hold for Wi-Fi mode" is a ½ s blink (LED-12) and "Hold for factory reset" a ¼ s blink (LED-13), the same timings stock Shelly firmware uses. Each press also gives a brief off-blink (LED-10) so you can count your presses.

## What the button does

Every gesture in every state. A refused action shows three quick flashes (LED-14). An accepted pairing, Wi-Fi, reset or settings request shows one long flash (LED-15).

| ID | State | Gesture | Result | Why |
|---|---|---|---|---|
| B-01 | Booting | Tap | Refused | Matter is not up yet (the first few seconds after power-on) |
| B-02 | Booting | 3+ taps | Refused | Pairing needs Matter |
| B-03 | Booting | Hold 5-10 s | Refused | Wait until the device has started |
| B-04 | Booting | Hold 10-20 s | Factory reset after a restart | A reset works even before Matter is up: the device restarts and resets once Matter starts |
| B-05 | Normal | Tap | Toggle | |
| B-06 | Normal | 3+ taps | Open pairing | See [Open pairing](#what-each-action-does) |
| B-07 | Normal | Hold 5-10 s | Enter Wi-Fi mode | |
| B-08 | Normal | Hold 10-20 s | Factory reset | |
| B-09 | First 60 s after an update | Tap | Toggle | |
| B-10 | First 60 s after an update | 3+ taps | Open pairing | |
| B-11 | First 60 s after an update | Hold 5-10 s | Refused | The new firmware confirms itself after 60 s. A restart before then rolls the update back. |
| B-12 | First 60 s after an update | Hold 10-20 s | Refused | Same reason: wait 60 s |
| B-13 | Matter update downloading | Tap | Toggle | |
| B-14 | Matter update downloading | 3+ taps | Open pairing | |
| B-15 | Matter update downloading | Hold 5-10 s | Refused | The restart would abandon the download |
| B-16 | Matter update downloading | Hold 10-20 s | Factory reset | A reset is always allowed; it abandons the download |
| B-17 | Normal, built without Wi-Fi mode | Tap | Toggle | |
| B-18 | Normal, built without Wi-Fi mode | 3+ taps | Open pairing | |
| B-19 | Normal, built without Wi-Fi mode | Hold 5-10 s | Refused | This build has no Wi-Fi mode |
| B-20 | Normal, built without Wi-Fi mode | Hold 10-20 s | Factory reset | |
| B-21 | Wi-Fi maintenance mode | Tap | Toggle, without Matter | The relay switches directly; Home Assistant sees the new state once the device is back in normal mode (P-03) |
| B-22 | Wi-Fi maintenance mode | 3+ taps | Refused | Pairing needs Matter, which is off in Wi-Fi mode. With a change waiting on the page, the presses save it instead (B-26) |
| B-23 | Wi-Fi maintenance mode | Hold 5-10 s | Exit Wi-Fi mode | The same gesture that entered it. An upload in progress is abandoned; nothing is installed |
| B-24 | Wi-Fi maintenance mode | Hold 10-20 s | Factory reset after a restart | The device restarts into normal mode and resets once Matter starts. An upload in progress is abandoned |
| B-25 | Wi-Fi mode, change waiting | Tap | Toggle, without Matter | As B-21. The change keeps waiting |
| B-26 | Wi-Fi mode, change waiting | 3+ taps | Save the change | The setup code, wall-switch mode, Wi-Fi mode password or meter calibration chosen on the page (see [Save the change](#what-each-action-does)) is saved. The presses must come within 2 minutes of the change; after that it is dropped and 3 presses are refused again (B-22) |
| B-27 | Wi-Fi mode, change waiting | Hold 5-10 s | Exit Wi-Fi mode | The change is dropped; the old setting stays |
| B-28 | Wi-Fi mode, change waiting | Hold 10-20 s | Factory reset after a restart | The change is dropped. A factory reset keeps the switch mode and meter calibration, and forgets a setup code you entered (see [Factory reset](#what-each-action-does)) |
| B-29 | Booting | 2 taps | Refused | As B-01 |
| B-30 | Normal | 2 taps | Toggle | Relay 2 on the 2PM; on other devices the same as a tap (relay 1) |
| B-31 | First 60 s after an update | 2 taps | Toggle | As B-30 |
| B-32 | Matter update downloading | 2 taps | Toggle | As B-30 |
| B-33 | Normal, built without Wi-Fi mode | 2 taps | Toggle | As B-30 |
| B-34 | Wi-Fi maintenance mode | 2 taps | Toggle, without Matter | As B-30, without Matter (B-21) |
| B-35 | Wi-Fi mode, change waiting | 2 taps | Toggle, without Matter | As B-34. The change keeps waiting |

"Normal" covers every normal-mode state not listed separately: paired or not, pairing window open or closed, joining Thread or connected, Identify running, or a hardware fault active.

## Wall switch

The SW terminal. It never resets the device, never opens pairing, and never enters Wi-Fi mode, as in stock firmware.

| ID | Variant | State | Result | Notes |
|---|---|---|---|---|
| SW-01 | Light, Outlet, Mini, 1PM, 2PM SW1 | Booting | Ignored | |
| SW-02 | Light, Outlet, Mini, 1PM, 2PM SW1 | Normal | Toggle | In the default Toggle mode: every change of the switch toggles, so any switch position works. Other modes: see [Wall switch modes](#wall-switch-modes) |
| SW-03 | Light, Outlet, Mini, 1PM, 2PM SW1 | Wi-Fi maintenance mode | Toggle, without Matter | In the default Toggle mode; see [Wall switch modes](#wall-switch-modes) |
| SW-04 | Opener | Booting | Ignored | |
| SW-05 | Opener | Normal | Its own use | The door contact sensor; it never switches the relay |
| SW-06 | Opener | Wi-Fi maintenance mode | Ignored | |
| SW-07 | Light Switch | Booting | Ignored | |
| SW-08 | Light Switch | Normal | Its own use | Sends On/Off to the bound lights; the button still toggles the local relay |
| SW-09 | Light Switch | Wi-Fi maintenance mode | Ignored | Bindings need Matter |
| SW-10 | 2PM SW2 | Booting | Ignored | |
| SW-11 | 2PM SW2 | Normal | Toggle | Relay 2, in SW2's own mode. SW1 drives relay 1 (SW-02) |
| SW-12 | 2PM SW2 | Wi-Fi maintenance mode | Toggle, without Matter | Relay 2, in SW2's own mode |

A change waiting on the Wi-Fi mode page doesn't change these rows: the wall switch never confirms it.

### Wall switch modes

The Light, Outlet, Mini, 1PM and 2PM let you choose what the wall switch does, on the Wi-Fi mode page (saved with 3 presses of the button, B-26). The Opener and Light Switch use SW for their own purpose and have no modes. While booting, the switch is ignored in every mode (SW-01). A build without Wi-Fi mode always uses Toggle, whatever was saved, since it has no page to change it back. The mode is kept through factory resets and updates. On the 2PM each input has its own mode, and each drives its own relay: SW1 relay 1, SW2 relay 2.

| ID | Mode | Change | Normal | Wi-Fi mode |
|---|---|---|---|---|
| SM-01 | Toggle | Either | Toggle | Toggle, without Matter |
| SM-02 | Push button | Press | Toggle | Toggle, without Matter |
| SM-03 | Push button | Release | Ignored | Ignored |
| SM-04 | Follow | To on | Turn on | Turn on, without Matter |
| SM-05 | Follow | To off | Turn off | Turn off, without Matter |
| SM-06 | Disabled | Either | Ignored | Ignored |

- **Toggle** (the default): for an ordinary on/off wall switch. Any flip toggles, whichever way the switch points.
- **Push button:** for a button that springs back (a doorbell-style button). Each press toggles once. If the firmware has the switch's polarity backwards (see below), it toggles on release instead, still once per press.
- **Follow:** the relay matches the switch: switch on, relay on. It acts on changes only; at power-on the relay follows its power-on setting (P-01), not the switch. It relies on which input level means "switch on", taken from a hardware-tested ESPHome configuration but not verified with this firmware. If Follow runs backwards on your device, build with `CONFIG_SHELLY_SWITCH_ACTIVE_HIGH=n` and please report it.
- **Disabled:** the switch does nothing. Use it if SW is not wired or picks up noise.

Turn on and Turn off go through the same checks as a toggle: a refused On shows LED-14, and on the 1PM an On clears an overcurrent trip and an Off cancels an overvoltage restore. If the relay is already in the wanted state, nothing happens and nothing flashes: the relay already shows the switch's position. This is the one input with no feedback of its own.

## What each action does

**Toggle.** Flips the relay through Matter, so every controller sees the change.
- If the relay refuses to turn on, the LED shows the refusal flash (LED-14). It refuses during over-temperature, when the temperature sensor has failed, and on the 1PM during overvoltage and for 2 s after a protection trip. On the 2PM an over-temperature trip turns both relays off, and both refuse to turn on until it clears. See [Protection and Metering](PROTECTION.md).
- If Matter stays busy for 2 s (for example while it is pairing), the toggle is refused too (LED-14) rather than acting late. The same applies to the Wi-Fi mode hold.
- **1PM:** a tap or switch change that turns the relay on clears an overcurrent trip. One that turns it off during an overvoltage trip cancels the automatic restore.
- **Opener:** a toggle is a 500 ms pulse. A tap during the pulse ends it.
- **Light Switch:** the button toggles the local relay only.

**Toggle, without Matter.** The same, in Wi-Fi maintenance mode: the relay switches directly and the same protections apply. The Opener pulses.

**Open pairing.** Opens a Matter pairing window over Bluetooth and Thread, and the LED shows the pairing blink (LED-02). The window takes the device's own setup code (see [Commissioning](COMMISSIONING.md#your-setup-code)).
- **Paired device:** the window lasts 3 minutes. Use it to add the device to a second ecosystem.
- **Unpaired device:** it lasts 15 minutes, like the window the device opens by itself at boot. After 15 minutes the device stops advertising (LED-03). Press 3 times to start again; no power cycle needed.
- **A window already open** restarts with its full time, unless a controller opened it with its own pairing code (Share device, for example): that window is left as it is, so the code the controller shows keeps working.
- **Refused** while someone is part-way through pairing, so their session is not cut off.

**Enter Wi-Fi mode** (experimental). The device restarts into Wi-Fi maintenance mode: Matter and Thread are off, the device runs its own Wi-Fi network named like stock Shelly's, `Shelly<model>-<the MAC on the sticker>`, with the sticker's setup code as its password (see [Wi-Fi mode password](UPDATING.md#wi-fi-mode-password)), and the page at `http://192.168.33.1` installs a firmware update and shows the device's Matter setup code. See [Updating](UPDATING.md#wi-fi-maintenance-mode).
- The relay keeps its state across the restart, apart from a brief off while it reboots. The Opener's trigger always stays off.
- The mode ends after an upload, on the page's Exit button, with the 5 s hold again, or after 10 minutes with no page open.
- Home Assistant shows the device as unavailable until it is back in normal mode.
- If the mode cannot start (the network or the page fails), the device flashes refusal and restarts normally.

**Exit Wi-Fi mode.** The device restarts into normal mode. The state the relay had in Wi-Fi mode becomes its previous state, and the relay then follows its power-on setting from there (see below): with Previous it keeps that state, and controllers see it once Matter is back.

**Factory reset.** Clears the Matter pairings and the Thread network, then restarts ready to pair (LED-02). A setup code you entered on the Wi-Fi mode page is forgotten: the device goes back to the code on its Shelly sticker, or to the code it made itself if it has no sticker code (see [Commissioning](COMMISSIONING.md#your-setup-code)).
- The firmware stays installed. This is not a return to stock firmware: see [Reversibility](REVERSIBILITY.md).
- **1PM:** the learned mains voltage is cleared too. The energy total is kept.
- An overcurrent trip survives the reset. The first On from the new controller, or a tap, clears it.

**Save the change.** On the Wi-Fi mode page you can change these settings: the Matter setup code (enter another 11-digit code, or make a new random one; typing the Shelly sticker's code goes back to it; see [Commissioning](COMMISSIONING.md#your-setup-code)); on the Light, Outlet, Mini, 1PM and 2PM, the [wall switch mode](#wall-switch-modes) (on the 2PM, SW1's and SW2's separately); the [Wi-Fi mode password](UPDATING.md#wi-fi-mode-password); and on the 1PM and 2PM, the meter calibration (see [Protection and Metering](PROTECTION.md#calibrating-the-meter)). Nothing changes until you press the button 3 times within 2 minutes: then the LED gives one long flash (LED-15). A new setup code applies from the next normal boot, a new Wi-Fi mode password the next time Wi-Fi mode starts; a new switch mode or calibration applies at once. One change waits at a time: choosing another replaces it. The page shows the waiting change and the time left; after 2 minutes, on Cancel, or on leaving Wi-Fi mode, it is dropped.

**Factory reset after a restart.** The device records the request, restarts, and resets as soon as Matter is up. If Matter never starts, the reset does not happen; reflash over UART.

## Power-on and restarts

| ID | Situation | Relay | LED |
|---|---|---|---|
| P-01 | Power-on, or any restart into normal mode | Follows StartUpOnOff (each relay its own on the 2PM): default Off. The Opener is always Off. A 1PM with an overcurrent trip stays Off. | Off while booting, then the state |
| P-02 | Restart into Wi-Fi mode | Back to its state before the restart, after a brief off. On the 1PM and 2PM, Off while the meter is off in Wi-Fi mode after an unexpected stop, since overcurrent protection is off then too (see [Calibrating the meter](PROTECTION.md#calibrating-the-meter)); you can still switch it on | Wi-Fi mode double blink (LED-08) |
| P-03 | Exit from Wi-Fi mode | Follows StartUpOnOff, as P-01, from its state in Wi-Fi mode: with Previous it keeps that state | Off while booting, then the state |
| P-04 | After a factory reset | Off | Pairing blink (LED-02) |

## LED

One LED shows everything. When several things apply at once, the highest wins:

1. Button hold hint (while held)
2. One-shot flash (press, refused, accepted)
3. Identify
4. Hardware fault, except in Wi-Fi maintenance mode, whose patterns (LED-08, LED-09) show over a fault so you can see that the mode is running. A fault also shows over a Matter update, so a trip is never hidden
5. Matter update downloading or installing (LED-16)
6. Device state

Timings are in milliseconds. "Repeating" patterns loop; "once" patterns play one time over whatever else is showing.

| ID | Pattern | Meaning |
|---|---|---|
| LED-01 | off | Booting |
| LED-02 | on 200, off 200, repeating | Pairing window open, whether or not the device is already paired |
| LED-03 | on 100, off 1900, repeating | Not paired, and the pairing window has closed: press 3 times to pair |
| LED-04 | on 500, off 1500, repeating | Paired, joining the Thread network |
| LED-05 | on | Connected and working |
| LED-06 | on 100, off 900, repeating | Identify: a controller asked the device to show itself |
| LED-07 | on 100, off 100, repeating | A hardware fault is active, such as over-temperature or a protection trip. See [Protection and Metering](PROTECTION.md). A 1PM on bench power shows this too, because its meter has no mains to measure (in Wi-Fi mode it shows LED-08 or LED-09 instead). |
| LED-08 | on 100, off 100, on 100, off 700, repeating | Wi-Fi maintenance mode |
| LED-09 | on | Wi-Fi maintenance mode, firmware upload in progress |
| LED-10 | off 100, once | Button press registered |
| LED-11 | off | Held 1-5 s: keep holding |
| LED-12 | on 500, off 500, repeating | Held 5-10 s: release for Wi-Fi mode (or to leave it) |
| LED-13 | on 250, off 250, repeating | Held 10-20 s: release to factory reset |
| LED-14 | on 100, off 100, on 100, off 100, on 100, off 100, once | Refused (see [What the button does](#what-the-button-does)) |
| LED-15 | on 600, once | Accepted: pairing opened, Wi-Fi mode or reset starting, or a setting saved |
| LED-16 | on 1000, off 1000, repeating | A Matter update is downloading or installing: keep the device powered. It restarts into the new firmware when done. The 5 s hold is refused a little longer (B-15): from the moment the device starts checking for an update until the check or update ends |

## Timings

| ID | Meaning | Value |
|---|---|---|
| T-01 | A press shorter than this is a tap | 1000 ms |
| T-02 | Taps closer together than this count together | 300 ms |
| T-03 | Hold at least this long for Wi-Fi mode | 5000 ms |
| T-04 | Hold at least this long for factory reset | 10000 ms |
| T-05 | Hold at least this long to cancel | 20000 ms |
| T-06 | Taps that open pairing | 3 |

## Keeping this page accurate

This page is the specification, and the code is tested against it.
- **Test rows:** every G, H, B, SW and SM row is a host test with the same ID and wording, in `source/components/test_host/test_ui.c`.
- **Code rows:** the LED and T rows are read straight from `led_patterns.h` and `button_logic.h`.
- **The check:** `scripts/check-ui-spec.py`, run by `source/components/test_host/run.sh`, fails if the page and the code disagree in any row.

To change how the device behaves, change this page, the code and the tests together. See [CLAUDE.md](../CLAUDE.md).

---

## Related

- [Commissioning](COMMISSIONING.md) — pairing with Apple Home, Google Home, Alexa and Home Assistant
- [Updating](UPDATING.md) — Matter OTA and Wi-Fi maintenance mode
- [Protection and Metering](PROTECTION.md) — 1PM Gen4 protections and what refuses a turn-on
- [Reversibility](REVERSIBILITY.md) — factory reset is not a return to stock
