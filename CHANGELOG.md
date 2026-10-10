# Changelog

All notable changes to this firmware are recorded here. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows [semantic versioning](https://semver.org/spec/v2.0.0.html).

Each firmware variant versions independently, and releases are grouped by device and variant below. Switching a device between variants always requires full reset and re-commission, because each variant is a different Matter device type.

## Shelly 1 Gen4, all variants

### [2.2.0] - Unreleased

Light, Outlet and Opener 2.2.0; Light Switch 2.3.0. Robustness release: shared driver components, automatic rollback, a liveness watchdog, hardware-fault reporting, and fixes for pin hold, switch glitches and NVS recovery. Updates over Matter OTA without re-pairing.

#### Added
- Wall switch modes, set on the Wi-Fi mode page and saved with 3 presses of the button: Toggle (every change toggles, the default and the previous behaviour), Push button (a button that springs back toggles once per press; before, it switched on and straight back off), Follow (the relay matches the switch position) and Disabled. The Opener and Light Switch keep their own use of SW. See [Wall switch modes](docs/BUTTON-AND-LED.md#wall-switch-modes).
- The status LED blinks slowly and evenly (1 s on, 1 s off) while a Matter update downloads and installs, so you know not to unplug the device (LED-16).
- Each device has its own Matter setup code instead of the shared test code `3497-011-2332`: the code and QR on its Shelly sticker, read (never written) from Shelly's factory data in the stock `shelly` partition, and checked against its stored verifier. A device without a usable sticker code makes a random code the first time it starts and keeps it. The serial log shows the code at every start, and the Wi-Fi mode page shows it with its QR. On that page you can instead enter another 11-digit code or make a new random one, saved only after 3 presses of the button (B-26); typing the sticker's code goes back to it, and a factory reset forgets a code you entered. Devices already paired keep their pairings. Builds without Wi-Fi mode use the sticker code, or the test code without one. On one 2PM Gen4 the page showed the sticker's code, read from the device; pairing with it is not yet tested on hardware. See [Your setup code](docs/COMMISSIONING.md#your-setup-code).
- Wi-Fi maintenance mode (experimental). Hold the button 5 s and the device restarts with Matter and Thread off, runs its own Wi-Fi network (named like stock Shelly's, e.g. `Shelly1G4-<the MAC on the sticker>`, WPA2/WPA3 with the sticker's setup code as its password, or your own set on the page; open on a device without a sticker code) and serves an update page at 192.168.33.1, like stock Shelly firmware's 5 s hold. Upload the variant's `.ota` file from a phone or laptop: the page checks the file's model before erasing anything, can install an older version on request, and returns to normal after the upload, on Exit, on another 5 s hold, or after 10 minutes with no page open. Every image now carries its Shelly hardware code. The page has tabs (Home, Pairing, Switch, Meter, Update, Diagnostics; those a model lacks are hidden), works on phones, and follows the phone's light or dark mode. `/recovery` is a simple install page with no code shared with the main page. See [Updating](docs/UPDATING.md#wi-fi-maintenance-mode).
- Stock Shelly button gestures with LED feedback. Tap to toggle, press 3 times to open a Matter pairing window (3 minutes when paired, 15 when not), hold 10 s to factory reset, hold past 20 s to cancel. Nothing happens while the button is held: the LED shows what releasing will do (a ½ s blink at 5 s, a ¼ s blink at 10 s), and refused or accepted actions show their own flashes. The full specification, checked against the code by the host tests, is [Button, Switch and LED](docs/BUTTON-AND-LED.md).
- The pairing blink also shows when a pairing window is open on a paired device, and a short flash every 2 s shows an unpaired device whose pairing window has closed (press 3 times to reopen it; no power cycle needed).
- Automatic rollback. A new firmware image confirms itself after running for 60 seconds with the Matter stack responsive; one that crashes, hangs or restarts before then is rolled back by the bootloader. Needs a bootloader from this release or later (installed by the web UI or UART, never by Matter OTA), and does not protect the update from an older release. See [Updating](docs/UPDATING.md).
- Liveness watchdog. A health task feeds the task watchdog only while the Matter thread stays responsive; if the thread is stuck for 30 seconds, the device reboots within the 60-second watchdog timeout.
- Hardware faults and boot reason in General Diagnostics. `ActiveHardwareFaults` and `BootReason` on the root endpoint, and a `HardwareFaultChange` event. Over-temperature reports ResettableOverTemp; a missing temperature sensor reports Sensor. Watchdog and panic restarts report SoftwareWatchdogReset and SoftwareReset instead of Unspecified. In Home Assistant 2026.7.0 or later the faults appear as a "Hardware faults" sensor that is disabled by default. See [Protection and Metering](docs/PROTECTION.md).
- Status LED fault pattern (100ms on, 100ms off) while any fault is active. Identify still takes priority.
- Build-time reversibility check. The build fails if secure boot, flash encryption, eFuse anti-rollback or a ROM download lockout is enabled, if the partition table differs from the stock layout, or if any eFuse-writing function is linked (`scripts/check-reversibility.py`).

#### Changed
- A tap toggles when the button is released (about 0.3 s later), instead of on press. Holding the button no longer switches the load.
- A turn-on the relay refuses (over-temperature, failed temperature sensor, protection) now shows three quick flashes instead of doing nothing visible.
- The drivers moved into shared components under `source/components/`, used by every device and variant. Pins moved from source `#define`s to a "Shelly board" Kconfig menu in each variant. The Matter composition, endpoint ids and stored data are unchanged, so a Matter OTA preserves commissioning.
- Over-temperature now clears below 65°C instead of as soon as the die drops below 75°C, so a device at the threshold no longer flaps.
- A temperature sensor that cannot be read (install failure, or three failed reads in a row) raises a Sensor fault and refuses turn-on until a reading returns. It used to be ignored silently.
- Button presses are handled on their own task instead of the ESP timer task, and the timer task stack is 6144 bytes.

#### Fixed
- Pairing from Android now completes. The device didn't answer pings on its Thread address, and Android's Matter pairing gives up at "Checking connectivity to Thread network" without one, even though the device was reachable for Matter itself. It now answers ping on every Thread address. Found on a Shelly 1PM Gen4 behind an Aqara border router; it affected every variant.
- Factory reset fired after about 1.5 s instead of the documented 10 s. The configured 10000 ms was outside the button driver's allowed range, so the driver's 1500 ms default applied, and the same press also toggled the relay. Gesture timing is now the firmware's own.
- Relay and LED pins held by the stock firmware. Stock firmware can leave the relay and LED pads latched, and a held pad ignores writes until power is removed. The firmware now releases the hold at boot, after first driving the pin to its safe level.
- Wall switch glitches. A spike that returns the SW input to its previous level no longer counts as a flip; only a real change of the debounced level does.
- NVS boot loop. If the NVS partition is full or was written by a newer NVS format, it is erased and initialized again, and the device comes back up ready to commission, instead of failing on every boot.
- Unchecked task and queue creation. Failures are now detected: the thermal monitor refuses turn-on if its task cannot start, and the button and switch inputs report an error instead of running half started.
- Opener: the trigger is never asserted at boot. A StartUpOnOff of On or Toggle no longer pulses it while Matter starts, and a stored "on" state (a restart mid-pulse) no longer closes the relay with no pulse timer armed to release it. The pulse also releases on time when the Matter stack is busy.

### [2.1.0] - 2026-07-18

Temperature sensor endpoint across all four variants, and a thermal task stack fix.

#### Added
- Temperature sensor endpoint reporting the ESP32-C6 die temperature, updated on changes of 0.5°C or more. The reading is null until the first poll after the Matter stack starts.

#### Fixed
- Thermal monitor task stack overflow. Publishing readings through `attribute::update()` runs Matter reporting on the thermal task's stack, and the previous 2048-byte stack overflowed. The stack is now 4096 bytes, which also covers the thermal-fault OnOff update that has always shared this stack.

### [2.0.0] - 2026-06-25

Web UI install, Matter OTA updates, and a stock-aligned partition layout, the same change across all four variants. Within a variant's v1.x line the NVS schema is stable, and an in-place upgrade through the ESP-IDF CLI preserves commissioning. Moving from any v1.x build to 2.0.0 is the exception: the partition layout changed and requires a one-time full reflash that erases commissioning, after which Matter OTA preserves it for future updates.

#### Added
- Web UI install. A packaged `automatous-io-shelly-1-gen4-{variant}-vX.Y.Z-ota.zip` installs through the Shelly web UI, with no UART adapter required. Built by `scripts/make-webui-ota-zip.py`.
- Matter OTA updates. A `.ota` image updates a commissioned device over Thread through Home Assistant's Matter Server, preserving commissioning, Thread credentials, and ecosystem pairings. Built by `scripts/make-matter-ota.py`.

#### Changed
- Partition layout now follows the stock Shelly 1 Gen4 offsets, with A/B app slots so one `partitions.csv` serves both UART flashing and the Web UI OTA. Moving to it from any v1.x build is a one-time full reflash over UART that erases commissioning; updates from 2.0.0 onward preserve it. Drops the `fctry` and `phy_init` partitions, since the DAC is compiled in and PHY data lives in the app.
- Distinct Matter Product ID per variant (Light `0x8001`, Light Switch `0x8002`, Opener `0x8003`, Outlet `0x8004`); Matter OTA images can only target the variant it was built for.

## Shelly 1 Gen4 Light

### [1.2.1] - 2026-05-23

Module refactor, safety contract, and earlier init. No user-visible behavior change.

#### Added
- Documented relay safety contract. All relay control flows through a single `relay_set()` function with an auditable invariant: pre-init safe-low, single-point thermal lockout, Thread-loss hold, and panic recovery.

#### Changed
- Refactored the codebase into per-concern modules (`button`, `thermal`, `switch_input`, `relay`) alongside `status_led`, each logging under its own tag.
- Moved subsystem initialization before `esp_matter::start()`, so the relay reaches its safe-low state earlier in boot and thermal protection is active before the Matter stack starts.

### [1.2.0] - 2026-05-14

Identify cluster and Multi-Admin LED fix.

#### Added
- Matter Identify cluster. The status LED blinks 100ms on and 900ms off for the requested duration, then returns to its previous state.
- Hardware version reporting of `v0.1.2`, matching the PCB silkscreen, in place of the SDK test default.

#### Fixed
- Multi-Admin LED state. The LED no longer drops into the BLE advertising blink when a commissioning window opens on an already-commissioned device. It now stays solid through Multi-Admin commissioning.

#### Changed
- Pinned `CONFIG_OPENTHREAD_FTD` explicitly rather than relying on an inherited value.

### [1.1.0] - 2026-05-11

Status LED support.

#### Added
- Status LED indication with four patterns: off while starting up, rapid blink while BLE advertising, slow blink while joining Thread, and solid when connected.

#### Changed
- Release tags now include model, generation, and variant (`shelly-1-gen4-light-vX.Y.Z`) to support future variants in the same repository.

#### Notes
- The status LED is on GPIO15 with active-low polarity, determined by GPIO sweep testing on real hardware, not GPIO0 as ESPHome documentation states.

### [1.0.0] - 2026-05-09

First public release. Matter On/Off Light device type with latching relay behavior.

#### Added
- Matter over Thread. The device joins your Thread mesh and is controlled directly over Matter.
- Multi-fabric commissioning to Apple Home, Home Assistant, and Google Home at the same time.
- Physical button toggle on the onboard relay button.
- External wall switch input on the SW terminal, hardware-debounced.
- State sync across all controllers.
- Thread Router mode to extend your Thread mesh for other devices.
- Factory reset via long press of the onboard button.
- Verified stock firmware backup and restore.

## Shelly 1 Gen4 Opener

### [1.0.1] - 2026-06-22

Closes a fail-open gap in the momentary pulse. No change to normal operation.

#### Fixed
- Pulse fail-open on timer-arm failure. If the one-shot auto-off timer could not be armed, for example under heap pressure during Matter, Thread, and BLE activity, the relay could remain energized instead of pulsing. The firmware now arms the auto-off before energizing and only closes the relay once its release is guaranteed, so a failed arm leaves the trigger de-asserted rather than held. The fix covers both timer creation and timer start. Reported by @456789TZ.

### [1.0.0] - 2026-06-16

First release of the Opener variant. Matter On/Off Plug-in Unit plus a Contact Sensor, built on the Light v1.2.1 module foundation and relay safety contract.

#### Added
- Momentary pulse relay. Turning the device on energizes the relay for a fixed pulse, then firmware returns it to off. The pulse is guaranteed in firmware through `relay_set()`, so a dropped command or a crashed hub cannot leave the relay energized. Default pulse width is 500ms.
- Door contact sensor on the SW terminal (GPIO10), exposed as a Matter Contact Sensor and reported to every commissioned fabric. In this variant the SW terminal is a sensor input, not a wall toggle.
- Three-endpoint Matter composition: an On/Off Plug-in Unit for the relay, a Contact Sensor for door state, and the Root Node.
- Compile-time constants in `main/app_priv.h`: `OPENER_PULSE_MS` (default 500) for pulse width and `CONTACT_OPEN_IS_HIGH` (default 0) for reed polarity.

## Shelly 1 Gen4 Outlet

### [1.0.0] - 2026-06-19

First release of the Outlet variant. Matter On/Off Plug-in Unit with latching relay behavior, built on the Light v1.2.1 module foundation and relay safety contract.

#### Added
- Latching relay presented as a Matter On/Off Plug-in Unit, so smart home apps display and voice-control the device as an outlet rather than a light. Electrically identical to the Light variant.
- External wall switch input on the SW terminal (GPIO10), hardware-debounced, toggling the relay as on the Light variant.
- Two-endpoint Matter composition: an On/Off Plug-in Unit for the relay, and the Root Node.

## Shelly 1 Gen4 Light Switch

### [2.2.0] - 2026-07-29

Wall switch state exposure. The toggle position on the SW terminal is now visible to controllers as a contact sensor.

#### Added
- Wall switch state on the SW input, exposed as a Matter Contact Sensor endpoint. The sensor tracks the physical toggle only; commands to bound devices have no effect on the physical wall switch state. In Home Assistant it appears as a binary sensor that is on while the toggle is on, labeled "Open" under the default 'Show as' door; changing to power renders it as On and Off. Apple Home shows Open and Closed with a few other options. Requested by @MetsGER84.
- Compile-time constant `SWITCH_ON_IS_HIGH` (default 0) in `main/app_priv.h`, mapping the SW input level to the reported state.

#### Changed
- `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` raised from 4 to 5 for the new endpoint. The contact sensor is created after the existing endpoints; endpoint ids stay stable and a Matter OTA from 2.1.0 adds the sensor without re-pairing.

### [1.0.0] - 2026-06-20

First release of the Light Switch variant, contributed by [Tomas McGuinness](https://github.com/tomasmcguinness). Derived from the Light 1.2.0 codebase, it adds detached-relay support so the SW terminal controls other Matter devices instead of the local relay.

#### Added
- Detached relay. The SW terminal (GPIO10) no longer drives the local relay. It sends a Matter OnOff Toggle command through a second endpoint's binding, so the wall switch can control other Matter devices such as bulbs, groups, or scenes. With no binding configured, flipping the switch does nothing locally.
- On/Off Light Switch endpoint driven by the SW input, with binding dispatch for both unicast and group bound commands.
- Two-endpoint Matter composition: an On/Off Light for the relay, controlled by app, voice, and the onboard button, and an On/Off Light Switch for the SW input, plus the Root Node.

## Shelly 1 Mini Gen4 Outlet

### [1.1.0] - Unreleased

The robustness changes of Shelly 1 Gen4 2.2.0, on the Mini. Updates over Matter OTA without re-pairing.

#### Added
- Wall switch modes, set on the Wi-Fi mode page and saved with 3 presses of the button: Toggle (every change toggles, the default and the previous behaviour), Push button (a button that springs back toggles once per press; before, it switched on and straight back off), Follow (the relay matches the switch position) and Disabled. The Opener and Light Switch keep their own use of SW. See [Wall switch modes](docs/BUTTON-AND-LED.md#wall-switch-modes).
- The status LED blinks slowly and evenly (1 s on, 1 s off) while a Matter update downloads and installs, so you know not to unplug the device (LED-16).
- Each device has its own Matter setup code instead of the shared test code `3497-011-2332`: the code and QR on its Shelly sticker, read (never written) from Shelly's factory data in the stock `shelly` partition, and checked against its stored verifier. A device without a usable sticker code makes a random code the first time it starts and keeps it. The serial log shows the code at every start, and the Wi-Fi mode page shows it with its QR. On that page you can instead enter another 11-digit code or make a new random one, saved only after 3 presses of the button (B-26); typing the sticker's code goes back to it, and a factory reset forgets a code you entered. Devices already paired keep their pairings. Builds without Wi-Fi mode use the sticker code, or the test code without one. On one 2PM Gen4 the page showed the sticker's code, read from the device; pairing with it is not yet tested on hardware. See [Your setup code](docs/COMMISSIONING.md#your-setup-code).
- Wi-Fi maintenance mode (experimental). Hold the button 5 s and the device restarts with Matter and Thread off, runs its own Wi-Fi network (named like stock Shelly's, e.g. `Shelly1MiniG4-<the MAC on the sticker>`, WPA2/WPA3 with the sticker's setup code as its password, or your own set on the page; open on a device without a sticker code) and serves an update page at 192.168.33.1, like stock Shelly firmware's 5 s hold. Upload the variant's `.ota` file from a phone or laptop: the page checks the file's model before erasing anything, can install an older version on request, and returns to normal after the upload, on Exit, on another 5 s hold, or after 10 minutes with no page open. Every image now carries its Shelly hardware code. The page has tabs (Home, Pairing, Switch, Meter, Update, Diagnostics; those a model lacks are hidden), works on phones, and follows the phone's light or dark mode. `/recovery` is a simple install page with no code shared with the main page. See [Updating](docs/UPDATING.md#wi-fi-maintenance-mode).
- Stock Shelly button gestures with LED feedback. Tap to toggle, press 3 times to open a Matter pairing window (3 minutes when paired, 15 when not), hold 10 s to factory reset, hold past 20 s to cancel. Nothing happens while the button is held: the LED shows what releasing will do (a ½ s blink at 5 s, a ¼ s blink at 10 s), and refused or accepted actions show their own flashes. The full specification, checked against the code by the host tests, is [Button, Switch and LED](docs/BUTTON-AND-LED.md).
- The pairing blink also shows when a pairing window is open on a paired device, and a short flash every 2 s shows an unpaired device whose pairing window has closed (press 3 times to reopen it; no power cycle needed).
- Automatic rollback. A new firmware image confirms itself after running for 60 seconds with the Matter stack responsive; one that crashes, hangs or restarts before then is rolled back by the bootloader. Needs a bootloader from this release or later (installed by the web UI or UART, never by Matter OTA), and does not protect the update from an older release. See [Updating](docs/UPDATING.md).
- Liveness watchdog. A health task feeds the task watchdog only while the Matter thread stays responsive; if the thread is stuck for 30 seconds, the device reboots within the 60-second watchdog timeout.
- Hardware faults and boot reason in General Diagnostics. `ActiveHardwareFaults` and `BootReason` on the root endpoint, and a `HardwareFaultChange` event. Over-temperature reports ResettableOverTemp; a missing temperature sensor reports Sensor. Watchdog and panic restarts report SoftwareWatchdogReset and SoftwareReset instead of Unspecified. In Home Assistant 2026.7.0 or later the faults appear as a "Hardware faults" sensor that is disabled by default. See [Protection and Metering](docs/PROTECTION.md).
- Status LED fault pattern (100ms on, 100ms off) while any fault is active. Identify still takes priority.
- Build-time reversibility check. The build fails if secure boot, flash encryption, eFuse anti-rollback or a ROM download lockout is enabled, if the partition table differs from the stock layout, or if any eFuse-writing function is linked (`scripts/check-reversibility.py`).

#### Changed
- A tap toggles when the button is released (about 0.3 s later), instead of on press. Holding the button no longer switches the load.
- A turn-on the relay refuses (over-temperature, failed temperature sensor, protection) now shows three quick flashes instead of doing nothing visible.
- The drivers moved into shared components under `source/components/`, used by every device and variant. Pins moved from source `#define`s to a "Shelly board" Kconfig menu in each variant. The Matter composition, endpoint ids and stored data are unchanged, so a Matter OTA preserves commissioning.
- Over-temperature now clears below 65°C instead of as soon as the die drops below 75°C, so a device at the threshold no longer flaps.
- A temperature sensor that cannot be read (install failure, or three failed reads in a row) raises a Sensor fault and refuses turn-on until a reading returns. It used to be ignored silently.
- Button presses are handled on their own task instead of the ESP timer task, and the timer task stack is 6144 bytes.

#### Fixed
- Pairing from Android now completes. The device didn't answer pings on its Thread address, and Android's Matter pairing gives up at "Checking connectivity to Thread network" without one, even though the device was reachable for Matter itself. It now answers ping on every Thread address. Found on a Shelly 1PM Gen4 behind an Aqara border router; it affected every variant.
- Factory reset fired after about 1.5 s instead of the documented 10 s. The configured 10000 ms was outside the button driver's allowed range, so the driver's 1500 ms default applied, and the same press also toggled the relay. Gesture timing is now the firmware's own.
- Relay and LED pins held by the stock firmware. Stock firmware can leave the relay and LED pads latched, and a held pad ignores writes until power is removed. The firmware now releases the hold at boot, after first driving the pin to its safe level.
- Wall switch glitches. A spike that returns the SW input to its previous level no longer counts as a flip; only a real change of the debounced level does.
- NVS boot loop. If the NVS partition is full or was written by a newer NVS format, it is erased and initialized again, and the device comes back up ready to commission, instead of failing on every boot.
- Unchecked task and queue creation. Failures are now detected: the thermal monitor refuses turn-on if its task cannot start, and the button and switch inputs report an error instead of running half started.

### [1.0.0] - 2026-07-06

First release for the Shelly 1 Mini Gen4, the first supported device beyond the Shelly 1 Gen4. Matter On/Off Plug-in Unit with latching relay behavior, at parity with the 1 Gen4 Outlet, plus a temperature sensor.

#### Added
- Latching relay presented as a Matter On/Off Plug-in Unit, on the Mini's own GPIO map, confirmed on hardware: relay GPIO10, button GPIO22, wall switch input GPIO12, status LED GPIO5.
- Temperature sensor endpoint reporting the ESP32-C6 die temperature to every commissioned fabric, updated on changes of 0.5°C or more.
- Three-endpoint Matter composition: an On/Off Plug-in Unit for the relay, a Temperature Sensor, and the Root Node.
- Distinct Matter Product ID (`0x8005`), so a Matter OTA image can only target this device and variant.

## Shelly 1 Mini Gen4 Light Switch

### [1.0.0] - 2026-09-26

The Light Switch variant on the Shelly 1 Mini Gen4, at parity with the 1 Gen4 Light Switch 2.2.0: detached relay, wall switch state as a contact sensor, and a temperature sensor. Requested by @zididadaday in #37.

#### Added
- Detached relay on the Mini's GPIO map, confirmed on hardware: relay GPIO10, button GPIO22, wall switch input GPIO12, status LED GPIO5. The SW terminal sends a Matter OnOff Toggle through the On/Off Light Switch endpoint's binding and does not drive the local relay. The relay is controlled by app and the onboard button.
- Wall switch state on the SW input, exposed as a Matter Contact Sensor endpoint. `SWITCH_ON_IS_HIGH` stays at 0; the GPIO12 level maps the same way as GPIO10 on the 1 Gen4, confirmed on hardware, and the state is read at boot.
- Temperature sensor endpoint reporting the ESP32-C6 die temperature, with the shared 75°C thermal cutoff.
- Five-endpoint Matter composition: an On/Off Light for the relay, an On/Off Light Switch for the SW input, a Temperature Sensor, a Contact Sensor, and the Root Node.
- Distinct Matter Product ID (`0x8006`); Matter OTA image can only target this device and variant.

## Shelly 1PM Gen4 Outlet

### [1.0.2] - Unreleased

Meter calibration from the Wi-Fi mode page. **Experimental: built and unit-tested, not yet run on hardware.** Updates over Matter OTA or from the Wi-Fi mode page, keeping the pairing.

#### Added
- The meter on the Wi-Fi mode page: live readings, and calibration against your own plug-in meter, saved with 3 presses of the button (B-26), kept through updates and factory resets, and bounded to 15% of the built-in calibration. See [Calibrating the meter](docs/PROTECTION.md#calibrating-the-meter).

#### Changed
- The setup code is now the one on the Shelly sticker (and its QR), read from Shelly's factory data, instead of the code the device made: re-pairing or adding it to another ecosystem uses the sticker code. A code you entered on the page is kept until a factory reset. Pairings are kept. Not yet tested on hardware. See [Your setup code](docs/COMMISSIONING.md#your-setup-code).
- Electrical protection, the relay layer and metering are shared with the two-relay Shelly 2PM Gen4 and work per relay. The 1PM's protection behaves as before.
- If a Wi-Fi mode boot stops unexpectedly or fails to start while the meter runs, later Wi-Fi mode boots leave the meter off until the next power cycle, so a meter fault can't stop you installing a fix. Overcurrent and overvoltage protection are off in those Wi-Fi mode boots, so the relays start off there.
- The Wi-Fi mode network is named like stock Shelly's: `Shelly<model>-<the 12-digit MAC on the sticker>`, e.g. `Shelly1PMG4-…`, instead of `Shelly-S1PMG4-<last six digits>`.
- `http://192.168.33.1/recovery` in Wi-Fi mode: a simple install page with no code shared with the main page, so a page bug can't stop a firmware install.
- The Wi-Fi mode page is rebuilt with tabs: Home (relays with live watts, device), Pairing, Switch, Meter, Update and Diagnostics. It works on phones, follows the phone's light or dark mode, and asks for a second tap before Exit restarts the device. A change waiting for 3 presses shows on every tab. The device serves the page gzipped through a small JSON API; firmware upload is unchanged. If the page fails to start, it links to `/recovery`.

### [1.0.1] - Unreleased

First release for the Shelly 1PM Gen4 (UL, 15 A / 120 V). **Experimental: built and unit-tested, not yet verified on hardware.** See the [1PM checklist](docs/FLASHING.md#shelly-1pm-gen4-checklist) before flashing.

#### Added
- Wall switch modes, set on the Wi-Fi mode page and saved with 3 presses of the button: Toggle (every change toggles, the default and the previous behaviour), Push button (a button that springs back toggles once per press; before, it switched on and straight back off), Follow (the relay matches the switch position) and Disabled. The Opener and Light Switch keep their own use of SW. See [Wall switch modes](docs/BUTTON-AND-LED.md#wall-switch-modes).
- The status LED blinks slowly and evenly (1 s on, 1 s off) while a Matter update downloads and installs, so you know not to unplug the device (LED-16).
- Each device has its own Matter setup code instead of the shared test code `3497-011-2332`: the code and QR on its Shelly sticker, read (never written) from Shelly's factory data in the stock `shelly` partition, and checked against its stored verifier. A device without a usable sticker code makes a random code the first time it starts and keeps it. The serial log shows the code at every start, and the Wi-Fi mode page shows it with its QR. On that page you can instead enter another 11-digit code or make a new random one, saved only after 3 presses of the button (B-26); typing the sticker's code goes back to it, and a factory reset forgets a code you entered. Devices already paired keep their pairings. Builds without Wi-Fi mode use the sticker code, or the test code without one. On one 2PM Gen4 the page showed the sticker's code, read from the device; pairing with it is not yet tested on hardware. See [Your setup code](docs/COMMISSIONING.md#your-setup-code).
- Wi-Fi maintenance mode (experimental). Hold the button 5 s and the device restarts with Matter and Thread off, runs its own Wi-Fi network (named like stock Shelly's, e.g. `Shelly1PMG4-<the MAC on the sticker>`, WPA2/WPA3 with the sticker's setup code as its password, or your own set on the page; open on a device without a sticker code) and serves an update page at 192.168.33.1, like stock Shelly firmware's 5 s hold. Upload the variant's `.ota` file from a phone or laptop: the page checks the file's model before erasing anything, can install an older version on request, and returns to normal after the upload, on Exit, on another 5 s hold, or after 10 minutes with no page open. Every image now carries its Shelly hardware code. See [Updating](docs/UPDATING.md#wi-fi-maintenance-mode).
- Stock Shelly button gestures with LED feedback. Tap to toggle, press 3 times to open a Matter pairing window (3 minutes when paired, 15 when not), hold 10 s to factory reset, hold past 20 s to cancel. Nothing happens while the button is held: the LED shows what releasing will do (a ½ s blink at 5 s, a ¼ s blink at 10 s), and refused or accepted actions show their own flashes. The full specification, checked against the code by the host tests, is [Button, Switch and LED](docs/BUTTON-AND-LED.md).
- The pairing blink also shows when a pairing window is open on a paired device, and a short flash every 2 s shows an unpaired device whose pairing window has closed (press 3 times to reopen it; no power cycle needed).
- Latching relay presented as a Matter On/Off Plug-in Unit, on the 1PM's GPIO map: relay GPIO4, button GPIO1, wall switch input GPIO10, status LED GPIO11.
- Power metering from the BL0942 meter (UART1, 9600 baud) as the Matter Electrical Sensor device type on the outlet endpoint: active power, voltage, current and frequency (Electrical Power Measurement, AC), and imported cumulative energy (Electrical Energy Measurement). Metering is advertised only once the device has seen AC mains.
- Energy total saved to flash every 15 minutes or 100 Wh, continuing across restarts and firmware updates.
- Overcurrent protection with an I²t heat model: no trip at or below 16.5 A, 54s at 18 A, 4.6s at the meter's 29.7 A full scale. Trips latch until a deliberate On or a power cycle, and survive a software restart.
- Overvoltage protection above 150 V (120 V mains) or 264 V (230/240 V mains), sustained 5 seconds, with automatic restore 30 seconds after the voltage returns.
- Board NTC temperature, with a trip at 90°C and the die sensor as a fallback.
- Welded-relay and meter-lost alerts.
- All the robustness features of Shelly 1 Gen4 2.2.0.
- Distinct Matter Product ID (`0x8006`).

#### Fixed
- Pairing from Android now completes. The device didn't answer pings on its Thread address, and Android's Matter pairing gives up at "Checking connectivity to Thread network" without one, even though the device was reachable for Matter itself. It now answers ping on every Thread address. Found on a Shelly 1PM Gen4 behind an Aqara border router; it affected every variant.

---

Full release notes, including tested scenarios, are on the [GitHub releases page](../../releases).

## Shelly 2PM Gen4 Outlet and Light

### [1.0.1] - Unreleased

Power metering and electrical protection, and meter calibration from the Wi-Fi mode page. **Experimental: built and unit-tested, not yet run on hardware.** Updates a 1.0.0 device from the Wi-Fi mode page, or over Matter OTA without re-pairing (Home Assistant may need to re-interview the device to show the new meter entities).

#### Added
- Power metering per relay with the ADE7953: power, voltage, current, frequency and imported energy on each relay's endpoint (the Matter Electrical Sensor device type), as on the 1PM. The calibration was measured on one 2PM against a plug-in meter with a 389 W heater on each relay. Energy is added up from the power readings, and an idle relay reads 0 instead of the meter's noise. See [Power metering](docs/PROTECTION.md#power-metering).
- Electrical protection per relay: an overcurrent heat model scaled to Shelly's 10 A per relay (nothing below 11 A, 54 s at 12 A, 12 s at 15 A), latched per relay; a second model on the two relays' sum, scaled to the 16 A device rating, that turns both relays off; overvoltage; and welded-relay and meter-lost alerts. See [Protection and Metering](docs/PROTECTION.md).
- The meter on the Wi-Fi mode page: live readings of each relay, and calibration against your own plug-in meter, saved with 3 presses of the button (B-26), kept through updates and factory resets, and bounded to 15% of the built-in calibration. The 1PM has the same page. See [Calibrating the meter](docs/PROTECTION.md#calibrating-the-meter). If a Wi-Fi mode boot stops unexpectedly or fails to start while the meter runs, later Wi-Fi mode boots leave the meter off until the next power cycle, so a meter fault can't stop you installing a fix. Overcurrent and overvoltage protection are off in those Wi-Fi mode boots, so the relays start off there.

#### Changed
- The setup code is now the one on the Shelly sticker (and its QR), read from Shelly's factory data, instead of the code the device made: re-pairing or adding it to another ecosystem uses the sticker code. A code you entered on the page is kept until a factory reset. Pairings are kept. Not yet tested on hardware. See [Your setup code](docs/COMMISSIONING.md#your-setup-code).
- The meter chip is set up as its datasheet asks after every reset (register 0x120); the firmware writes nothing else to it. The read-only probe of 1.0.0 is gone: the meter driver feeds the hardware snapshot instead.
- Electrical protection and the relay layer work per relay, shared with the 1PM, whose behaviour is unchanged.
- The Wi-Fi mode network is named like stock Shelly's: `Shelly<model>-<the 12-digit MAC on the sticker>`, e.g. `Shelly2PMG4-…`, instead of `Shelly-S2PMG4-<last six digits>`.
- `http://192.168.33.1/recovery` in Wi-Fi mode: a simple install page with no code shared with the main page, so a page bug can't stop a firmware install.
- The Wi-Fi mode page is rebuilt with tabs: Home (relays with live watts, device), Pairing, Switch, Meter, Update and Diagnostics. It works on phones, follows the phone's light or dark mode, and asks for a second tap before Exit restarts the device. A change waiting for 3 presses shows on every tab. The device serves the page gzipped through a small JSON API; firmware upload is unchanged. If the page fails to start, it links to `/recovery`.

### [1.0.0] - 2026-10-08

First builds for the Shelly 2PM Gen4. **Experimental bring-up builds.** Installed on one 2PM Gen4 (S4SW-002P16EU) from stock 2.0.1 with the web UI: Wi-Fi mode, the button (active low), both relays (their pins read back as written), both switch inputs, the NTC (about 34 °C at idle) and the ADE7953 (version 0x01 at 0x38; channel A measures relay 1, channel B relay 2) all worked, and the meter data calibrated the 1.0.1 build. See the [2PM checklist](docs/FLASHING.md#shelly-2pm-gen4-checklist) before installing.

#### Added
- Two channels: relay 1 (O1) on Matter endpoint 1 and relay 2 (O2) on endpoint 2, as two On/Off Plug-in Units (Outlet) or two On/Off Lights (Light), plus the board temperature on endpoint 3. Pins from the verified 2PM map: relays GPIO5 and GPIO3, SW1 GPIO11, SW2 GPIO10, button GPIO12, LED GPIO18, NTC GPIO4.
- The button: a tap toggles relay 1, 2 taps toggle relay 2 (B-30). Every other gesture is as on the other devices.
- Each wall switch input drives its own relay in its own mode (Toggle, Push button, Follow, Disabled), set separately on the Wi-Fi mode page (SW-10 to SW-12).
- Over-temperature protection on the board NTC (trip at 90°C, clear below 75°C), with the die sensor as a fallback. A trip turns both relays off.
- Board bring-up data on the Wi-Fi mode page: the diagnostics log, and a hardware snapshot of the last normal run (relay and input pins, button, temperatures, memory) at `/hw.json`. A read-only probe of the ADE7953 power meter, from 30 s after Matter starts, records its identity, configuration and raw readings there too, with estimates in volts, amps and watts. It never writes to the chip. Metering itself comes in a later build.

#### Not in this build
- No power metering, and no overcurrent or overvoltage protection. Rely on the circuit breaker.
- No cover (roller shutter) mode: the outputs are two independent switches with no interlock. Don't use these builds on a 2PM wired to a motor.

