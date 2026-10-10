# Protection and Metering

**[README](../README.md)** > **Protection and Metering** · [Report an issue](../../../issues/new)

This page explains how the firmware protects the load and the device, how it measures power on the Shelly 1PM Gen4 and 2PM Gen4, and how faults show up in Home Assistant and other Matter controllers.

> ⚠️ **This is secondary protection.** The circuit breaker in your panel is the primary protection for the wiring and the load, and nothing here replaces it. The firmware can only open the relay earlier than a breaker would, or tell you that something is wrong. It cannot open a welded relay, and it cannot see what the meter cannot measure.

> ⚠️ **Shelly 1PM Gen4 and 2PM Gen4 support is experimental.** The metering and electrical protection described here have not been verified on hardware yet, apart from the 2PM meter readings used for its calibration. Every hardware-dependent assumption is designed to fail safe, and the assumptions are listed in [What has not been verified](#what-has-not-been-verified).

---

## Contents

- [At a glance](#at-a-glance)
- [Overcurrent](#overcurrent)
- [Overvoltage](#overvoltage)
- [Over-temperature](#over-temperature)
- [Welded relay and meter alerts](#welded-relay-and-meter-alerts)
- [Resetting a trip](#resetting-a-trip)
- [Seeing faults in Home Assistant](#seeing-faults-in-home-assistant)
- [Power metering](#power-metering)
- [Calibrating the meter](#calibrating-the-meter)
- [Supply and loads](#supply-and-loads)
- [Changing the limits](#changing-the-limits)
- [What has not been verified](#what-has-not-been-verified)

---

## At a glance

| Protection | Devices | Trigger | Action | Reset |
|---|---|---|---|---|
| Overcurrent | 1PM Gen4, 2PM Gen4 | Heat model on current above 16.5 A (1PM) or 11 A per relay (2PM) | That relay off, **latched** | A deliberate On, or a power cycle |
| Total current | 2PM Gen4 | Heat model on the two relays' sum above 17.6 A | Both relays off, **latched** | A deliberate On of each, or a power cycle |
| Overvoltage | 1PM Gen4, 2PM Gen4 | Above 150 V (120 V mains) or 264 V (230/240 V mains) for 5s | Relay off (both on the 2PM) | Automatic, 30s after the voltage is back in range |
| Over-temperature | All | Board NTC above 90°C for 10s (1PM, 2PM), or ESP32 die above 75°C for 10s (others) | Relay off (both relays on the 2PM), turn-on refused | Automatic, below 75°C (NTC) or 65°C (die) |
| No temperature sensor | All | No usable temperature reading | Turn-on refused | Automatic, when a reading returns |
| Welded relay | 1PM Gen4, 2PM Gen4 | 0.5 A or more flowing 5s after a relay was switched off | Alert only | Current stops, or the next On/Off |
| Meter lost | 1PM Gen4, 2PM Gen4 | No valid reading from the power meter for 5s | Alert only; the relays keep working | Automatic |

On the 2PM each relay has its own overcurrent model and latch, so an overload on one leaves the other alone. Firmware 1.0.0 for the 2PM had no metering: over-temperature was its only protection. Electrical protection starts with 1.0.1.

On a DC supply the meter can't measure, so overcurrent and overvoltage protection don't work there. See [Supply and loads](#supply-and-loads), which also covers inductive loads and the EU model.

Every trip turns the relay off first and then reports it: the OnOff state in your controller changes to off, and a hardware fault is raised (see [Seeing faults in Home Assistant](#seeing-faults-in-home-assistant)). The status LED flashes rapidly (100ms on, 100ms off) while any fault is active. A turn-on that a protection refuses shows three quick flashes instead of switching; see [Button, Switch and LED](BUTTON-AND-LED.md).

---

## Overcurrent

The Shelly 1PM Gen4 (UL) is rated 15 A. Loads briefly draw more than that: a motor or compressor starting, an incandescent lamp's cold filament, a power supply charging its capacitors. A trip at exactly 15 A would switch those loads off for no reason, so the firmware uses a heat model instead of an instantaneous limit, much like the thermal element of a breaker.

**How it works.** The firmware reads the RMS current every 200ms. Below 16.5 A (110% of the rating, which covers a ±2% meter error with margin) nothing accumulates: a load at 15 A runs indefinitely. Above 16.5 A, heat builds in proportion to I² − 16.5², and the relay trips when the heat reaches 2800 A²s. Below 16.5 A, and whenever the relay is off, the heat cools off with a 5-minute time constant.

| Current | Time to trip |
|---|---|
| 16.5 A or less | Never |
| 18 A | 54 s |
| 20 A | 22 s |
| 22.5 A | 12 s |
| 25 A | 8 s |
| 29.7 A and above | 4.6 s |

**Shelly 2PM Gen4.** Shelly rates the 2PM 10 A per relay and 16 A for the whole device. Each relay has the same kind of model, scaled to 10 A so the trip times match the table at the same multiple of the rating: nothing builds at 11 A or less, 12 A trips in 54 s, 15 A in 12 s and 20 A in 4.5 s (k = 1244 A²s). A second model on the sum of the two relays' currents, scaled to 16 A (pickup 17.6 A, 19.2 A trips in 54 s), turns **both** relays off: two loads of 9 A each are fine one at a time but too much together. Each latched relay is turned back on separately.

**Transients.** A short burst above 15 A adds only a little heat. A motor start that reads 29.7 A for one second adds about 610 A²s, a fifth of the trip level, and it cools off between starts. A compressor that starts every few minutes never accumulates enough to trip. One that starts every 30 seconds does, because the heat does not have time to cool: that pattern is a short-cycling fault, and the trip is intended.

**What the meter cannot see.** On the 2PM the ADE7953's clipping point is not known for this board, so no reading is treated as clipped: rely on the breaker for short circuits and very high currents. On the 1PM, the BL0942 current channel clips at about 29.7 A for a sine wave. Every reading at or near that level is treated as at least 29.7 A, so a heavy overload still trips in under 5 seconds, and a short circuit is left to the breaker. Loads with a high crest factor, such as switch-mode power supplies without power factor correction, clip earlier, from about 14 A RMS. The meter then reads **low**, which is the unsafe direction, and the trip comes later than the table says or not at all. Rely on the breaker for those loads.

A trip is **latched**: the relay stays off until someone deliberately turns it back on. See [Resetting a trip](#resetting-a-trip).

---

## Overvoltage

A lost neutral on a 120/240 V split-phase supply can put up to 240 V across a 120 V load. The firmware opens the relay when the voltage stays above the limit for 5 seconds:

| Mains | Limit |
|---|---|
| 120 V | 150 V |
| 230/240 V | 264 V |

**Learning the mains.** The device learns which system it is on from the first settled readings after it is installed: 80 V or more for three readings, then below 180 V means 120 V, and 180 V or more means 230/240 V. It stores the result and never relearns on its own, so a surge or a lost neutral cannot reclassify a 120 V installation as 240 V. A [factory reset](COMMISSIONING.md#factory-reset) clears it, so a device moved to a different installation relearns. Until the mains is learned, there is no overvoltage protection.

**Recovery.** Once the voltage has been back in range for 30 seconds, the relay returns to the state it was in before the trip. If you switch the outlet during the trip (an Off command, a toggle, the button or the wall switch), your choice wins and nothing is restored. On the 2PM the trip turns both relays off, and each is restored on its own: switching one during the trip cancels only that one's restore. A restore goes through every other check first, so it never overrides an over-temperature or overcurrent condition.

---

## Over-temperature

Every device monitors its temperature every 2 seconds and reports it on the Temperature Sensor endpoint.

- **Shelly 1PM Gen4 and 2PM Gen4:** the board NTC. On the 2PM a trip turns both relays off and both report Off. Trip above 90°C for 10 seconds, clear below 75°C. The NTC's beta value (3350) is an estimate, not a measurement, and the thresholds leave room for that. If the NTC reads as an open circuit (or cannot be read) three times in a row, the firmware raises a sensor fault and falls back to the ESP32 die sensor with the die thresholds. A shorted NTC reads as very hot and trips.
- **All other devices:** the ESP32-C6 die temperature. Trip above 75°C for 10 seconds, clear below 65°C.

While over temperature, turn-on is refused. If no sensor gives a usable reading at all (three failed reads in a row), a sensor fault is raised and turn-on is refused until a reading returns. The relay is not switched off in that case, because the firmware cannot tell that anything is actually hot.

---

## Welded relay and meter alerts

**Welded relay.** If 0.5 A or more is still flowing 5 seconds after the relay was switched off (on the 2PM, each relay on its own meter channel) (with 2 seconds allowed for the load to wind down), the relay contacts may have welded. The firmware cannot open a welded relay, so this is an alert only. It clears when the current stays below 0.2 A for 10 seconds, or on the next On/Off cycle. Treat this alert seriously: the load cannot be turned off from the device, only at the breaker.

**Meter lost.** If the power meter stops answering for 5 seconds, a sensor fault is raised. The relay keeps working normally; only overcurrent, overvoltage and welded-relay detection pause until the meter returns. Voltage, current and frequency read as unknown, and power holds its last value while the relay is on (it reads 0 while off).

---

## Resetting a trip

- **In Wi-Fi maintenance mode** (see [Updating](UPDATING.md#wi-fi-maintenance-mode)) every protection still runs and trips the relay, unless the meter is off there after an unexpected stop (see [Calibrating the meter](#calibrating-the-meter)): then overcurrent and overvoltage are off in Wi-Fi mode, the relays start off there, and over-temperature still runs. An overvoltage trip is not restored automatically there: the relay stays off until you switch it, or until the device is back in normal mode and its power-on setting applies.
- **Overcurrent** is latched. Any of these clears it: an On command from a controller (or an On with timed off or recall global scene that the outlet acts on as an On), a Toggle from off, or a tap of the device button or a flip of the wall switch that turns it on (a tap acts when you let go). An On the relay refuses, within the 2 s below or during overvoltage, leaves the trip latched, and so does a tap or flip refused for any reason. Removing power clears it too. A software restart (a crash, a watchdog reset, a firmware update) does **not**: the latch survives in memory that keeps its contents across a restart, so a reboot can never re-energize a tripped load, even with Power-On Behavior set to On.
- **Overvoltage, over-temperature and sensor faults** clear themselves once the condition is gone.
- **After any trip,** turn-on is refused for 2 seconds. This keeps a wall switch or an automation from chattering the relay against a fault that is still present.

A turn-on that the firmware refuses fails the command, and the OnOff state stays off.

---

## Seeing faults in Home Assistant

Faults are reported through the standard Matter General Diagnostics cluster, so any Matter controller can read them:

| Fault | Reported as (HardwareFaultEnum) |
|---|---|
| Overcurrent, welded relay | Unspecified |
| Over-temperature | ResettableOverTemp |
| Overvoltage | PowerSource |
| No temperature sensor, NTC open, meter lost | Sensor |

The device also emits a HardwareFaultChange event each time the set changes, and reports its boot reason (including watchdog resets), reboot count and uptime.

In **Home Assistant 2026.7.0 or later**, active faults appear as a **Hardware faults** problem sensor on the device page. **It is disabled by default. Enable it** (device page → entities → Hardware faults → Enable) and use it in an automation to get notified of a trip. The fault type is visible in the device's diagnostics download. Boot reason, reboot count and uptime appear there too.

Apple Home and Google Home do not show hardware faults. In those apps a trip looks like the outlet turning itself off.

---

## Power metering

On the Shelly 1PM Gen4 the outlet endpoint, and on the Shelly 2PM Gen4 each relay's endpoint, also carries the Matter **Electrical Sensor** device type, with the standard Electrical Power Measurement and Electrical Energy Measurement clusters:

| Measurement | Unit | Reported when |
|---|---|---|
| Power | W | Changes by 1 W or 2%, whichever is larger |
| Voltage | V | Changes by 1 V |
| Current | A | Changes by 0.05 A |
| Frequency | Hz | Changes by 0.1 Hz |
| Energy (imported) | kWh | Every 10 Wh, and every 5 minutes |

All values are also refreshed once a minute. Home Assistant shows power, voltage, current and energy on current releases, and frequency from **2026.10**. The energy sensor can be added to the Energy dashboard. Apple Home and Google Home may show power or nothing, depending on their support for these clusters.

**Accuracy.** The meter is calibrated with references measured on one unit of each model at 120 V / 60 Hz: on the 1PM from the ESPHome configuration, on the 2PM against a plug-in meter with a 389 W heater on each relay. Expect roughly ±2% on another unit; [calibrate it](#calibrating-the-meter) against your own meter for better. Reverse power (a load feeding back) is counted as consumption. On the 2PM, a relay drawing under 1 W and 0.15 A reads 0: with nothing connected the meter reads about 0.07 A of noise.

**Energy total.** Each relay's running total is saved to flash every 15 minutes or every 100 Wh, whichever comes first, and continues across restarts and firmware updates. The 2PM's meter has no energy counter of its own, so its totals are added up from the power readings, five times a second. A power cut can lose up to 15 minutes or 100 Wh of the most recent energy. The total survives a factory reset. It is lost only if the device's storage has to be erased to recover from corruption, which also clears commissioning.

**AC only.** The meter measures AC. The device advertises metering only once it has seen AC mains (80 V or more), and remembers that it has. A 1PM powered from DC never shows a meter that would read zero. If a device is first powered from DC and later from AC, the metering entities appear after the next restart, and Home Assistant may need to re-interview the device.

---

## Calibrating the meter

Every device ships with the calibration measured on a reference unit, so this is optional. To match your own plug-in meter (a Kill-A-Watt or similar), open the **Meter** tab in [Wi-Fi maintenance mode](UPDATING.md#wi-fi-maintenance-mode), which also shows the live readings:

1. Plug a steady resistive load of at least 50 W (a space heater, a kettle or an incandescent bulb; not LED lights, chargers or motors) through your meter into the device's output, and turn that relay on. Keep it well under the rating: 10 A per relay on the 2PM.
2. Wait a few seconds for the reading to settle, then enter what your meter shows: volts, amps and watts. Leave a box empty to keep that value as it is. On the 2PM, choose the relay first; voltage is shared by both relays, current and power are per relay.
3. Press **Calibrate**, then press the device's button **3 times** within 2 minutes to save it ([Save the change](BUTTON-AND-LED.md#what-each-action-does)). It applies at once.

The device works out the correction from its own last 5 seconds of readings, so what the page displays doesn't need to match the moment you read your meter exactly. It refuses when:
- the load is under 0.5 A (for current) or 50 W (for power), or the voltage is under 80 V: too small to calibrate accurately;
- the readings changed by more than 3% in those 5 seconds (wait until the load is steady);
- the correction would be more than 15% from the built-in calibration (a typo, or the meter on a different load). The overcurrent protection uses these readings, so the bound also limits how far a mistake can move it.

Without a watts value, power follows the voltage and current corrections. **Back to the built-in calibration** undoes it (also with 3 presses). The calibration is kept through firmware updates and factory resets: it describes the hardware, not the pairing. A firmware update with better built-in values keeps your unit's calibration as it was.

Wi-Fi mode is how you install firmware, so a meter fault there must not lock you out of it: if a Wi-Fi mode boot stops unexpectedly (a crash, a hang, or the mode failing to start) while the meter runs, later Wi-Fi mode boots leave the meter off, and the Meter tab says so, until the next power cycle. Without the meter, overcurrent and overvoltage protection are off in those Wi-Fi mode boots, so the relays start off instead of returning to their state (you can still switch them on); over-temperature still runs. Metering and protection in normal operation are not affected. Not yet tested on hardware.

---

## Supply and loads

Notes from Shelly's own specifications for the 1PM Gen4 ([Knowledge Base](https://kb.shelly.cloud/knowledge-base/shelly-1pm-gen4)) that affect how far the firmware can protect you.

- **DC supply (24-30 V).** The device runs on DC, but the power meter measures AC only. On DC there is **no overcurrent and no overvoltage protection**, and no metering. Shelly rates the relay 10 A at 30 V DC: keep DC loads within that yourself. Over-temperature protection still works.
- **Inductive loads.** Shelly lists LED drivers, transformers, fans and refrigerators as supported only with an RC snubber across the load. Switching them without one wears the relay contacts, and a welded contact is the one fault the firmware can only report, not stop ([Welded relay](#welded-relay-and-meter-alerts)).
- **Resistive and capacitive loads** (heaters, incandescent lamps, electronics) need nothing extra.
- **EU model (S4SW-001P16EU, 16 A / 240 V).** The defaults are for the UL model (15 A / 120 V). On the EU model:
  - the overcurrent heat model starts at 16.5 A, which still runs a 16 A load but with little margin for meter error, so build with `CONFIG_SHELLY_PROT_RATED_CURRENT_MA=16000` (heat then builds above 17.6 A; see [Changing the limits](#changing-the-limits));
  - the 264 V overvoltage limit already covers 230/240 V mains;
  - the meter calibration was measured on one unit and may read differently on another model.
- **Ambient temperature.** Shelly rates the device for -20 °C to 40 °C. A hot enclosure brings the board closer to the 90 °C over-temperature trip.

---

## Changing the limits

The limits are compile-time settings. To change them, [build from source](BUILDING.md) and set them under **Shelly electrical protection** and **Shelly thermal protection** in `idf.py menuconfig`, or in the variant's `sdkconfig.defaults.c6_thread_shelly`. The defaults suit the UL-rated 15 A / 120 V Shelly 1PM Gen4; the 2PM variants set 10 A per relay and 16 A in total. The built-in meter calibration is under **Shelly board** (1PM) or **Shelly ADE7953 power meter** (2PM).

---

## What has not been verified

No part of the 1PM support has run on hardware yet. On the 2PM, the meter readings and their calibration were measured on hardware with the bring-up build (1.0.0), which only read the meter; the metering build (1.0.1), its protection and the calibration page have not run yet. Each assumption below fails safe if it is wrong:

| Assumption | If it is wrong |
|---|---|
| The UL 1PM Gen4 uses the same pin map as the unit that was mapped (PCB v0.1.2) | Unsafe: another pin could drive the relay. **Check the PCB revision before flashing** ([Flashing Guide](FLASHING.md#shelly-1pm-gen4-checklist)). |
| The meter calibration suits your unit | Readings off by a few percent. The 110% pickup absorbs ±2%. |
| The NTC beta is about 3350 | The temperature reads offset. The 90°C trip leaves margin, and a short or open NTC is handled. |
| The ESP32-C6 ADC is accurate below 0.4 V, where the NTC reads when hot | Over-temperature trips early or late; IDF's ADC calibration is used. |
| The web UI hardware code for the UL 1PM is `S1PMG4` | The web UI refuses the package. Harmless. |
| 2PM: the meter's setup write (0x120 = 0x30, as ESPHome writes) leaves the calibration measured without it in place | Readings off by a little. Check with a plug-in meter and recalibrate on the page. |
| 2PM: the overcurrent and total-current trips act as tested on the host | A trip comes early or late; the breaker remains the protection. |

---

## Related documentation

- [README](../README.md) — project overview and quick start
- [Why Matter over Thread](WHY.md) — the rationale for Matter over Thread
- [Flashing Guide](FLASHING.md) — wiring, backing up stock firmware, and flashing
- [Reversibility](REVERSIBILITY.md) — warranty, factory keys, and how reversible flashing is
- [Commissioning](COMMISSIONING.md) — pairing the device and reading the status LED
- [Updating](UPDATING.md) — keeping a device current after flashing
- [Power Consumption](POWER.md) — measured draw and the Thread Router design choice
- [Building from Source](BUILDING.md) — compiling the firmware yourself
- [GPIO Map](GPIO.md) — ESP-Shelly-C68F pin assignments for Gen4 devices
- [Certification](CERTIFICATION.md) — uncertified status and test credentials
- [Roadmap](ROADMAP.md) — current known limitations and planned work
- [Contributing](CONTRIBUTING.md) — reporting bugs and the firmware filename convention
- [Contributors](CONTRIBUTORS.md) — people who have helped move the project forward
