# recovery-console

DRM terminal emulator for Android recovery (TWRP/etc).
Renders to `/dev/dri/card0` via dumb framebuffer. No GPU needed.

## Usage

```sh
# Spawn an interactive shell
./recovery-console

# Attach to /dev/console (init logs, login prompt, etc.)
./recovery-console /dev/console

# Any file/FIFO/device works
./recovery-console /proc/kmsg
```

## Display wake fix

TWRP blanks the display after its timeout by disabling the CRTC and writing 0
to the backlight sysfs node. This tool re-asserts `SETCRTC` + backlight every
`WAKE_INTERVAL_MS` ms AND immediately on any keypress, so the panel wakes up.

## Config (config.h)

| Define | Description |
|---|---|
| `DRM_DEVICE` | DRM device path |
| `BACKLIGHT_PATH` | sysfs backlight brightness node (empty to skip) |
| `BACKLIGHT_MAX` | Brightness value to write on wake |
| `STATIC_CONN_ID` / `STATIC_CRTC_ID` | Set to 0 for auto-detect |
| `FB_WIDTH` / `FB_HEIGHT` / `FB_REFRESH` | Panel resolution |
| `MODE_*` | DRM mode timings (get from `modetest` on device) |
| `FONT_SCALE` | Pixel multiplier (2 = 16x32 effective cell size) |
| `WAKE_INTERVAL_MS` | How often to re-assert display (default 2000ms) |
| `DEFAULT_SHELL` | Shell to spawn in interactive mode |

## Getting device IDs

Run on the device via `adb shell`:

```sh
# List connectors, CRTCs, current mode
modetest -M mediatek        # or whatever driver
# or from /sys
cat /sys/class/drm/card0-*/status
cat /sys/class/drm/card0-*/modes
```

For mode timings without `modetest`, read them from the kernel with:
```sh
cat /sys/kernel/debug/dri/0/state
```

## Build (static, musl)

```sh
make aarch64   # for Android arm64
make armhf     # for 32-bit ARM
```

Requires musl cross-compiler. Get from https://musl.cc or build with
`musl-cross-make`.

## Detach keys

- `Ctrl-]` - detach
- `Ctrl-A d` - detach (screen-style)
