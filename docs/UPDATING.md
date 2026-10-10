# Updating the firmware

**[README](../README.md)** > **Updating** · [Report an issue](../../../issues/new)

This guide covers updating a supported Shelly Gen4 device that is already running the Automatous firmware with Matter OTA support. There are four paths, and the right one depends on whether you want to keep the device's Matter commissioning or not. If you have not flashed the firmware yet, start with the [Flashing Guide](FLASHING.md).

If your Shelly 1 Gen4 is running an Automatous build earlier than v2.0.0, update it with a one-time full reflash over [UART](FLASHING.md#flash-with-usb-uart). The partition layout changed in v2.0.0; this one step erases commissioning and needs to be re-commissioned afterward. From v2.0.0 onward this requirement no longer applies, and further updates are delivered via OTA. The Shelly 1 Mini Gen4 has had the OTA-capable layout since its first release.

---

## Contents

- [Choosing an update path](#choosing-an-update-path)
- [Matter OTA through Home Assistant](#matter-ota-through-home-assistant)
- [Wi-Fi maintenance mode](#wi-fi-maintenance-mode)
- [ESP-IDF CLI](#esp-idf-cli)
- [Building the OTA image](#building-the-ota-image)

---

## Choosing an update path

| Path | Preserves commissioning | Hardware | Method |
|---|---|---|---|
| Matter OTA through Home Assistant | Yes | None | OTA |
| Wi-Fi maintenance mode | Yes | A phone or laptop | Upload over the device's own Wi-Fi |
| ESP-IDF CLI | Yes | USB-UART adapter | UART flash |
| ESPConnect | No | USB-UART adapter | UART flash |

The easiest path is Matter OTA. The new firmware is sent to the device over Thread, the device writes it to its inactive slot and reboots, and its Matter commissioning, Thread credentials, and ecosystem pairings all survive. No wires, no re-pairing.

Wi-Fi maintenance mode needs no Home Assistant and no wires, only a phone or laptop next to the device. Unlike Matter OTA, it can install an older version, and it still works when Thread or Matter is not.

ESPConnect is the full reinstall covered in the [Flashing Guide](FLASHING.md#flash-with-espconnect). It is listed here for comparison only. It erases flash, and therefore commissioning, so it is for a first install or switching variants, and not for an in-place update.

---

## Matter OTA through Home Assistant

Because the firmware uses Matter test credentials, only the Home Assistant ecosystem is supported and documented in this repo to serve OTA updates. Home Assistant's Matter Server acts as the OTA provider; you place an `.ota` image where it can read it, and it offers that image to any eligible commissioned device whose vendor and product ID match.

Verified on **Matter Server add-on, version 9.0.2**.

### 1. Enable Test Net DCL

The Matter Server only serves uncertified OTA images when Test Net DCL is enabled. This is the same setting that lets the test credentialed device commission in the first place.

1. Go to **Settings → Apps → Matter Server → Configuration**.
2. Turn on **Enable test-net DCL usage**.
3. **Restart** the add-on.

Enabling it also points the server at its OTA folder, `/addon_configs/core_matter_server/updates/`. Without Test Net DCL, custom `.ota` files are ignored.

### 2. Get the OTA image

Download the `.ota` for your device and variant from the [latest release](../../../releases/latest), or [build it yourself](#building-the-ota-image). The image is named `automatous-io-{hardware}-{variant}-vX.Y.Z.ota`. Its version must be higher than what the device is running, or the server has nothing newer to offer.

### 3. Place the image

The OTA folder lives inside the add-on's config directory. The simplest way to reach it is the Samba Share add-on.

1. Install and start the **Samba Share** add-on if you have not already.
2. From your computer, open the `addon_configs` share, then `core_matter_server`, then `updates` (full path `/addon_configs/core_matter_server/updates/`). Create the `updates` folder if it does not exist.
3. Copy the `.ota` file into `updates`.

### 4. Restart the add-on to import

The server scans the `updates` folder **at startup**. Restart the Matter Server add-on to import the image right away rather than waiting for the next restart or OTA update availability interval.

1. **Settings → Apps → Matter Server → Restart.**
2. Watch the add-on log. On a successful import it logs `Imported OTA file: …`, and the `.ota` **disappears** from the `updates` folder. That is expected: the server copies the image into its own storage location and deletes the source. The image is now loaded.

### 5. Install the update

Once the image is loaded, the update is available in two places. Use whichever you prefer:

- On the device page in Home Assistant, where its firmware update appears.
- In the Matter Server add-on, by selecting the node and choosing **Update**.

1. Trigger the update from either location above.
2. The device downloads over Thread, writes its inactive slot, verifies, and reboots into the new firmware. If the update does not appear right away, the device queries on its own timer; power-cycling it prompts an immediate check.
3. The device comes back reporting the new version, and the server then offers nothing further. A download that fails verification is never booted, and the device keeps running its current firmware.

> **Automatic rollback.** From the releases that include it (Shelly 1 Gen4 Light, Outlet and Opener 2.2.0, Light Switch 2.3.0, Shelly 1 Mini Gen4 Outlet 1.1.0, Shelly 1PM Gen4 Outlet 1.0.0, Shelly 2PM Gen4 Outlet and Light 1.0.0), a newly installed image has to prove itself: it confirms itself after running for 60 seconds with the Matter stack responsive. If it crashes, hangs or restarts before then, the device boots the previous firmware again. Two conditions apply:
>
> - The device's **bootloader** must support rollback. Only a web UI or UART install writes the bootloader; a Matter OTA never replaces it. A device that has only ever been updated over Matter OTA from an older release keeps its old bootloader, and gets rollback the next time it is installed over the web UI or UART.
> - The update **from** an older release to one of these is not protected: the older firmware does not mark the new image as on trial.
>
> Any restart in the first 60 seconds after an update, including a power cut, counts as a failure and rolls back. Leave the device powered for a minute after an update.

While a Matter update downloads and installs, the status LED blinks slowly and evenly, 1 s on and 1 s off (LED-16 in [Button, Switch and LED](BUTTON-AND-LED.md)). Don't unplug the device while it does. It then restarts into the new firmware.

> 💡 **One image updates every matching device.** The provider offers the stored image by vendor and product ID; a single `.ota` updates every device of that variant on your Thread network. Publish once, and each matching device picks it up the next time it checks in.

> **Updating to a release with per-device setup codes.** Your pairings are kept. After the update the device uses its own [setup code](COMMISSIONING.md#your-setup-code), normally the one on its Shelly sticker, so the shared test code `3497-011-2332` stops working for new pairing windows (3 presses of the button, or adding it to another ecosystem). Read the new code from the Wi-Fi mode page before you need it. If the update rolls back, the older firmware uses the test code again.

---

## Wi-Fi maintenance mode

> **Experimental.** Built and unit-tested, not yet verified on hardware.

Hold the device's button for 5 seconds and the device restarts into Wi-Fi maintenance mode. For as long as the mode lasts, Matter and Thread are off and the device runs its own Wi-Fi network with an update page, protected by a [password](#wi-fi-mode-password). It is the same idea as stock Shelly firmware's 5-second hold, and it uses the same address, `192.168.33.1`. Every gesture and LED pattern involved is in [Button, Switch and LED](BUTTON-AND-LED.md).

The mode is a restart rather than a switch because Espressif does not support a Wi-Fi access point alongside a Thread router on the ESP32-C6. A boot of its own also leaves all of the device's memory to Wi-Fi and the upload.

### 1. Get the OTA image

Use the same `.ota` file as for [Matter OTA](#2-get-the-ota-image): `automatous-io-{hardware}-{variant}-vX.Y.Z.ota`. The page refuses:
- a `.bin`: use the `.ota`;
- firmware for a different Shelly model: the image carries its hardware code (`S1G4`, `Mini1G4`, `S1PMG4` or `S2PMG4`), and it must match;
- firmware built before this feature: it has no hardware code, so install it over UART. Matter OTA only offers a version higher than the one running, and every build without a hardware code is older than one with it.

### 2. Enter the mode

1. Hold the button. After 5 seconds the LED blinks every ½ second: let go.
2. The LED gives one long flash and the device restarts. The relay drops briefly while it restarts, then returns to its previous state, unless over-temperature or a protection trip refuses it. The Opener's trigger always stays off.
3. The LED shows a double blink: the mode is running.

The hold is refused (three quick flashes) within 60 seconds of an update, because a restart before the update confirms itself would roll it back, and while a Matter OTA download is in progress.

### 3. Upload

1. On a phone or laptop, join the Wi-Fi network named like stock Shelly's, `Shelly<model>-<the 12-digit MAC on the sticker>`, for example `Shelly1PMG4-A1B2C3D4E5F6`. Its password is the [Matter setup code on the Shelly sticker](#wi-fi-mode-password), as 11 digits without the dashes, unless you set your own. A device without a sticker code runs an open network, like stock Shelly's setup network. Anyone who joins can use the page, switch the relay and install firmware built for this model, so enter the mode only when you are about to use it, and press **Exit** when you are done.
2. The update page usually opens on its own. If not, browse to `http://192.168.33.1`. Your phone may warn that the network has no internet; that is expected. The window that opens on its own is the phone's sign-in window, which may not let you choose a file; if choosing the `.ota` does nothing, open `http://192.168.33.1` in your browser.
3. The page has tabs: **Home** (the relays and the device's model, variant and firmware), **Pairing** (the [Matter setup code](#matter-setup-code)), **Switch**, **Meter**, **Update** and **Diagnostics**. Tabs a model doesn't have are hidden. On **Update**, choose the `.ota` file (on a computer you can also drag it onto the page) and press **Install**. The LED stays solid while the file uploads and installs.
4. The device checks the file before erasing anything, installs it into its inactive slot, verifies it and restarts into it.

If the page doesn't work in your browser, `http://192.168.33.1/recovery` is a simple install page that does only step 3, with no code shared with the main page.

To install an older version of the same variant, tick **Allow installing an older version** first. An image for a different variant of the same hardware (for example Outlet onto a device running Light) installs whatever its version, since each variant has its own version numbers, but it is a different Matter device type: remove the device from your controller and pair it again afterward.

### 4. Back to normal

The device returns to normal operation, with Matter and Thread back on, in any of these cases:
- after an upload;
- when you press **Exit** on the page (tap it twice: the second tap confirms);
- when you hold the button 5 seconds again;
- after 10 minutes with no page open (the main page and `/recovery` both keep it running while open).

A 5 s hold during an upload abandons it: nothing is installed. The relay's state in the mode becomes its previous state, and it then follows its power-on setting from there: with the default Off it turns off, with Previous it keeps that state (see [Button, Switch and LED](BUTTON-AND-LED.md), P-03). [Automatic rollback](#5-install-the-update) applies as for a Matter OTA: leave the device powered for a minute after an update.

While the mode lasts:
- a tap and the page's Toggle still switch the relay, and so does the wall switch on the Light, Outlet, Mini, 1PM and 2PM (each relay on the 2PM, with 2 taps for relay 2), as its [switch mode](BUTTON-AND-LED.md#wall-switch-modes) says (the Opener's contact and the Light Switch's bindings need Matter and are ignored), but controllers only see the relay's state once the device is back in normal mode;
- Home Assistant shows the device as unavailable;
- every protection still trips.

### Matter setup code

The page's **Pairing** tab shows the device's [Matter setup code](COMMISSIONING.md#your-setup-code), the code you pair it with. It shows its QR too, and the sticker's code when another is in use. To change it, enter another 11-digit code (the sticker's own goes back to it) or press **Make a new random code**, then press the device's button 3 times within 2 minutes. The LED gives one long flash and the page confirms. Until you press, nothing changes; after 2 minutes, on Cancel or when you leave the mode, the change is dropped. The new code applies once the device is back in normal operation. Your existing pairings keep working either way.

Anyone who joins the network can read the code while the mode is on. Changing it needs the button presses, which need the device in hand.

### Wi-Fi mode password

The network uses WPA2 and WPA3 together: phones and laptops that have WPA3 use it, older ones WPA2. Its password, first match wins:
1. **Your own**, set on the page's **Home** tab: 8 to 63 characters (letters, digits, spaces and symbols, no accents or emoji), saved with 3 presses of the device's button within 2 minutes, as for the setup code.
2. **The setup code on the Shelly sticker**, as its 11 digits without the dashes: `2058-491-0137` becomes `20584910137`. It stays the sticker's code even if you pair with another setup code.
3. **None** on a device whose sticker code can't be read (it then uses a code it made itself, which isn't printed anywhere): the network is open until you set your own.

A new password applies the next time the mode starts, so the connection you are using stays up. The Home tab says what the next one will be; note it before you exit. A factory reset (hold 10-20 s) forgets your own password, so the device always comes back to a password printed on it, or to none: Wi-Fi mode is how you install firmware, and a forgotten password must never lock it. If the device can't start the network with its password, it starts it open rather than not at all, and the page says so.

### Wall switch mode

On the Light, Outlet, Mini, 1PM and 2PM, the page's **Switch** tab sets what the wall switch wired to SW does (on the 2PM, SW1 and SW2 each have their own setting, for relay 1 and relay 2): **Toggle** (the default, for an ordinary on/off switch), **Push button** (for a button that springs back), **Follow** (the relay matches the switch position) or **Disabled**. Choose one, press **Save**, then press the device's button 3 times within 2 minutes, as for the setup code. The new mode applies at once, so you can try it before you exit. See [Wall switch modes](BUTTON-AND-LED.md#wall-switch-modes).

On the 1PM and 2PM, the **Meter** tab shows the meter's live readings (each relay's on the 2PM) and can calibrate the meter against your own plug-in meter, saved the same way with 3 presses. See [Calibrating the meter](PROTECTION.md#calibrating-the-meter).

---

## ESP-IDF CLI

If you build from source and have a UART adapter on the device, `idf.py flash` updates the bootloader, partition table, and application while leaving NVS and commissioned state intact. See [Flash your build](BUILDING.md#flash-your-build).

---

## Building the OTA image

`scripts/make-matter-ota.py` wraps a variant's built application as a Matter `.ota`, reading the vendor and product ID from the build's `sdkconfig` and the software version from `CHIPProjectConfig.h`; the image always matches the firmware it came from and can only target the device and variant it was built for. See [Building from Source](BUILDING.md#build-the-release-artifacts).

---

## Related documentation

- [README](../README.md) — project overview and quick start
- [Why Matter over Thread](WHY.md) — the rationale for Matter over Thread
- [Flashing Guide](FLASHING.md) — wiring, backing up stock firmware, and flashing
- [Reversibility](REVERSIBILITY.md) — warranty, factory keys, and how reversible flashing is
- [Commissioning](COMMISSIONING.md) — pairing the device and reading the status LED
- [Power Consumption](POWER.md) — measured draw and the Thread Router design choice
- [Protection and Metering](PROTECTION.md) — over-temperature, overcurrent and overvoltage protection, power metering, and hardware faults
- [Building from Source](BUILDING.md) — compiling the firmware yourself
- [GPIO Map](GPIO.md) — ESP-Shelly-C68F pin assignments for Gen4 devices
- [Certification](CERTIFICATION.md) — uncertified status and test credentials
- [Roadmap](ROADMAP.md) — current known limitations and planned work
- [Contributing](CONTRIBUTING.md) — reporting bugs and the firmware filename convention
- [Contributors](CONTRIBUTORS.md) — people who have helped move the project forward
