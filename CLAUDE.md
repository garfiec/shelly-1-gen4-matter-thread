# CLAUDE.md

Guidance for Claude (and anyone else) changing this repository.

## The device UI is a specification: keep it in sync

The button, the wall switch (SW) and the status LED are the device's only local interface. [docs/BUTTON-AND-LED.md](docs/BUTTON-AND-LED.md) specifies all of it: every gesture, what it does in every state, every LED pattern and every timing.

**Any change to how an input or the LED behaves must, in the same commit:**
1. Update `docs/BUTTON-AND-LED.md`.
2. Update the matching rows in `source/components/test_host/test_ui.c`: same ID, same words.
3. Update `docs/BUTTON-GUIDE.md`, the plain-language guide (tables and Mermaid diagrams), wherever the change shows. No script checks it, so read it through.
4. Pass `source/components/test_host/run.sh`. It runs the UI tests and `scripts/check-ui-spec.py`, which fails if the doc and the code disagree in any row.

**This applies to changes in:**
- `source/components/shelly_button/` (`button_logic.h` holds the gesture timings, T-xx)
- `source/components/shelly_status_led/` (`led_patterns.h` holds every pattern, LED-xx)
- `source/components/shelly_app_common/ui_policy.c` (what each input does in each state and switch mode, B-xx, SW-xx and SM-xx) and `shelly_ui.cpp` (carrying it out)
- The LED and commissioning-window handling in `source/components/shelly_app_common/app_common.cpp` (`shelly_led_refresh`)
- `source/components/shelly_switch_input/` and `source/components/shelly_maintenance/`
- `source/components/shelly_app_common/shelly_settings.cpp` (the switch mode and the change waiting for 3 presses)
- Each device's `app/app_main.cpp` UI wiring per device type (`shelly_ui_config_t`: toggle, local toggle, switch role, switch handler, and on the 2PM the channel count and relay 2's functions), and the wall-switch callbacks that hand every change to `shelly_ui_switch_changed()` (`shelly_app_common/shelly_contact_sensor.cpp` for the Shelly 1 Gen4 Opener, `shelly_switch_binding.cpp` for the Light Switch)

**When you design or change a UI behavior, walk the whole matrix so no mode is left out:**
- **Inputs:** tap, 2 taps, 3 or more taps, hold released at 1-5 s, 5-10 s, 10-20 s, and 20 s or more; wall switch change to on and to off, in each switch mode (Toggle, Push button, Follow, Disabled); power-on.
- **States:**
  - booting (Matter not up);
  - normal (paired or not, pairing window open or closed, joining or connected, Identify, fault);
  - first 60 s after an update (image pending verify);
  - Matter update downloading or installing (LED-16);
  - built without Wi-Fi mode;
  - Wi-Fi maintenance mode (including an upload in progress, and a change on the page waiting for 3 presses).
- **Devices and types:** every device type on every device (`device_profile.h`): Light and Outlet (SW toggles, in the wall switch modes), the Shelly 1 Gen4 Opener (SW is a contact sensor, a toggle is a pulse), the Mini's Opener (a toggle is a pulse, SW keeps the modes), Light Switch (SW drives bindings); the 2PM has two relays (a tap is relay 1, 2 taps relay 2, SW1 and SW2 each drive their own relay in their own mode). Also a change of type, which may need pairing again (P-05).
- **Feedback:** every action must show something (a relay change, LED-14 refused or LED-15 accepted). The one documented exception: a Follow-mode switch change when the relay is already in that state (the relay already shows it). Nothing may happen while the button is held. Actions fire on release.

Other docs (`COMMISSIONING.md`, `FLASHING.md`, `UPDATING.md`, `PROTECTION.md`, `GETTING-STARTED.md`, `README.md`) link to `BUTTON-GUIDE.md` or `BUTTON-AND-LED.md` rather than restating them; `BUTTON-GUIDE.md` is the one deliberate restatement. If one of them mentions a gesture or LED pattern, keep it consistent or replace it with a link.

## Other rules

- **Reversibility:** the build refuses secure boot, flash encryption, eFuse writes and any partition table other than the stock layout (`scripts/check-reversibility.py`, run on every build). Never weaken it. See `docs/REVERSIBILITY.md`.
- **Matter OTA compatibility:** existing devices update over Matter OTA and keep their pairing. In each device's `app/app_main.cpp`, keep, for every device type, the order and set of `::create` calls, the endpoint IDs, the clusters and the NVS keys that type's product line has always had, and keep the device type values stored in NVS (`device_profile.h`). Every product line of a device is the same firmware: build and release them from the same commit.
- **Hardware:** nothing is verified on hardware unless a doc says so. Don't claim untested behavior works.
- **Build:** in the builder container, `idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.c6_thread_shelly build` (see `docs/BUILDING.md`).
- **Host tests:** `source/components/test_host/run.sh` needs only a host C compiler and python3.
- **CI:** every push to `main` and every pull request runs the host tests, `scripts/check-web.sh` and a build of every product line in which a warning in this repository's code is an error (`SHELLY_WERROR=1 scripts/ci-build.sh <product line>`). Keep new code free of warnings.
- **Versions:** calendar-based, `YYYY.MM.PATCH`, the same for every device and product line. Only the Release workflow changes them (`scripts/bump-version.py`); don't edit `VERSION`, `PROJECT_VER` or the software version in `CHIPProjectConfig.h` by hand. Add changes to `CHANGELOG.md` under `### [Unreleased]` in **All devices**. See `docs/RELEASING.md`.
