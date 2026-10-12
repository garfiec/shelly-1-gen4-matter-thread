# Roadmap

**[README](../README.md)** > **Roadmap** · [Report an issue](../../../issues/new)

This document tracks the current known limitations and the planned work. To prioritize something, vote on the [roadmap discussion](../../../discussions) or open an issue with your use case.

---

## Contents

- [Known limitations](#known-limitations)
- [Planned](#planned)

---

## Known limitations

- Matter OTA updates are supported and documented in Home Assistant only. Because the firmware uses test credentials, the assumption is that no other major Matter ecosystem will serve OTA updates to it. Commissioning-preserving updates run through Home Assistant's Matter Server. See [Updating](UPDATING.md).
- Thermal protection was validated on the Shelly 1 Mini Gen4 with the earlier firmware, which cut the relay above 75°C sustained for 10 seconds and allowed turn-on again as soon as the die was back at 75°C: the trip, the latch that blocks turn-on while hot, and the automatic recovery were tested on Shelly 1 Mini Gen4 hardware. The current shared thermal code keeps the 75°C, 10-second trip and adds a clear level (turn-on again below 65°C) and a sensor-failure fault; that version has not run on hardware yet. The trip logic is shared across all devices and variants.
- Shelly 2PM Gen4 support is experimental. The 1.0.0 bring-up build ran on one unit: Wi-Fi mode, the button, both relays and switch inputs, the NTC and the power meter's readings worked, and calibrated the meter. The 1.0.1 metering build (per-relay metering, overcurrent, total-current and overvoltage protection, the calibration page) has not run on hardware yet. See the [2PM checklist](FLASHING.md#shelly-2pm-gen4-checklist).
- Shelly 1PM Gen4 support is experimental. Power metering, overcurrent, overvoltage and NTC over-temperature protection are implemented and unit-tested but have not run on hardware. See [Protection and Metering](PROTECTION.md#what-has-not-been-verified).

---

## Planned

- Shelly 1PM Gen4 hardware verification: pin map on the UL unit, meter calibration, NTC beta, and a trip test on a bench load.
- Shelly 2PM Gen4: hardware verification of the 1.0.1 metering build, then the Light Switch and Cover (window covering) variants.
- Shelly's own factory meter calibration, so each device could use its own calibration instead of the reference unit's. On one 2PM it was not in the stock `shelly` partition (which holds Matter attestation, the sticker setup code and factory identity) or in stock's file system. If Shelly keeps it per unit, it is in stock's `nvs`, which the web UI package erases on install: finding it means keeping that partition on a fresh device. The calibration page stays for owners who want to match their own meter.
- Stock features worth bringing back, from a capture of the stock 1PM Gen4 firmware. First, factory reset and Wi-Fi mode from the wall switch, for devices whose button is out of reach inside a wall box. See [Stock Firmware Features](STOCK-FEATURES.md#candidates-by-priority).
- Shelly Flood Gen4 support.
- NTC on the Shelly 1 Mini Gen4 and other devices that carry one (the Shelly 1PM Gen4 already uses its NTC).
- A Sleepy End Device variant for battery and solar constrained installs, trading mesh extension for lower power.
- Build ergonomics. Migrate the build to consume esp-matter through the component library instead of a separately installed SDK, and fold the C6 and Thread settings into the base sdkconfig.defaults, so a plain `idf.py build` works without extra setup.

---

## Related documentation

- [README](../README.md) — project overview and quick start
- [Why Matter over Thread](WHY.md) — the rationale for Matter over Thread
- [Flashing Guide](FLASHING.md) — wiring, backing up stock firmware, and flashing
- [Reversibility](REVERSIBILITY.md) — warranty, factory keys, and how reversible flashing is
- [Commissioning](COMMISSIONING.md) — pairing the device and reading the status LED
- [Updating](UPDATING.md) — keeping a device current after flashing
- [Power Consumption](POWER.md) — measured draw and the Thread Router design choice
- [Protection and Metering](PROTECTION.md) — over-temperature, overcurrent and overvoltage protection, power metering, and hardware faults
- [Building from Source](BUILDING.md) — compiling the firmware yourself
- [GPIO Map](GPIO.md) — ESP-Shelly-C68F pin assignments for Gen4 devices
- [Certification](CERTIFICATION.md) — uncertified status and test credentials
- [Contributing](CONTRIBUTING.md) — reporting bugs and the firmware filename convention
- [Contributors](CONTRIBUTORS.md) — people who have helped move the project forward
