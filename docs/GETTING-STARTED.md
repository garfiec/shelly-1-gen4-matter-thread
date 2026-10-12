# Getting Started

**[README](../README.md)** > **Getting Started** · [Report an issue](../../../issues/new)

This guide takes a Shelly Gen4 from stock firmware to working in your smart home app, using only the Shelly's own web page: no wires, no soldering. It takes about 15 minutes. If something doesn't go as described, check the [FAQ](#faq-and-gotchas) at the end.

> ⚠️ Installing from the web page is one-way: you can't go back to stock firmware afterwards unless you first made a full backup over a USB-UART adapter (see the [Flashing Guide](FLASHING.md#flash-with-usb-uart)). It also voids your warranty. See [Reversibility](REVERSIBILITY.md).

## Contents

1. [What you need](#what-you-need)
2. [Choose your package](#choose-your-package)
3. [Power up the Shelly and join its network](#power-up-the-shelly-and-join-its-network)
4. [Check the firmware slot](#check-the-firmware-slot)
5. [Install the firmware](#install-the-firmware)
6. [Factory reset once](#factory-reset-once)
7. [Pair with your smart home](#pair-with-your-smart-home)
8. [Keeping it updated](#keeping-it-updated)
9. [Changing the device type](#changing-the-device-type)
10. [FAQ and gotchas](#faq-and-gotchas)

---

## What you need

- **A supported Shelly**, still on stock firmware. See [Supported devices](../README.md#supported-devices).
- **A Thread border router.** Matter over Thread devices reach your network through one. Any of these work ("Tested" means tested by the original project or this one):

  | Border router | Tested |
  |---|---|
  | Apple HomePod mini, HomePod (2nd gen) | Yes |
  | Apple TV 4K (2nd gen A2169) | Yes |
  | Apple TV 4K (3rd gen, Wi-Fi + Ethernet model; the Wi-Fi-only model has no Thread) | Not yet |
  | Home Assistant with Connect ZBT-1 (formerly SkyConnect) or Yellow | Yes |
  | Home Assistant with Connect ZBT-2 | Not yet |
  | Google Nest Hub (2nd gen), Nest Hub Max, Nest Wifi Pro, Google TV Streamer | Not yet |
  | Amazon Echo (4th gen), or a newer Echo with Thread (e.g. Echo Hub, Echo Studio) | Not yet |

  An iPhone 15 Pro, or an iPhone 16 or newer (except the 16e), has its own Thread radio and can help with pairing, but it isn't a border router: the device still needs one of these at home to stay reachable.

- **A phone** with your smart home app (Apple Home, Google Home, Alexa or Home Assistant).
- **A computer or phone with Wi-Fi and a browser**, for the install.

## Choose your package

Open the [latest release](../../../releases/latest) and find your device. Each device has one firmware, packaged once per product line (Light, Outlet and so on). The packages differ only in which device type the device starts as; you can change the type afterwards on the maintenance page. Each has two files:

- `…-ota.zip`: **the one you want now.** It installs from the stock Shelly web page.
- `….ota`: for later updates, through Home Assistant or the device's maintenance page.

Pick the package for the type you want to start with:

| Type | Use it for |
|---|---|
| Light | Lights. Your app shows a light. |
| Outlet | Anything else you switch on and off: sockets, fans, heaters, pumps. Your app shows an outlet. |
| Garage door opener | Garage doors and gates: a half-second pulse per command. On the Shelly 1 Gen4, SW reads a door sensor; the Shelly 1 Mini Gen4 has no door sensor, since its SW carries mains voltage. Not on the 1PM or 2PM. |
| Light Switch | Using the wall switch to control other Matter devices through a binding (Shelly 1 Gen4 and Shelly 1 Mini Gen4). |

Changing to a type with other Matter endpoints later means pairing the device again: see [Changing the device type](#changing-the-device-type).

## Power up the Shelly and join its network

1. **Wire the Shelly to power.** If you haven't wired one before, follow Shelly's own wiring diagrams for your model: [Shelly 1 Gen4](https://kb.shelly.cloud/knowledge-base/shelly-1-gen4), [Shelly 1 Mini Gen4](https://kb.shelly.cloud/knowledge-base/shelly-1-mini-gen4), [Shelly 1PM Gen4](https://kb.shelly.cloud/knowledge-base/shelly-1pm-gen4), [Shelly 2PM Gen4](https://kb.shelly.cloud/knowledge-base/shelly-2pm-gen4). Mains wiring is dangerous: if you're not sure, use an electrician.
2. **Join its Wi-Fi network.** A new Shelly starts in standalone mode and runs its own Wi-Fi network, named like `Shelly1G4-…` (the model, then the MAC address). Join it from your phone or computer. If you don't see it, hold the Shelly's button for 5 seconds to turn its network back on.
3. **Open its web page** at `http://192.168.33.1`. This is where you check the slot and upload the firmware.

## Check the firmware slot

> [!IMPORTANT]
> **Don't skip this step.** This firmware only installs when the stock firmware is running from **slot 1**. New Shelly devices usually run from slot 0, so most will need a stock update first.

The Shelly keeps two copies of its firmware, in slot 0 and slot 1. Shelly's updater always writes new firmware into the slot it isn't running from, and this firmware's bootloader starts slot 0. So the install only works when stock is running from slot 1, and every stock update switches slots.

1. Open `http://192.168.33.1/rpc/Shelly.GetDeviceInfo`.
2. Find `"slot"`. If it says `1`, carry on to the install.
3. If it says `0`, install a stock firmware update first. The Shelly downloads stock updates from the internet, so connect it to your home Wi-Fi in its web page's Wi-Fi settings, then install the update from the firmware settings. If no update is offered, try Shelly's beta channel.
4. **Check `slot` again right before you upload**, and install this firmware straight after. The page is now also at the Shelly's address on your home network, which your router's device list shows. A second stock update would switch it back to slot 0.

If you upload from slot 0, stock firmware stays in control: the upload stalls, or it reports success and the device restarts back into stock. Install a stock update and try again. It isn't known whether such an attempt writes Shelly's permanent eFuse marker (see [Reversibility](REVERSIBILITY.md)), so it's best to check first.

## Install the firmware

1. In the Shelly's web page (`http://192.168.33.1` on its own network, or its address on your home network), go to the firmware settings and choose to install from a file.
2. Select the `…-ota.zip` you downloaded and confirm.
3. Wait for the device to restart. The LED blinks quickly when the new firmware is running and ready to pair.
4. **Unplug the power for a few seconds and plug it back in.** This firmware releases the relay and LED pins that stock can leave latched, but a power cycle makes sure of it.

## Factory reset once

**Don't skip this.** Stock firmware leaves data behind that makes Matter updates fail silently later.

1. Hold the device's button for **10 seconds**. After 5 s the LED blinks every ½ second, and after 10 s every ¼ second.
2. Let go while it blinks every ¼ second. The LED gives one long flash and the device restarts.
3. The LED blinks quickly again: it's ready to pair.

See the [Button guide](BUTTON-GUIDE.md) for what each hold does.

## Pair with your smart home

1. Find the setup code. It's normally the **QR code and 11-digit code on the Shelly sticker**. If your Shelly has no sticker code, or you want to check which code it uses, see [Where is my setup code?](#where-is-my-setup-code)
2. In your app, add a new Matter device and scan the QR code or type the code.
3. The firmware uses Matter test credentials, so apps treat it as uncertified (see [Certification](CERTIFICATION.md)):
   - **Apple Home** warns "Uncertified Accessory": tap **Add Anyway**.
   - **Google Home** warns that the device hasn't been certified: choose **Add anyway**.
   - **Home Assistant** doesn't ask, but it won't pair until **Enable test-net DCL usage** is on in the Matter Server app's configuration (restart the app after turning it on). See [Commissioning](COMMISSIONING.md#google-home-alexa-home-assistant).
4. When pairing finishes, the LED stays on.

Pairing stays open for 15 minutes after the device starts. If it has closed (a short flash every 2 s), **press the button 3 times** to open it again. To add the device to a second app later, use the first app's "share" or "pair another app" option, or press the button 3 times. Step-by-step instructions for each app are in [Commissioning](COMMISSIONING.md).

## Keeping it updated

You have two ways to update:

- **Through Home Assistant** (Matter update). This keeps all your pairings. It needs a few one-time settings in the Matter Server app; see [Updating](UPDATING.md#matter-ota-through-home-assistant).
- **From the device's maintenance page.** Hold the button for **5 seconds** and let go while the LED blinks every ½ second. The device starts its own Wi-Fi network: join it from your phone (the password is the sticker's 11 digits, without dashes) and open `http://192.168.33.1`. Drop the `….ota` file onto the **Update** tab. This works without Home Assistant, and it can install an older version too. See [Updating](UPDATING.md#wi-fi-maintenance-mode).

If an update crashes or hangs within its first minute, the device goes back to the previous version by itself. This needs this firmware's bootloader, which the web install puts in place; a device moved from the original project's firmware by a Matter update doesn't have it.

## Changing the device type

Open the maintenance page (hold the button 5 seconds), go to the **Switch** tab, and choose a **Device type**. It applies when you exit Wi-Fi mode.

- **On the Shelly 1 Mini Gen4, Outlet and Garage door opener** have the same Matter endpoints, so switching between them keeps the pairing.
- **Every other change** gives the device other Matter endpoints, which your apps' pairing doesn't fit. The page warns you and asks you to confirm. When you exit Wi-Fi mode, the device clears its pairings and restarts ready to pair. Remove it from your apps first, then pair it again with the code on the Pairing tab. Your setup code, Wi-Fi mode password, wall switch modes and meter calibration are kept.

See [Device type](BUTTON-AND-LED.md#device-type) for the details.

---

## FAQ and gotchas

### The install stalls, or the device restarts back into stock firmware

Stock firmware was running from slot 0. Install a stock update, check that `slot` reads 1, then install this firmware straight away. See [Check the firmware slot](#check-the-firmware-slot).

### Where is my setup code?

- **Usually:** on the Shelly sticker, as a QR code and an 11-digit code. The firmware reads that code from the device itself, so the sticker is the right one to use.
- **No sticker code:** the device makes its own random code the first time it starts and keeps it. To see it, open the maintenance page (hold the button 5 s) and go to the **Pairing** tab.
- **Your own code:** you can set any valid code, or make a new random one, on the same tab.

See [Your setup code](COMMISSIONING.md#your-setup-code).

### My app can't find the device while the maintenance page is open

Thread and Bluetooth are off while the device is in Wi-Fi mode, so Matter apps can't see it. Tap **Exit** on the page (or hold the button 5 s again) and pair after it restarts. If the LED doesn't show the quick pairing blink, press the button 3 times.

### What's the maintenance network's password?

By default it's the setup code from the Shelly sticker: the 11 digits without dashes. You can set your own on the page's **Home** tab. Note it down: you'd need a factory reset to get back to the sticker code if you forget it. A Shelly without a sticker code has an open network instead.

### The page asks me to press the button 3 times

The page saves changes as soon as you press Save. Two things wait for 3 presses of the device's button within 2 minutes instead:

- **A new Wi-Fi mode password**, because a typo would lock you out of the page.
- **Any change on an open network** (a device without a sticker code), so someone passing by can't change your settings.

### Home Assistant doesn't offer my update

Matter only offers a version **higher** than the one installed. To reinstall the same version, or go back to an older one, use the maintenance page's **Update** tab. Also check that **Enable test-net DCL usage** is on in the Matter Server app. See [Updating](UPDATING.md).

### My app says the device is uncertified

Expected. The firmware uses Matter's test credentials, because only certified products can have real ones. Choose to add it anyway. See [Certification](CERTIFICATION.md).

### Android pairing gets stuck at "Checking connectivity to Thread network"

Older builds didn't answer pings on their Thread address, which Android's pairing checks for. This firmware does; if you're on an older build, update it.

### My garage door opener shows as a switch in Home Assistant

Matter has no "momentary button" device type, so the opener shows as a switch that turns itself off after the pulse. To get a button in Home Assistant, create a template button that turns the switch on.

### The maintenance page closed by itself

Wi-Fi mode ends after 10 minutes with no page open, after an update, or when you press Exit. The device then restarts normally. Hold the button 5 s to start it again.

### Can I go back to stock firmware?

Only if you made a full backup over a USB-UART adapter **before** installing. A factory reset keeps this firmware installed: it clears the pairings and the Thread network, not the firmware. See [Reversibility](REVERSIBILITY.md).

### I pressed the button and nothing happened

The LED tells you why:
- **Three quick flashes:** the device refused. Usually it's still starting up, a protection is active (for example it's too hot), or you held for Wi-Fi mode while a Matter update was downloading or in the first minute after one.
- **No reaction at all to a hold:** a hold of 1–5 seconds intentionally does nothing.

See the [Button guide](BUTTON-GUIDE.md).

---

## Related

- [Button guide](BUTTON-GUIDE.md): every press, every hold, every LED pattern
- [Commissioning](COMMISSIONING.md): pairing with each ecosystem in detail
- [Updating](UPDATING.md): Matter updates and the maintenance page
- [Flashing Guide](FLASHING.md): the install in more depth, and USB-UART flashing with a backup
