# Releasing

**[README](../README.md)** > **Releasing** · [Report an issue](../../../issues/new)

Releases are cut by a GitHub Actions workflow. It builds every product line at the new version, packages them, and publishes them as a GitHub release. Nobody packages a release by hand.

---

## Versions

Every device and product line shares one version, `YYYY.MM.PATCH`: the year and month of the release (month zero-padded) and a patch number that starts at 0 each month. The tag is `vYYYY.MM.PATCH`, for example `v2026.10.0`. `VERSION` at the repository root holds the current one.

Matter compares a device's software version as a 32-bit number and only offers an update to a higher one. A version's number is `YYYY*10000 + MM*100 + PATCH`:

| Version | Matter software version |
|---|---|
| 3.0.0 (the last SemVer version) | 196608 (0x00030000) |
| 2026.10.0 | 20261000 (0x01352888) |
| 2026.10.1 | 20261001 |
| 2026.11.0 | 20261100 |

Every calendar version is above every earlier release and each release is above the one before, so devices always see a release as newer. PATCH stops at 99 in a month.

There are no release-candidate versions. A candidate and its final release would share a number, and Matter would never move a device from one to the other. Mark a release you haven't tried on hardware as a pre-release instead.

`scripts/bump-version.py` is the only thing that changes the version. It writes it into `VERSION`, each product line's `CMakeLists.txt` (`PROJECT_VER`, which the Wi-Fi mode page shows and compares) and `main/CHIPProjectConfig.h` (the Matter software version and its string). Run `scripts/bump-version.py check` to confirm they agree.

---

## Cut a release

1. Make sure `CHANGELOG.md` has a `### [Unreleased]` section under **All devices** that describes the changes.
2. Open **Actions > Release > Run workflow** on `main`. Tick **Mark it as a pre-release** unless the build has been tried on hardware.
3. Wait about half an hour.

The workflow:

1. Works out the version from today's UTC date and `VERSION`: the next patch this month, or `.0` in a new month.
2. Runs the host tests and builds all 9 product lines at that version in the builder image, with warnings in this repository's code as errors.
3. Packages each build (`scripts/ci-package.sh`) and checks that each `.ota` header carries the line's product ID and the new version.
4. Only then commits the version and the CHANGELOG (`chore(release): vYYYY.MM.PATCH`), tags the commit and pushes both to `main` together. If `main` moved while the build ran, the push fails and nothing is released; run it again.
5. Publishes the GitHub release: the CHANGELOG section, a table of which file to download for each device, the Matter software version, and the merged pull requests.

Each release holds 28 files:

| File | Use |
|---|---|
| `{hardware}-{line}-vYYYY.MM.PATCH.ota` | Updates: Matter OTA, or the Wi-Fi mode page |
| `{hardware}-{line}-vYYYY.MM.PATCH-ota.zip` | The first install from the stock Shelly web UI |
| `{hardware}-{line}-vYYYY.MM.PATCH-uart.bin` | A UART install at `0x0` |
| `SHA256SUMS.txt` | The SHA-256 of every file above |

To release a commit that already carries its version, push its tag instead (`git tag -a v2026.10.0 && git push origin v2026.10.0`). The workflow checks that `VERSION`, the sources and the CHANGELOG all say that version, and publishes it without committing anything.

---

## After a pre-release

Install the pre-release on a device of each kind you can test, from the Wi-Fi mode page or Matter OTA. When it works, edit the release on GitHub, untick **Set as a pre-release** and tick **Set as the latest release**.

---

## Related documentation

- [Building from Source](BUILDING.md), including [continuous integration](BUILDING.md#continuous-integration)
- [Updating](UPDATING.md)
- [Flashing](FLASHING.md)
