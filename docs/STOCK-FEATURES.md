# Stock Firmware Features

**[README](../README.md)** > **Stock Firmware Features** · [Report an issue](../../../issues/new)

This page catalogs what the stock Shelly firmware offers on the Shelly 1PM Gen4, how this firmware covers each feature, and which ones are candidates to add. The planned ones are in the [Roadmap](ROADMAP.md).

**Source:**
- a read-only capture of a Shelly 1PM Gen4 running stock 2.0.1, taken on 2026-09-23 with `scripts/capture-shelly-webui.py`;
- the device reported 170 RPC methods, its full configuration and status, and its web UI.

The Shelly 1 Gen4 and 1 Mini Gen4 share almost all of this, without the power meter.

---

## Contents

- [Output and wall switch](#output-and-wall-switch)
- [Protection](#protection)
- [Metering](#metering)
- [Device and system](#device-and-system)
- [Not applicable to this firmware](#not-applicable-to-this-firmware)
- [Candidates, by priority](#candidates-by-priority)

---

## Output and wall switch

| Stock feature | Stock setting | This firmware |
|---|---|---|
| Input mode: **Follow** (stock default) | `in_mode: follow`: the output follows the switch position | **Follow** [switch mode](BUTTON-AND-LED.md#wall-switch-modes) |
| Input mode: **Edge** | `flip`: every change toggles | **Toggle** switch mode (our default) |
| Input mode: **Momentary** | `momentary`: every push toggles | **Push button** switch mode |
| Input mode: **Detached** | `detached`: the input doesn't drive the relay, but its state is still reported | **Disabled** switch mode, which doesn't report the input |
| Input mode: **Activation** | `activate`: for a motion sensor, any input turns on and restarts the auto-off timer | Not available |
| Power-on state | `initial_state`: on, off, restore last, or **match the switch** (stock's value on this unit) | Matter StartUpOnOff: on, off, previous or toggle. **No "match the switch"** |
| Auto off / Auto on | `auto_off`, `auto_on`, each with a delay of up to 7 days | Matter's On-with-timed-off command only. **No device setting** |
| Input type | `switch` or `button` | The switch mode covers it |
| Invert input | `invert` | Build-time only (`CONFIG_SHELLY_SWITCH_ACTIVE_HIGH`) |
| Enable or disable input | `enable` | Disabled switch mode |
| Lock inputs | `in_locked`: the switch and the button don't toggle | Disabled switch mode covers the wall switch. **The button can't be locked** |
| **Factory reset from the wall switch** | `factory_reset: true` (on by default): toggling SW several times shortly after power-on resets the device | **Not available: reset and Wi-Fi mode need the device's own button** |

## Protection

| Stock feature | Stock setting (this unit) | This firmware |
|---|---|---|
| Overcurrent | `current_limit`: 16 A, user-set | A heat model starting at 16.5 A (15 A rating ×110%), latched. Build-time setting |
| Overvoltage | `voltage_limit`: 280 V, user-set | 150 V on 120 V mains, 264 V on 230/240 V (mains learned). Build-time setting |
| **Undervoltage** | `undervoltage_limit`, user-set | **Not available** |
| Overpower | `power_limit`: 4480 W, user-set | Covered by overcurrent at a fixed voltage. **No watt limit** |
| Voltage auto-recovery | `autorecover_voltage_errors`: **off** on this unit | Always on in normal operation: the relay returns to its previous state 30 s after the voltage is back in range. Not in Wi-Fi mode ([Resetting a trip](PROTECTION.md#resetting-a-trip)) |
| Overheating | Fixed | Board NTC above 90 °C for 10 s |

## Metering

| Stock feature | This firmware |
|---|---|
| Voltage, current, power, frequency, power factor | Voltage, current, power, energy and frequency through Matter's Electrical Power Measurement and Electrical Energy Measurement clusters. Power factor is not reported |
| Energy total | Yes, kept across restarts and updates |
| Returned energy (`ret_aenergy`), reverse power measurement | Not available |
| Counters: switch-on count, on-time, time above a power threshold | Not available (Matter has no standard attribute for them) |
| "Consumption type" label for third-party systems | Not needed: the Matter device type (the variant) says what the load is |

## Device and system

| Stock feature | This firmware |
|---|---|
| Device name | Set in your Matter controller |
| Time zone, location, SNTP | Not needed: controllers do the scheduling |
| Eco mode (lower power) | Not available (the radio is Thread, not Wi-Fi) |
| Debug logs over UDP or WebSocket | Serial log. A [diagnostics build](BUILDING.md#diagnostics-build) (off by default) also keeps the last 6 KB of the log and shows it on the Wi-Fi mode page |
| Firmware update from a file or URL | Wi-Fi mode page, from a file. Matter OTA |
| Switch between Matter and Zigbee firmware | Not applicable |
| Setup network (AP) with a password option; stock 2.x turns an open AP off after a countdown | Wi-Fi mode: WPA2/WPA3, with a [password](UPDATING.md#wi-fi-mode-password) from the sticker or your own (open on a device without a sticker code, and as a last resort if the secured network can't start); ends after 10 minutes with no page open |
| Identify | Matter Identify |

## Not applicable to this firmware

This firmware is Matter over Thread with no Wi-Fi in normal operation, and its RAM is tight. These stock features depend on Wi-Fi or the cloud, or duplicate what Matter controllers do:
- Wi-Fi client, roaming and range extender;
- Shelly Cloud, MQTT, outbound WebSocket, Modbus TCP and KNX;
- scripts, schedules, webhooks and actions;
- virtual components (boolean, number, text, enum, group) and the key-value store;
- BLE gateway, BTHome devices and sensors, and BLE RPC;
- HTTPS certificates and TLS settings.

---

## Candidates, by priority

1. **Factory reset and Wi-Fi mode from the wall switch.**
   - **Why:** installed in a wall box, the device's button is out of reach. Today a factory reset, the Wi-Fi mode page (setup code, updates, switch mode) and 3-press pairing all need that button. Stock turns reset-from-switch on by default for this reason.
   - **The idea:** a sequence of wall-switch flips shortly after power-on.
   - **Cost:** a new gesture ladder for SW, new rows in [Button, Switch and LED](BUTTON-AND-LED.md), and care that normal switching can never trigger it.
2. **Power-on "match the switch"** for Follow mode, which is stock's default combination. In Follow mode the relay would take the switch's position at power-on, instead of StartUpOnOff.
3. **Invert input as a page setting.** Follow's polarity is unverified. A page setting would fix a backwards Follow without a rebuild.
4. **Protection limits on the Wi-Fi mode page**, within the hardware's rating:
   - current and voltage limits;
   - a new **undervoltage** trip;
   - an optional watt limit;
   - the voltage auto-recovery on/off, since stock ships with it off.
5. **Auto off / auto on timers and the Activation mode** for motion sensors, as a page setting.
6. **Lock the button**, as stock's "Lock inputs" does for both inputs.
7. **Power factor and returned energy**, if the Matter clusters and the BL0942 support them.
8. **Counters** (switch-on count, on-time): low value without a standard Matter attribute.
