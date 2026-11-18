<div align="center">

<img src="launcher/icon.jpg" alt="Asphalt 6: Adrenaline" width="160">

# a6_nx

**Asphalt 6: Adrenaline HD on Nintendo Switch**

An unofficial Nintendo Switch wrapper for the Android version of
**Asphalt 6: Adrenaline HD**.

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Version](https://img.shields.io/badge/Version-0.8.0-4C8BF5?style=for-the-badge)](#)
[![ARM](https://img.shields.io/badge/ARM-32--bit-0091BD?style=for-the-badge&logo=arm&logoColor=white)](#)

</div>

---

## About

`a6_nx` is a native wrapper that runs the 32-bit (ARMv7) Android build of
**Asphalt 6: Adrenaline HD** on Nintendo Switch. It loads the game's own Glitch
engine library and recreates the services that library expects under Horizon OS:
Android, JNI, OpenSL ES audio, input and OpenGL ES 2. The process runs in
AArch32 mode, on libnx32 and a 32-bit build of Mesa.

This release targets **Asphalt 6: Adrenaline HD 1.3.3**
(`com.gameloft.android.HEP.GloftA6HP`, version code 6). It is tested with the
repacked APK for 64-bit phones. That APK keeps the original 32-bit engine as
`lib/arm64-v8a/libguest_asphalt6.so`, and the wrapper runs that library
directly. The repack's 64-bit translator libraries are not used.

No game code or data is included. The game data is unpacked from your APK on
the first launch.

---

## Controls

| Input | Action |
| --- | --- |
| **Left Stick** | Steer |
| **R / ZR** | Accelerate |
| **ZL / B** | Brake |
| **A / X** | Nitro |
| **Y** | Change camera |
| **+** | Pause |
| **-** | Back |
| **L3 + R3** | Easter egg: the developers' free cameras |
| **Touchscreen** | Touch input in handheld mode, as on a phone |

In menus the D-Pad moves, A confirms and B goes back. In the pause menu the
D-Pad moves, A confirms, and B or + resumes the race. The D-Pad does not steer
in races.

**Pointer:** in menus and in the pause menu, **L** switches a pointer on and off.
While it is on, the **Right Stick** moves it and **ZR** taps where it points. Hold ZR to drag. This reaches buttons the D-Pad cannot,
such as paint, tuning and buy on the car screen. While the pointer is off,
**ZL / ZR** go to the previous / next entry, for example the cars and categories
of the car selection.

**Garage:** the **Left Stick** or the D-Pad walks, the **Right Stick** looks around
(both go straight to the garage camera, so looking never picks a car), and **A** or
**ZR** taps the crosshair in the middle of the screen, for example to pick a car.

---

## Build

### Requirements

* Docker
* The vita2hos AArch32 toolchain image
  (`ghcr.io/vita2hos/devcontainer/vita2hos:latest`): devkitARM, switch-tools
  and miniz
* [libnx32](https://github.com/aks796/libnx32), the 32-bit libnx
* [mesa32](https://github.com/aks796/mesa32), the 32-bit Mesa 20.1 and
  libdrm_nouveau for Switch
* The `devkitpro/devkita64` image for the launcher

`tools/setup_deps.sh` fetches and builds libnx32 and Mesa into `deps/` and
`portlibs32/`. You only need to run it once.

```bash
./tools/setup_deps.sh
```

Compile the 32-bit game program, then the launcher that carries it:

```bash
./build.sh
launcher/build.sh
```

The output is `launcher/a6_nx.nro`. The build number is written into both
programs and appears in `debug.log`.

For a clean rebuild:

```bash
./build.sh clean
./build.sh
```

---

## Running

Requires Atmosphère and a homebrew menu that can install forwarders, such as
sphaira.

Put `a6_nx.nro` and your APK in this folder on the SD card:

```text
sd:/switch/a6_nx/
├── a6_nx.nro
└── a6.apk
```

The APK is recognized by what it holds, so its file name does not matter.

1. In the homebrew menu, choose **Asphalt 6: Adrenaline** and install a
   forwarder from its options.
2. Launch the new **Asphalt 6: Adrenaline** icon on the HOME menu.

The first launch unpacks the game data from the APK. This takes a while. It
then installs the game program as an Atmosphère ExeFS override for that icon
(`atmosphere/contents/<title id>/exefs.nsp`) and restarts it.

Afterwards the folder looks like this:

```text
sd:/switch/a6_nx/
├── a6_nx.nro
├── a6.apk
├── config.ini
├── external/files/     game data and saves (profile*.sav)
└── debug.log
```

To update, replace `a6_nx.nro`. The game updates its override on the next
launch.

### Settings (`config.ini`)

The file is written with its defaults on the first launch.

| Section | Setting | Values |
| --- | --- | --- |
| `[display]` | `resolution` | `auto`, `1080`, `720` |
| `[display]` | `frame_rate` | `30` (default), `60` |
| `[controls]` | `garage_look_speed` | `0.05` - `1` (default `0.3`): how fast the Right Stick looks around in the garage |
| `[controls]` | `steering` | `stick` (default), `motion`, `both` |
| `[controls]` | `steer_gain` | `0.2` - `1.5` (default `0.6`) |
| `[controls]` | `motion_gain` | `0.5` - `4` (default `1.5`) |
| `[controls]` | `motion_invert` | `false` (default), `true` |
| `[controls]` | `touch_screen` | `true` (default), `false` |
| `[game]` | `language` | `auto` (the console's), `en`, `fr`, `de`, `es`, `it`, `ja`, `pt-br`, `zh`, `ko`, `ru`, `pt`, `pl` |
| `[game]` | `promo_cars` | `true` (default): Audi TT-RS and Ferrari California unlock by stars; `false`: locked as in the original |
| `[debug]` | `cooperative` | keep `false`: `true` hangs the car selection |

---

## Status

**Version 0.8.0.** Boot, menus, sound and music, the garage and all 42
cars are working. Races run on several tracks, with results and achievements.
The controller works in races, menus, the pause menu and the garage.

### Roadmap

* **Car selection**
  * Look at the car with the Left Stick: left / right turns it.
  * D-Pad down reaches the buttons at the bottom (paint, tuning, buy, race).
* **Graphics settings:** check the in-game visual options.
* **Intro video:** play the game's intro (needs video playback).
* **Motion controls:** steering by tilting the console or controller.
* After 1.0.0: Music Player controls with the D-Pad, on-screen keyboard for
  name entry.

Online features are not available. The game runs as a device without a network
connection, so multiplayer and store features do not work.

The wrapper is built for version **1.3.3** of the game. Other versions have not
been tested.

---

## Credits

**Asphalt 6: Adrenaline Nintendo Switch port**: 7UNC46

**Asphalt 6: Adrenaline**: Gameloft

The runtime comes from [mc3_nx](https://github.com/Kaays/mc3_nx) (Kaays) and
[a8retry_nx](https://github.com/aks796/a8retry_nx) (aks796). The loader derives
from the open-source Android `.so` loader work by Andy Nguyen
(TheOfficialFloW) and fgsfds, ported to AArch32 with reference to vita2hos. It
is MIT-licensed. See `LICENSE`.

Uses stb_vorbis (Sean Barrett, public domain or MIT) and miniz (MIT). See
`THIRD_PARTY_NOTICES.md`.

Built with devkitPro, devkitARM, libnx32 (vita2hos) and a 32-bit build of Mesa.

---

## Contributing

Bug reports and tested improvements are welcome. Include your firmware and
Atmosphère versions, the steps to reproduce, and `debug.log` and
`debug.prev.log` from the game folder. If the game closed by itself, include
`crash.log` and the `a6_nx.elf` from your build as well.

---

## Disclaimer

This is an unofficial fan project and is not affiliated with, sponsored by or
endorsed by Nintendo or Gameloft. Asphalt and Asphalt 6: Adrenaline are
trademarks of Gameloft.

No game code or assets are included. You need your own copy of the game.
