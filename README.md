<p align="center"><img src="assets/brand/banner.svg" alt="Project Nativ: your PS Vita games, rebuilt natively" width="720"></p>

# Dead or Alive 5 Plus, rebuilt natively

A native port of **Dead or Alive 5 Plus** (PS Vita) for macOS, Linux, Windows and Android, built on
[Project Nativ](https://github.com/TheLostPantheon/nativ).

This repository is a fork of Project Nativ. It adds the game bring-up for Dead or Alive 5 Plus — a pack, the runtime
work the game needed, and an Android target — on top of upstream's static recompiler and runtime. Upstream's own
support for Killzone: Mercenary, Uncharted: Golden Abyss, Unit 13 and Shovel Knight is unchanged and still here.

**You bring your own copy of the game and your own firmware.** Nothing of either ships in this repository, and nothing
ever will.

## Credits

This port exists because of other people's work.

- **[Project Nativ](https://github.com/TheLostPantheon/nativ)** by TheLostPantheon and its contributors: the static
  recompiler, the runtime, the GPU backends, the audio stack, the pack system and the documentation. Essentially all of
  the code here is theirs. This fork's changes sit on top of it.
- The projects Nativ itself is ported from and built with — Dear ImGui, stb, LibAtrac9, Monocypher, SDL3,
  SPIRV-Cross, FFmpeg and others — are listed with their licences in [THIRD_PARTY.md](THIRD_PARTY.md).
- **Dead or Alive 5 Plus** is © Koei Tecmo. This project is unaffiliated with them and ships none of their code or
  data.

## What works

Measured on a Retroid Pocket Nova (Snapdragon 8 Gen 2, Adreno 740, Android 16) and on Apple silicon macOS.

| Area | State |
|---|---|
| Boot, title screen, menus | Runs at 60 fps |
| Arcade, Versus, training | Plays at 60 fps on most stages; the heaviest stages sit around 42–52 fps |
| Story mode | Plays, cutscenes at 60 fps |
| Intro and story movies | Play (SceMp4 through FFmpeg's demuxer) |
| Sound | Music, voices and effects through the NGS mixer, with ATRAC9 decoding |
| Graphics | Vulkan/Metal/D3D12 through SDL GPU, with the game's USSE shaders translated |
| 4:3 displays | Anamorphic mode: the picture keeps its 4:3 shape while the HUD widens to the screen |
| Android | Sideloadable APK, arm64, Vulkan, AAudio, SDL gamepad |

Known gaps: online play and trophies are stubs for this title, and the heaviest stages are CPU-bound rather than
GPU-bound, so a faster device helps less than one might hope. Save data works.

## Nothing of the game ships

This repository holds no game code, no game data and no firmware. The pack under `packs/PCSB00296/` contains only facts
about the game — its title ID, the SHA-256 of its main program, module names and settings — plus shader recipes. Your
game data is decrypted and recompiled on your own machine and never leaves it.

You need:

- **Dead or Alive 5 Plus, EU release, version 1.00, title ID `PCSB00296`**, with a licence file (`work.bin` or a
  `.rif`) that decrypts it. The pack is matched by the SHA-256 of the game's main program
  (`48820fdd29262db7a3c0bf93c4090a652b9dd6b8aa8dfad5de8a3389d63d78cd`), so other regions and updates are not
  supported yet.
- **Sony's PS Vita firmware update file** (a `.PUP`), from Sony's own download site. The runtime's system libraries are
  recompiled from it.

## Building it

These are the steps for a machine that has never built this before. The pipeline is Python plus a C toolchain; the game
build links a toolchain of pinned libraries that a script downloads and compiles once.

### 1. Prerequisites

- **Python 3.10+** and `git`.
- **A C/C++ compiler and CMake/Ninja** for the toolchain build: Apple's Command Line Tools (`xcode-select --install`) on
  macOS, a normal build environment on Linux, and on Windows either Visual Studio's build tools or a Linux/macOS
  machine to make the toolchain on.
- **Android only:** the Android NDK (r26+), SDK platform 35 with build-tools 35.0.0, and a JDK (17 or 21).

### 2. Get the code and its Python packages

```sh
git clone https://github.com/hjcoggan/doa5-v-arm.git
cd doa5-v-arm

python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

`./vitarecomp` uses `.venv/bin/python` when it exists. On Windows, call the driver directly from PowerShell:

```powershell
.\.venv\Scripts\python.exe .\pipeline\vitarecomp.py --help
```

### 3. Build the toolchain once

The runtime links SDL3, SPIRV-Cross and FFmpeg's H.264 decoder, built from pinned sources:

```sh
# macOS (needs the Command Line Tools, cmake and ninja)
runtime/platform/macos/setup-toolchain.sh

# Linux x86_64 (needs SDL's backend headers and cmake/ninja; see the script's header)
runtime/platform/linux/setup-toolchain.sh
```

Each script builds into `~/toolchains/ativ-<os>` by default and prints where it put it. Nothing built here is needed on
the machine that runs the game.

### 4. Decrypt the firmware and add your game

```sh
./vitarecomp firmware /path/to/PSVita-Update.PUP
./vitarecomp scan /path/to/your/vita/dumps     # finds games, updates and firmware at any depth
./vitarecomp add /path/to/PCSB00296            # a decrypted game folder, or its VPK plus a licence
```

Your games, firmware and builds live in one library folder: `dump/` inside the checkout (git-ignored), or your per-user
data folder (`~/Library/Application Support/ProjectNativ/Nativ/library` on macOS, `~/.local/share` on Linux,
`%LOCALAPPDATA%` on Windows), or wherever `ATIV_DUMP` points. The commands print the exact paths.

### 5. Analyze, recompile and build the game

```sh
./vitarecomp analyze  PCSB00296
./vitarecomp recompile PCSB00296
./vitarecomp build    PCSB00296
./vitarecomp run      PCSB00296
```

`analyze` finds the game's functions (32,784 of them here), `recompile` translates the ARMv7/Thumb-2/VFP/NEON code to C
(about 3.7 M guest instructions), and `build` compiles that C into `doa5_host`. The pack's `RUN_ENV` turns off target
alias sync, which costs several milliseconds a frame here and changes nothing on screen.

The first three stages take a while; later runs only redo what changed.

### 6. Android (optional)

```sh
export ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.2.12479018
export JAVA_HOME=/opt/homebrew/opt/openjdk@21          # JDK 17 or 21
runtime/platform/android/setup-toolchain.sh            # builds SDL3 for arm64 Android into ATIV_ANDROID_TOOLCHAIN

TARGET=android ./vitarecomp build PCSB00296
./android/make-apk.py PCSB00296                        # -> android/build/PCSB00296.apk
```

The APK carries the recompiled code, SDL3 and the memory images, but not the game's data (it is gigabytes). Push that
once to the app's external files folder:

```sh
adb install -r android/build/PCSB00296.apk
adb push "$HOME/Library/Application Support/ProjectNativ/Nativ/library/decrypted/PCSB00296/fs/app0" \
  /sdcard/Android/data/org.projectnativ.pcsb00296/files/fs/
```

`make-apk.py` prints the exact paths for the title it built. [android/README.md](android/README.md) has the details,
including the FFmpeg-for-Android build that enables the movies.

## Using it

The launcher (`launcher/build.sh` → `launcher/nativ`) walks through first-run setup, builds your games and starts them.
The in-game menu (`F1` by default, or the pad's guide button) has the display, audio and graphics settings.

Controls follow the Vita's layout; a controller is mapped through SDL. On Android the window is landscape and
fullscreen, and the gamepad works without configuration.

## How it works

A Vita game is ARM code written against Sony's system software, and Nativ handles the two separately.

- **The game's code is recompiled.** A static recompiler translates the game's ARMv7 code, Thumb-2, VFP and NEON
  included, into C once, ahead of time. That C is compiled into a native program.
- **The system underneath is rebuilt.** Sony's libraries the game loads, such as libgxm, libc and FIOS, are recompiled
  with it from your firmware. Below them a runtime stands in for the Vita's kernel and hardware: it runs the game's
  threads, draws its graphics on your GPU and plays its sound.

[How it works](docs/architecture.md) goes into the design, and [the runtime in detail](docs/runtime.md) covers display
timing, audio, the GPU backend and hooks.

## Repository layout

| Folder | What's in it |
|---|---|
| `packs/PCSB00296/` | This port's pack: `pack.toml` (versions, modules, settings) and `shaders.txt` (shader recipes). |
| `recompiler/` | The static recompiler, its runtime header `rt.h`, and seeds for functions the analysis misses. |
| `runtime/` | What a recompiled game runs on: `core/` (kernel and libraries), `gfx/`, `audio/`, `media/`, `input/`, `net/`, `ui/`, `platform/`, `debug/` and `tests/`. |
| `pipeline/` | The `vitarecomp` driver, plus game and licence handling. |
| `android/` | The Android target: manifest and activity templates, `make-apk.py`, and its notes. |
| `decrypt/` | Decrypting firmware updates and game packages, and converting SELF to ELF. |
| `launcher/` | The launcher: your library, first-run setup and settings. |
| `dev/` | Developer tools: replays, diffs, bisects, the remote gate and tests. |
| `third_party/` | Dear ImGui, stb, LibAtrac9, Monocypher and the HE-VAG tables, each with its licence. |
| `docs/` | The documentation listed below. |

## Documentation

- [Player guide](docs/player-guide.md): installing, first run, controls and known issues.
- [How it works](docs/architecture.md): the design, layer by layer.
- [Working on Nativ](docs/developers.md): the pipeline, packs and versions, and working across machines.
- [The runtime in detail](docs/runtime.md): display timing, audio, the GPU backend, controls and hooks.
- [Verification](docs/verification.md): the regression gate.
- [Debugging a game](docs/debugging.md): logs, captures, replays and the renderer's switches.
- [The shader compiler](docs/shader-compiler.md) and [the USSE instruction set](docs/usse-isa.md).
- [Android](android/README.md): the APK target.

## Licence

This fork, like the project it is forked from, is free software under the **GNU General Public License, version 2 or
(at your option) any later version**: see [LICENSE](LICENSE). If you redistribute a build, it is under the same terms.

Third-party code keeps its own licence. [THIRD_PARTY.md](THIRD_PARTY.md) lists each project, its licence and where it
is used. The game and the firmware are not covered by any of this: they remain Koei Tecmo's and Sony's, and you supply
your own.

## AI disclaimer

This project is made with the help of AI tools: much of its code and documentation was written with AI assistance,
under its developer's direction, and tested on the game itself. That applies to upstream's work and to this fork's.

## Supporting upstream

Project Nativ is free, and everything in it is for everyone. If you'd like to support its development, upstream takes
tips on [Ko-fi](https://ko-fi.com/kingtorro).
