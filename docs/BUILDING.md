# Building from Source

**[README](../README.md)** > **Building from Source** · [Report an issue](../../../issues/new)

This guide covers building the Automatous firmware from source. If you just want to flash pre-built firmware, see the [Flashing Guide](FLASHING.md) instead. You do not need to build anything.

Builds run in a dev container. Everything the build needs is installed in the image at fixed versions, and produces the same firmware as a build anywhere else.

---

## Contents

- [Requirements](#requirements)
- [Repository structure](#repository-structure)
- [Set up the container](#set-up-the-container)
- [Build](#build)
- [The Wi-Fi mode page](#the-wi-fi-mode-page)
- [Build the release artifacts](#build-the-release-artifacts)
- [Flash your build](#flash-your-build)

---

## Requirements

- **[Docker](https://docs.docker.com/get-docker/)**
- **VS Code** with the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension, for the editor workflow. Optional — the container also runs from a plain shell.
- **macOS or Linux.** The Windows build path is not currently tested.
- **Node 20.19 or later**, only to change the [Wi-Fi mode page](#the-wi-fi-mode-page). The firmware build doesn't need it.

The container is the only supported build environment. A local ESP-IDF and esp-matter install may work, but it isn't tested, and SDK version drift is the usual reason a build comes out different from the released firmware. If a problem doesn't reproduce in the container, it's hard to help.

The image is built from `.devcontainer/Dockerfile` and contains:

| Component | Version |
|---|---|
| ESP-IDF | `v5.5.2` |
| ESP-Matter | `2cb668c95de4f24786d20b7cb03c171d6e27b79e` |
| connectedhomeip (esp-matter submodule) | `8f943388af4d12dc5c484eae21b22723e03c3616` |

These are the versions the released binaries were produced with. Newer versions may build, but they are not what shipped.

ESP-Matter is used as a cloned repository rather than a component from the registry, and the build reads it through `ESP_MATTER_PATH`. The pinned esp-matter commit is on `main`, not a release branch, which is why no published `espressif/esp-matter` image matches it and the image is built here instead.

connectedhomeip is a submodule of esp-matter, so checking out the esp-matter commit and updating its submodules pulls the matching connectedhomeip commit automatically — it is not pinned separately. It lives in Espressif's connectedhomeip fork, not the upstream CSA repository, so those commits will not resolve there.

The Dockerfile pins the toolchain. The `dependencies.lock` in each variant directory pins the components the build pulls from the registry. Between them every input is pinned, so rebuilding a commit gives you the binaries that commit produced.

---

## Repository structure

The repository organizes firmware by device. Each device has one app, in its `app/` directory, and one ESP-IDF project per Matter product line, which sets only that line's Product ID and default device type:

```
source/
├── components/          # Shared drivers and logic used by every device (relay, inputs, LED,
│                        # thermal, diagnostics, device types, the Wi-Fi mode page, and the
│                        # 1PM's and 2PM's meter, metering and protection)
├── shelly-1-gen4/
│   ├── app/             # The app: Light, Outlet, Opener (door sensor on SW), Light Switch
│   ├── light/           # Product ID 0x8001, default type Light (released as its own build before 3.0.0)
│   ├── light-switch/    # 0x8002, Light Switch
│   ├── opener/          # 0x8003, Opener
│   └── outlet/          # 0x8004, Outlet
├── shelly-1-mini-gen4/
│   ├── app/             # The app: Outlet, Opener (no door sensor), Light, Light Switch
│   ├── outlet/          # 0x8005, Outlet
│   └── light-switch/    # 0x8006, Light Switch (released by the original project before 3.0.0)
├── shelly-1pm-gen4/
│   ├── app/             # The app: Outlet, Light, with metering and protection
│   └── outlet/          # 0x8009, Outlet
└── shelly-2pm-gen4/
    ├── app/             # The app: Outlet, Light, two relays with metering each
    ├── outlet/          # 0x8007, Outlet
    └── light/           # 0x8008, Light
```

Each product line directory is a self-contained ESP-IDF project: its `main/CMakeLists.txt` compiles the device's `app/` sources, and it pulls in the shared components from `source/components/`. Its `sdkconfig.defaults.c6_thread_shelly` differs from the device's other lines only in `CONFIG_DEVICE_PRODUCT_ID` and `CONFIG_SHELLY_DEFAULT_TYPE_*`. The pins and board options live in the "Shelly board" menu of `app/Kconfig.app`, which each line's `main/Kconfig.projbuild` sources. Build commands run from inside a product line directory. Build and package every line of a device from the same commit: they are the same firmware.

The pure logic in the shared components has host tests that need only a C compiler and python3, not the container. It covers:
- the power meter protocol, energy accounting, and the protection and thermal state machines;
- the button gestures and what each input does in each state;
- the LED patterns;
- Wi-Fi maintenance mode's upload check and captive-portal DNS;
- the Matter setup code: the 11-digit manual code, its check digit and the saved record;
- the diagnostics log ring, the hardware snapshot's storage and text forms, and the ADE7953 register table (it must never list a register whose read changes the chip);
- the ADE7953 codec (its scaling against the 2PM's measured runs, and the only two writes the driver may send), the meter calibration logic, and the metering helpers it uses. The script also fails if the ADE7953 driver writes the chip anywhere but its one guarded write function.

```bash
source/components/test_host/run.sh
```

The manual-code encoder was cross-checked against CHIP's own generator (`src/setup_payload/python/SetupPayload.py`, in the builder container): `test_setup_code vectors 60` prints random codes to compare. The QR payload tests use vectors from the same generator. The page's QR image comes from Nayuki's qrcodegen (vendored in `shelly_maintenance/qrcodegen`, MIT); its output was checked module for module against the independent `segno` encoder.

The same script runs `scripts/check-ui-spec.py`, which fails if [Button, Switch and LED](BUTTON-AND-LED.md) and the code disagree. Change them together (see [CLAUDE.md](../CLAUDE.md)).

### Diagnostics build

For troubleshooting a device with no serial cable, `CONFIG_SHELLY_DIAG_LOG` (off by default, in the "Shelly diagnostics" menu) keeps the last 6 KB of the log in memory that survives a restart and shows it on the [Wi-Fi mode](UPDATING.md#wi-fi-maintenance-mode) page (**Diagnostics log: Open**). It also:
- logs free heap (current, lowest and largest block), the Thread role, and the device's addresses as OpenThread and lwIP see them, every 5 s while pairing;
- freezes the log 5 s after a failed pairing, so it survives until you read it. A power cut clears it; so does **Clear** on the page.

It costs 6 KB of heap for the whole run. Build it into its own directory so the normal build is untouched, for example the 1PM:

```bash
cd source/shelly-1pm-gen4/outlet
idf.py -B build_diag -DSDKCONFIG=sdkconfig.diag \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.c6_thread_shelly;sdkconfig.defaults.diag" set-target esp32c6 build
```

Install its `build_diag/outlet.bin`, packaged as an `.ota`, from the Wi-Fi mode page. Go back by uploading the normal `.ota` the same way.

### Board bring-up data

For a board that isn't verified yet, `CONFIG_SHELLY_HW_SNAPSHOT` ("Shelly hardware snapshot" menu) records what the hardware reads, for the Wi-Fi mode page. It is off by default and on in the Shelly 2PM Gen4 builds, together with the diagnostics log:
- **The snapshot**, every 10 s of a normal run: each relay as written and as its pin reads back (`CONFIG_SHELLY_RELAY_READBACK`), each wall switch pin and how many edges it saw, the button pin, the NTC's ADC reading and temperature, the die temperature and the heap. A line a minute also goes to the diagnostics log. It lives in memory that survives the restart into Wi-Fi mode (about 450 bytes); a power cut clears it.
- **The ADE7953's registers**, on a board with that meter: the meter driver copies the chip's identity, configuration, gains, interrupt registers and measurements into the snapshot once a normal run's snapshot has started, then the measurements every minute. It never reads a register whose read resets it. (The 1.0.0 bring-up build had a separate read-only probe for this.)
- **On the page:** the Hardware data on the Diagnostics tab, and the raw `/api/hw`.
- **Shelly's stock data partitions** (`CONFIG_SHELLY_FACTORY_DUMP`, "Shelly Wi-Fi maintenance mode" menu, off in every build; turn it on only for a one-off build that reads a new device): links on the Hardware data card download a read-only copy of `shelly`, `fs_0`, `fs_1` or `zb_storage` (`/part?name=shelly`), the stock regions this firmware never uses, to find what Shelly keeps there, such as a factory meter calibration. It only reads flash, and never copies this firmware's own `nvs`. The `shelly` partition may hold the device's own keys and anyone who joins the Wi-Fi mode network could download it, so keep the option off outside bring-up and keep the copies private.

Wi-Fi maintenance mode is on by default (`CONFIG_SHELLY_MAINTENANCE`, in the "Shelly Wi-Fi maintenance mode" menu). The Wi-Fi driver's static data costs about 24 KB of RAM in normal operation, where Wi-Fi never runs. Turning the option off gives that back, and the 5 s hold is then refused.

Every build also runs `scripts/check-reversibility.py`, which fails the build if the firmware could stop a device from returning to stock. See [Reversibility](REVERSIBILITY.md#what-this-firmware-guarantees). The examples below build the Shelly 1 Gen4 `light` variant; to build a different one, substitute its hardware and variant directories in the `cd` command.

---

## Set up the container

Clone the repository:

```bash
git clone https://github.com/automatous-io/shelly-1-gen4-matter-thread.git
cd shelly-1-gen4-matter-thread
```

Then use **one** of the two below. Same environment either way.

### In VS Code

Open that folder and run **Dev Containers: Reopen in Container** from the Command Palette. VS Code builds the image, starts the container, and reopens the workspace inside it. Nothing else is needed — skip to [Build](#build).

### From a shell

If you are not using VS Code, build the image and start a container directly:

```bash
docker build -t automatous-io/shelly-gen4-builder:idf-v5.5.2-matter-2cb668c9 .devcontainer

docker run --rm -it \
  -v "$PWD":/workspaces/shelly-1-gen4-matter-thread \
  -w /workspaces/shelly-1-gen4-matter-thread \
  -e SDKCONFIG_DEFAULTS=sdkconfig.defaults.c6_thread_shelly \
  automatous-io/shelly-gen4-builder:idf-v5.5.2-matter-2cb668c9 bash
```

The first build takes roughly 20–25 minutes and produces an image of about 18 GB. It compiles the Matter SDK's build environment. After that it starts in seconds, and it only rebuilds if the Dockerfile changes.

Both SDKs are exported in every shell in the container, and `SDKCONFIG_DEFAULTS` is already set. Note that esp-matter's CMake overrides that environment variable during a build, so [Build](#build) still passes the defaults file with `-D`. To confirm which revisions an image holds:

```bash
cat /opt/esp/esp-idf-version.txt          # v5.5.2
cat /opt/esp/esp-matter-commit.txt        # 2cb668c9...
cat /opt/esp/connectedhomeip-commit.txt   # 8f943388...
```

---

## Build

A variant has to be configured once before it can be built. From the repository root inside the container:

```bash
cd source/shelly-1-gen4/light
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.c6_thread_shelly set-target esp32c6
idf.py build
```

After that first `set-target`, the variant's `build/` directory records both the target and the defaults file, so every later build in that variant is just:

```bash
idf.py build
```

Repeat the two-command form for each variant you build, and again after deleting a variant's `build/` directory or `sdkconfig`.

Both flags are needed, for different reasons:

- `-DSDKCONFIG_DEFAULTS` supplies the C6 and Thread settings. The container exports `SDKCONFIG_DEFAULTS` too, but esp-matter's `cmake_common/components_include.cmake` overwrites the CMake variable of that name, and ESP-IDF only reads the environment when that variable is empty. Passing `-D` puts the file where esp-matter won't drop it.
- `set-target esp32c6` sets `IDF_TARGET` on the CMake command line. Each variant's `CMakeLists.txt` picks the device HAL before `project.cmake` runs, and an empty `IDF_TARGET` there is treated as `esp32`. Without it a clean checkout picks the esp32 HAL and stops with `please set esp32 as the IDF_TARGET`.

The build produces individual `.bin` files in the `build/` directory. The application image is named after the variant; the light build produces `build/light.bin` and the opener build produces `build/opener.bin` and so on. The `Project build complete` line at the end of the build prints its exact path.

A `build/` directory is tied to the environment that created it, because CMake caches absolute toolchain paths. If you switch between containers, delete `build/` and `sdkconfig` first.

---

## The Wi-Fi mode page

The page that [Wi-Fi maintenance mode](UPDATING.md#wi-fi-maintenance-mode) serves is a Svelte 5 app in `source/components/shelly_maintenance/web/`, written in TypeScript. Vite builds it into one self-contained HTML file: no fonts, images or scripts load from elsewhere, because the device's network has no internet. `scripts/gzip.js` compresses that file into `source/components/shelly_maintenance/page.html.gz`.

**The built file is committed.** The firmware build only embeds `page.html.gz` and serves it with `Content-Encoding: gzip`, so it needs no Node and no network, and the builder image stays as it is. After changing anything under `web/`, rebuild the page on the host (Node 20.19 or later) and commit `page.html.gz` with the source:

```bash
scripts/check-web.sh
```

It installs the pinned dependencies (`npm ci`), type-checks the source, rebuilds the page, and fails if the committed `page.html.gz` doesn't match the source. It compares the decompressed page, since another zlib may compress the same page to different bytes.

To work on the page without a device, run `npm run dev` in `web/`. A pretend device in `web/mock/device.ts` answers the page's API. `/__mock?d=light` (or `mini`, `1pm`, `2pm`, `2pm-skipped`) picks the device, `/__mock/press` stands in for the 3 button presses that save a change, and `/__mock/offline` stops the replies for 10 seconds, like a restart.

The page talks to the firmware through a small JSON API in `maintenance.cpp`:

| Request | What it does |
|---|---|
| `GET /api/state` | Everything the page shows, and `caps`: what this build has (relays, switch modes, meter, diagnostics, device type), which decides the tabs |
| `POST /api/set` | A setting, saved at once behind the Wi-Fi mode password; the password, and every setting on an open network, waits for 3 presses of the button (B-26): `{"key":"switch.mode","ch":2,"value":"follow"}`, `code`, `code.random`, `wifi.password`, `wifi.password.default`, `cal`, `cal.reset`, `profile` (`light`, `outlet`, `opener` or `light-switch`, as the build offers; `"repair":true` confirms a type that needs pairing again), or `cancel` |
| `POST /api/action` | Something that happens at once: `toggle` a relay, `diag.clear`, `exit` |
| `GET /api/meter`, `GET /api/hw` | Live meter readings, and the hardware data, polled only while their tab is open |
| `POST /update` | The firmware upload: the raw `.ota` file as the body |
| `GET /recovery` | A simple install page in plain HTML, with no code shared with the main page |

`/recovery` is the safety net: if the main page ever breaks, firmware can still be installed. The main page also shows a link to it if its script fails to start or throws.

---

## Build the release artifacts

Each install and update path needs a different packaged artifact, and all three come from the same build:

| Artifact | Path it serves | Built with |
|---|---|---|
| `automatous-io-{hardware}-{variant}-vX.Y.Z.bin` | UART / ESPConnect full install | `idf.py merge-bin` |
| `automatous-io-{hardware}-{variant}-vX.Y.Z-ota.zip` | Shelly web UI install | `scripts/make-webui-ota-zip.py` |
| `automatous-io-{hardware}-{variant}-vX.Y.Z.ota` | Matter OTA update | `scripts/make-matter-ota.py` |

The examples below build the `light` variant; for another, point the script at its directory or substitute its name.

### Merged binary (UART)

A single `.bin` for flashing at offset `0x0`, matching the released binaries. From the variant directory:

```bash
idf.py merge-bin
```

It reads the flash mode, flash size, and every partition offset from the build's flash arguments and writes `build/merged-binary.bin` in the `/build` directory. Rename it to `automatous-io-{hardware}-{variant}-v{version}.bin`.

### Web UI package

The `.zip` that installs through the Shelly web UI. Run it from the repository root pointed at a variant directory:

```bash
python3 scripts/make-webui-ota-zip.py source/shelly-1-gen4/light
```

It reads the variant and version from the build, bundles the bootloader, partition table, otadata, and application with an empty filesystem image, and writes `automatous-io-{hardware}-{variant}-vX.Y.Z-ota.zip` next to the build.

### Matter OTA image

The `.ota` served to commissioned devices through Home Assistant:

```bash
python3 scripts/make-matter-ota.py source/shelly-1-gen4/light
```

It calls `ota_image_tool.py` from the connectedhomeip checkout, found via `ESP_MATTER_PATH`. It has to run inside the container. It reads the vendor and product ID from the build's `sdkconfig` and the software version from `CHIPProjectConfig.h`; the image always matches the firmware it came from and can only target the device and variant it was built for. The output is `automatous-io-{hardware}-{variant}-vX.Y.Z.ota`. See [Updating](UPDATING.md) for how to serve it.

---

## Flash your build

This is for pushing a build you just made onto a device already running this firmware. For a first install, wiring, or putting stock firmware back, see the [Flashing Guide](FLASHING.md).

The container has esptool and pyserial, so it can flash directly. What it needs is access to the serial port.

Put the Shelly into [flash mode](FLASHING.md#enter-flash-mode) and connect the UART adapter first.

### Pass the port through (Linux, Docker Engine)

This requires plain Docker Engine — Docker Desktop runs a virtual machine that cannot see USB devices, on any platform. On macOS and Windows, build in the container and flash from the host instead, following [Flash with USB-UART](FLASHING.md#flash-with-usb-uart).

Add the device to `runArgs` in `.devcontainer/devcontainer.json`, adjusting the path to your adapter:

```jsonc
"runArgs": ["--add-host=host.docker.internal:host-gateway", "--device=/dev/ttyUSB0"],
```

Rebuild the container, then from the variant directory:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Don't commit that line. Docker won't start a container when the device is missing, so it breaks for everyone who isn't flashing.

### What flashing does

```bash
idf.py -p <PORT> flash monitor
```

This writes the bootloader, partition table, and application image but leaves the `nvs` partition intact; Matter fabric and Thread credentials are preserved and the device does not need re-commissioning. Do not use `idf.py erase-flash`, which wipes everything including commissioning.

For a first flash, or to install or update through a packaged artifact, see [Build the release artifacts](#build-the-release-artifacts) for which file each path uses, then the [Flashing Guide](FLASHING.md) for the merged `.bin` or web UI `.zip`, or [Updating](UPDATING.md) for the Matter `.ota`.

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
- [GPIO Map](GPIO.md) — ESP-Shelly-C68F pin assignments for Gen4 devices
- [Certification](CERTIFICATION.md) — uncertified status and test credentials
- [Roadmap](ROADMAP.md) — current known limitations and planned work
- [Contributing](CONTRIBUTING.md) — reporting bugs and the firmware filename convention
- [Contributors](CONTRIBUTORS.md) — people who have helped move the project forward
