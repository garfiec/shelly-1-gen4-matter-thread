# Commissioning

**[README](../README.md)** > **Commissioning** · [Report an issue](../../../issues/new)

This guide covers pairing a freshly flashed Shelly Gen4 device to your smart home ecosystem, sharing one device across multiple ecosystems, reading the status LED, troubleshooting a stalled commissioning, and factory resetting the device. It notes the Opener variant's differences where they apply. If you have not flashed the firmware yet, start with the [Flashing Guide](FLASHING.md). Once it is commissioned, [updates are available](UPDATING.md) through Home Assistant with Matter OTA, with commissioning preserved.

---

## Contents

- [Status LED reference](#status-led-reference)
- [Your setup code](#your-setup-code)
- [Before commissioning](#before-commissioning)
- [Apple Home](#apple-home)
- [Google Home, Alexa, Home Assistant](#google-home-alexa-home-assistant)
- [Commissioning to multiple ecosystems](#commissioning-to-multiple-ecosystems)
- [Opener variant](#opener-variant)
- [Troubleshooting](#troubleshooting)
- [Factory reset](#factory-reset)

---

## Status LED reference

> **Button quick card.** Tap: on/off · Press 3×: pairing · Hold 5 s: Wi-Fi mode · Hold 10 s: factory reset · Keep holding past 20 s: cancel

The complete button and LED specification is [Button, Switch and LED](BUTTON-AND-LED.md): every gesture in every state, and every LED pattern. The patterns you'll see while pairing:

| LED pattern | Meaning |
|---|---|
| Off | Device starting up |
| Rapid blink (200 ms on, 200 ms off) | Pairing window open, ready to commission |
| Short flash every 2 s | Not paired, and the pairing window has closed. Press the button 3 times to open it again. |
| Slow blink (500 ms on, 1500 ms off) | Joining the Thread network |
| Solid on | Connected and working |
| Very fast blink (100 ms on, 100 ms off) | A hardware fault is active. See [Protection and Metering](PROTECTION.md). |

---

## Your setup code

Each device has its own Matter setup code, an 11-digit number like `1234-567-8901`. The device makes it the first time it starts, and keeps it through factory resets and updates. Nobody else's device shares it.

There's no sticker for it, so read it from the device before you pair. Either:
- **In the flashing log.** Right after a UART flash, the serial monitor shows a line like `Matter setup code: 1234-567-8901 (made by this device, NEW on this boot)` at every start.
- **On the Wi-Fi mode page.** Hold the button 5 s, join the `Shelly-…` network and read the code on the page (see [Updating](UPDATING.md#wi-fi-maintenance-mode)). Then press **Exit**: the device restarts with a 15-minute pairing window.

**Use your own code instead.** On the same page you can enter the 11-digit code from the Shelly sticker (or any valid Matter code), or make a new random one. It is only saved once you press the device's button 3 times within 2 minutes, so someone else on the open network can't change it. Enter the 11-digit number, not the sticker's QR code: the QR names Shelly as the maker, and some controllers refuse a device that reports a different one.

**When the code changes on its own.** The device makes a new code whenever its settings memory is erased: after a full UART reflash, or if the settings memory was unusable and the device had to erase it. Both also remove the device from your controllers. The log and the page always show the current code.

**When it's the shared test code instead.** In these cases the device uses the test code below, which every device running this firmware shares:
- a build without Wi-Fi mode, which has no page to show another code;
- if the device couldn't save its own code (it says so in the log and on the page, and tries again at the next start);
- after an update rolls back to firmware older than this feature.

![Matter setup QR code for the shared test code](images/matter_qrcode_20202021_3840.png)

```
Shared test code: 3497-011-2332
```

> This is the ESP-Matter SDK test code. With it, pair devices one at a time: several unpaired devices advertising the same code confuse controllers. See [Certification](CERTIFICATION.md) for what the uncertified warning means.

The code is used whenever a pairing window opens: the one after power-on (unpaired device), 3 presses of the button, and the window that opens after the device is removed from its last controller. Pairings you already have don't depend on it.

---

## Before commissioning

After commissioning, disconnect the UART from the Shelly and install it wherever you need it.

---

## Apple Home

Enter [your setup code](#your-setup-code) manually.

1. Open the Home app.
2. Tap the plus button, then Add Accessory.
3. Tap More Options, then enter the setup code.
4. Press Add Anyway when prompted. This is expected. See [Certification](CERTIFICATION.md) for why.
5. Name the device and place it in a room.

> Note for users coming from HomeKit Bridge or Home Assistant setups. HomeKit's reclassify as Fan, Light, or Switch option, available when bridging a stock Shelly through Home Assistant, is not available with native Matter devices. The Matter device type is declared by the firmware at flash time and cannot be changed in the Apple Home app. If you need a different device type, flash the [variant](../README.md#variants) that provides it.

---

## Google Home, Alexa, Home Assistant

Google Home and Alexa support Matter commissioning through their respective apps. Enter [your setup code](#your-setup-code) when prompted. Google and Alexa hubs have not been directly tested, but the firmware should work with any Matter-compliant hub. Let us know and we will update the compatible hubs section.

Home Assistant commissions through its Matter integration. Use the same setup code. Because this firmware uses Matter test credentials, Home Assistant's Matter Server needs Test Net DCL enabled before it will commission an uncertified device like this one.

Turn on **Enable test-net DCL usage** in the Matter Server add-on configuration and restart the add-on. This is the same setting that later lets Home Assistant serve [Matter OTA updates](UPDATING.md), so enabling it now covers both commissioning and updates.

Verified on **Matter Server add-on, version 9.0.2**.

---

## Commissioning to multiple ecosystems

Matter lets one device belong to several ecosystems at the same time. This is called multi-fabric, or Multi-Admin. You do not run the initial setup again for each one. Instead, an ecosystem the device already belongs to opens a short pairing window and hands out a fresh, temporary setup code that the next ecosystem uses to join.

1. Commission the device to your first ecosystem as described above.
2. In that ecosystem, open the device and choose the option to add it to another platform. In Apple Home this is "Turn On Pairing Mode" in the accessory settings. In Home Assistant it is the share or "Add to another fabric" action on the device page.
3. The first ecosystem displays a temporary setup code.
4. In the second ecosystem's app, start adding a Matter device and enter that temporary code, not the device's own setup code.
5. Repeat for any further ecosystems.

While the pairing window is open the status LED shows the rapid pairing blink, and it returns to solid when the window closes. You can also open a 3-minute window from the device itself: press the button 3 times. The second ecosystem then asks for [your setup code](#your-setup-code), not a temporary one.

---

## Opener variant

The Opener variant commissions exactly as above. It behaves differently from the Light variant once paired.

- It presents as a switch or outlet for the relay trigger, plus a separate contact sensor for door state. It is not a light.
- The trigger is momentary. When you turn it on from a controller, the relay energizes for about 500ms and the firmware returns it to off. The control tile may briefly show on and then revert to off. This is the controller reconciling a momentary action that Matter models as On/Off, and it is normal. Triggering from the onboard button updates almost instantly.

### Apple Home

The contact sensor can be recategorized to Garage Door in its accessory settings, which gives a native garage door status display. Door, Window, and Blinds are also available. This changes only how Apple Home labels and displays the sensor. It does not change the Matter device type, which is fixed at flash time. The relay tile acts as the momentary trigger.

### Home Assistant

Home Assistant exposes the Opener as a switch for the trigger and a binary sensor for door state. To get a single garage tile, combine them into a Cover template with `device_class: garage`, using the switch to trigger and the binary sensor to report open or closed.

---

## Troubleshooting

### Commissioning hangs in Home Assistant

If Home Assistant's Matter integration hangs during commissioning or shows "setting up" indefinitely, restart the Matter Server add-on.

1. Go to Settings, then Apps, then Matter Server, then Restart.
2. Wait 60 seconds for the full restart.
3. [Factory reset the Shelly](#factory-reset).
4. Retry commissioning.

---

## Factory reset

Hold the onboard button for 10 seconds. After 5 seconds the LED blinks every ½ second, and after 10 seconds every ¼ second. Let go while it blinks every ¼ second. (Keep holding past 20 seconds to cancel.) On this firmware that clears the device's Matter commissioning and returns it to BLE commissioning mode. It does not restore the original Shelly firmware or factory state; the Automatous firmware stays installed, and going back to stock requires the UART backup (see [Reversibility](REVERSIBILITY.md)). The LED gives one long flash, the device restarts, and the LED returns to the rapid pairing blink from the [status LED reference](#status-led-reference), which confirms the reset worked. Within 60 seconds of a firmware update the reset is refused (three quick flashes); wait a minute and try again. See [Button, Switch and LED](BUTTON-AND-LED.md) for every case. Remove the device from any ecosystems you added it to before re-commissioning.

---

## Related documentation

- [README](../README.md) — project overview and quick start
- [Button, Switch and LED](BUTTON-AND-LED.md) — every gesture, every state, every LED pattern
- [Why Matter over Thread](WHY.md) — the rationale for Matter over Thread
- [Flashing Guide](FLASHING.md) — wiring, backing up stock firmware, and flashing
- [Reversibility](REVERSIBILITY.md) — warranty, factory keys, and how reversible flashing is
- [Updating](UPDATING.md) — keeping a device current after flashing
- [Power Consumption](POWER.md) — measured draw and the Thread Router design choice
- [Protection and Metering](PROTECTION.md) — over-temperature, overcurrent and overvoltage protection, power metering, and hardware faults
- [Building from Source](BUILDING.md) — compiling the firmware yourself
- [GPIO Map](GPIO.md) — ESP-Shelly-C68F pin assignments for Gen4 devices
- [Certification](CERTIFICATION.md) — uncertified status and test credentials
- [Roadmap](ROADMAP.md) — current known limitations and planned work
- [Contributing](CONTRIBUTING.md) — reporting bugs and the firmware filename convention
- [Contributors](CONTRIBUTORS.md) — people who have helped move the project forward
