# CLAUDE.md

Guidance for Claude (and anyone else) changing this repository.

## The device UI is a specification: keep it in sync

The button, the wall switch (SW) and the status LED are the device's only local interface. [docs/BUTTON-AND-LED.md](docs/BUTTON-AND-LED.md) specifies all of it: every gesture, what it does in every state, every LED pattern and every timing.

**Any change to how an input or the LED behaves must, in the same commit:**
1. Update `docs/BUTTON-AND-LED.md`.
2. Update the matching rows in `source/components/test_host/test_ui.c`: same ID, same words.
3. Pass `source/components/test_host/run.sh`. It runs the UI tests and `scripts/check-ui-spec.py`, which fails if the doc and the code disagree in any row.

**This applies to changes in:**
- `source/components/shelly_button/` (`button_logic.h` holds the gesture timings, T-xx)
- `source/components/shelly_status_led/` (`led_patterns.h` holds every pattern, LED-xx)
- `source/components/shelly_app_common/ui_policy.c` (what each input does in each state, B-xx and SW-xx) and `shelly_ui.cpp` (carrying it out)
- The LED and commissioning-window handling in `source/components/shelly_app_common/app_common.cpp` (`shelly_led_refresh`)
- `source/components/shelly_switch_input/` and `source/components/shelly_maintenance/`
- Each variant's `main/app_main.cpp` UI wiring (`shelly_ui_config_t`: toggle, local toggle, switch role, switch handler), and the wall-switch callbacks that hand every change to `shelly_ui_switch_changed()` (the Opener's `contact_sensor.cpp`, the Light Switch's `switch_binding.cpp`)

**When you design or change a UI behavior, walk the whole matrix so no mode is left out:**
- **Inputs:** tap, 2 taps, 3 or more taps, hold released at 1-5 s, 5-10 s, 10-20 s, and 20 s or more; wall switch change; power-on.
- **States:**
  - booting (Matter not up);
  - normal (paired or not, pairing window open or closed, joining or connected, Identify, fault);
  - first 60 s after an update (image pending verify);
  - Matter update downloading;
  - built without Wi-Fi mode;
  - Wi-Fi maintenance mode (including an upload in progress, and a setup-code change waiting for 3 presses).
- **Variants:** Light, Outlet, Mini and 1PM (SW toggles), Opener (SW is a contact sensor, a toggle is a pulse), Light Switch (SW drives bindings).
- **Feedback:** every action must show something (a relay change, LED-14 refused or LED-15 accepted). Nothing may happen while the button is held. Actions fire on release.

Other docs (`COMMISSIONING.md`, `FLASHING.md`, `UPDATING.md`, `PROTECTION.md`, `README.md`) link to `BUTTON-AND-LED.md` rather than restating it. If one of them mentions a gesture or LED pattern, keep it consistent or replace it with a link.

## Other rules

- **Reversibility:** the build refuses secure boot, flash encryption, eFuse writes and any partition table other than the stock layout (`scripts/check-reversibility.py`, run on every build). Never weaken it. See `docs/REVERSIBILITY.md`.
- **Matter OTA compatibility:** existing devices update over Matter OTA and keep their pairing. In each variant's `app_main.cpp`, keep the order and set of `::create` calls, the endpoint IDs, the clusters and the NVS keys.
- **Hardware:** nothing is verified on hardware unless a doc says so. Don't claim untested behavior works.
- **Build:** in the builder container, `idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.c6_thread_shelly build` (see `docs/BUILDING.md`).
- **Host tests:** `source/components/test_host/run.sh` needs only a host C compiler and python3.
