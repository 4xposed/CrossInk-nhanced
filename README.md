> **This is a personal fork of [CrossInk](https://github.com/uxjulia/CrossInk)** with Anki support and OPDS Servers with .xtc files.

### Supported Devices

- Xteink X3
- Xteink X4
- Xteink X4 Pro
- Xteink X4 Classic
- Seeed Studio Sticky

## What's different in this fork

My goal with this fork was to extend CrossInk to add support for:
- Anki decks for langauge learning.
- Support OPDS Servers which serve files in .xtc format.
- Support for Yomitan dictionary.

---

## Installation

The fastest way to install Crossink is by using Inky, Crossink's web companion app: https://inky.crossink.dev/#flash-tools

Download a `firmware-*.bin` from the [releases page](https://github.com/4xposed/CrossInk-nhanced/releases), then flash it with the web installer or command line.

Once running firmware from this fork, use Update in Settings to connect to Wi-Fi and install a newer stable release from `4xposed/CrossInk-nhanced`. The updater selects the binary for your device, stages it on the SD card, validates it, and restarts after installation. Keep an SD card inserted with enough free space for the firmware download.

Release maintainers can run the **Build release assets** workflow with a version such as `0.1.1` to create a draft release with device-specific binaries, then publish it when ready. OTA checks published stable releases; drafts and prereleases are not offered. The release version must be newer than the installed version. Devices still running firmware that checks the upstream repository need a one-time manual flash of this fork's firmware to switch update sources.

See [Installation](./docs/installation.md) for step-by-step flashing and revert instructions.

---

## Guides & Documentation

Visit [https://www.crossink.dev](https://www.crossink.dev) for more user guides and additional documentation.

---

## Development quick start

CrossInk uses PlatformIO for building and flashing firmware. See [Getting Started](./docs/development/getting-started.md) for prerequisites, clone setup, and validation commands.

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

Connect your device to your computer via a USB cable. Before the first build, initialize the repository's submodules (including `freeink-sdk`):

```sh
git submodule update --init --recursive
```

Then flash the firmware using the correct environment for the device. The `default` environment is for the X3/X4 devices. ESP32-S3 devices have their own named environments.

```sh
pio run -e default --target upload
```

If PlatformIO reports `PackageException: Can not create a symbolic link for freeink-sdk/libs/hardware/BatteryMonitor, not a directory`, the `freeink-sdk` submodule is not initialized. Run the submodule command above and retry.

See [Testing and Debugging](./docs/development/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

---

## Notice on Contributions

This repository does not accept pull requests. Feature requests may be opened in [discussions](https://github.com/uxjulia/CrossInk/discussions), but major features requiring ongoing support should be directed upstream to [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader).
