# Magic Carpet 2 HD - Reloaded fork 0.8.1

A fork of **Magic Carpet 2 HD** that moves the world renderer from the CPU to
the GPU (Direct3D 11) while reproducing the original software renderer pixel
for pixel.

Lineage of this work:

* **Bullfrog Productions** wrote Magic Carpet 2 (1995).
* **Tomas Vesely** reverse engineered it from assembler to C/C++: https://github.com/turican0/remc2
* **thobbsinteractive and contributors** turned that into Magic Carpet 2 HD - resolution support, modern controls, sound, installer, multiplayer: https://github.com/thobbsinteractive/magic-carpet-2-hd
* This fork adds the GPU renderer on top.

None of this would exist without the work above. See [Credits](#credits).

**How this fork was made:** the code in it was written with
[Claude Code](https://claude.com/claude-code), Anthropic's coding agent. I am
not a programmer - I directed the work, tested every change on screen and
decided what was good enough to keep. Saying so up front seems only fair, both
to the people whose project this builds on and to anyone reading the code. Every
claim in this README is measured, and how to reproduce the measurements is in
[docs/GPU-RENDERER.md](docs/GPU-RENDERER.md).

The Magic Carpet 2 HD community lives on Discord: https://discord.gg/GR55HCbJJ4

## New in this fork ##
- **Direct3D 11 world renderer** - terrain, world sprites and sky rasterised on the GPU
- **Exact draw order** between terrain and sprites, so nothing floats in front of geometry it should be hidden behind
- **Exact destination blending** (translucent water, shadows) via rasterizer ordered views
- **Roughly 30x less CPU time per world frame**, e.g. 11.85 ms -> 0.40 ms at 1280x720
- **The exit warp keeps its frame rate** - it used to drop the world onto the software rasteriser, 60 fps to 3.5 at 4K; its blur is an RGB trail behind the palette resolve, tunable via `--warp_strength` / `--warp_decay_ms`
- **View distance up to 4x**, switchable in game
- **Frame rate counter**, switchable in game
- Every stage sits behind its own feature flag - all on by default, and with all of them off you get the unchanged software renderer

Technical documentation, measurements and known limitations:
[docs/GPU-RENDERER.md](docs/GPU-RENDERER.md)

## What you get from the base project ##

Everything Magic Carpet 2 HD already does stays in place - HD, 2K and 4K
support, modern controls, the improved sound and music, the configurator, the
MSI installer and LAN multiplayer. Those are the upstream project's work, not
this fork's:

* Downloads and installer: https://github.com/thobbsinteractive/magic-carpet-2-hd/releases/latest
* Installation guide (GOG edition or original CD): https://github.com/thobbsinteractive/magic-carpet-2-hd/wiki/Windows-Installation-Guide

## Download ##

**[Download remc2-gpu.exe (Windows x64)](https://github.com/SputnikKaputtnik/magic-carpet-2-hd-reloaded/releases/latest/download/remc2-gpu.exe)** — always the latest release.
[All releases and their notes](https://github.com/SputnikKaputtnik/magic-carpet-2-hd-reloaded/releases).

The executable only, and deliberately named apart from the base project's
`remc2.exe`: put it **next to** that one rather than over it and both stay
playable, because the game finds its data by where the executable sits, not by
what it is called. It is not code signed, so SmartScreen will warn; the release
notes carry the SHA256 to check against.

## Installing this fork ##

**You need your own copy of Magic Carpet 2.** No game data is contained in this
repository and none will ever be - the GOG edition or an original CD is
required.

1. Install Magic Carpet 2 HD first, using the installer and guide linked above.
   That sets up the game data, the configurator and everything else.
2. Download `remc2-gpu.exe` above, or build this fork yourself (see
   [Build](#build)).
3. Put it in the same folder as the `remc2.exe` of that installation and start
   it instead of the original.

That is the whole installation. **The GPU renderer is on by default** - there is
nothing to edit, and nothing is overwritten. To go back, start `remc2.exe`
again.

### Keeping the settings apart ###

Both executables read `config.json`, which means they share one resolution. If
you want different settings for each - a higher resolution for the GPU renderer,
say - copy `config.json` to **`config-gpu.json`** in the same folder and edit
that. `remc2-gpu.exe` prefers it when it is there and leaves `config.json` to
the base project.

Either file can switch individual stages off again under `graphics`:

```json
"gpuPalettePresentation": true,
"gpuWorldGeometry": true,
"gpuSprites": true,
"gpuSky": true,
"gpuExactBlend": true
```

Setting all of them to `false` gives you the unchanged software renderer inside
this executable.

Requirements: Windows with Direct3D 11. `gpuSky` and the exact blend modes want
feature level 11_1 hardware, which in practice means anything from roughly 2013
onwards; without it those stages say so in the log and step aside on their own,
and the rest keeps working.

One thing to know: the Magic Carpet 2 HD configurator and its shortcuts keep
launching the original `remc2.exe`. If you would rather have the GPU renderer
behind those, back up `remc2.exe` and rename `remc2-gpu.exe` into its place.

# Controls #
Controls can be redefined in the Configurator/Config.json file, however here are the defaults:
</br>
Forward = W</br>
Backwards = S</br>
Move Left = A</br>
Move Right = D</br>
Open Spell Menu = LCtrl / Mouse 4</br>
Open Map = Tab / Middle Mouse</br>
</br>
Graphics options, all with Shift held:</br>
Shift+F1 = Reflections, Shift+F2 = Sky, Shift+F3 = Shadows, Shift+F4 = Light sources</br>
Shift+F11 = Cycle view distance (1x to 4x)</br>
Shift+F12 = Frame rate counter</br>

# Community links #
- **Magic Balls:** A project that uses the same engine but renders the image via the Godot engine: https://github.com/turican0/MagicBalls<br>
- Blog from the very beginning of this project's development: https://github.com/turican0/remc2/wiki<br>
- Dosbox version for data comparison: https://github.com/turican0/dosbox-x-remc2<br>
- FAQ: https://github.com/thobbsinteractive/magic-carpet-2-hd/wiki/FAQ
- Moburma has been tirelessly working to document cut levels, level data structures and missing graphics at: https://tcrf.net/Magic_Carpet_2:_The_Netherworlds

# Build #

The GPU renderer is Windows and Direct3D 11 only. The rest of the code builds on
Windows and Linux exactly as it does upstream - with every `gpu*` flag off, this
fork behaves like the base project.

## Steps: to build and run this code

### Windows:
- 1: Install the latest version of Visual Studio 2022 Community. Ensure you install [vcpkg](https://devblogs.microsoft.com/cppblog/vcpkg-is-now-included-with-visual-studio/)
- 2: Pull the development branch
- 3: Open "remc2.sln", you can build either x64 or 32 bit versions
- 4: Build the code
- 5: Purchase a copy of Magic Carpet 2 from GOG here: https://www.gog.com/game/magic_carpet_2_the_netherworlds
- 6: Install the Game. Copy the "NETHERW" directory to "remc2\Debug\" Folder
- 7: Copy the "Extract" folder to your Game Directory, run extract-GOG-CD.bat. The CD Data will now be copied to a directory called "CD_Files" in the "Extract" directory
- 8: Move "CD_Files" directory into the "remc2\Debug" Folder
- 9: Run

### Linux:

#### Building on Linux

There are two ways to build the Linux binary.
- Building a native binary
  1. Pull the development branch using GitHub (this is much easier if you install Visual Studio Code and install C++ Extension, cmake, cmake tools). When pulling the branch either do a recursive clone of the repository or ensure that after the pull you run: `git submodule init` and `git submodule update`
  2. Once pulled, within the `magic-carpet-2-hd/` directory pull the `findfirst` repo.
  3. Make sure that you have `CMake`, `make` and a recent `GCC` installed
     - To install them on Debian/Pi OS: `sudo apt install -y cmake`  
  4. Make sure that you have the following dependencies as development packages (the exact names depend on your distro)
  - SDL2
  - SDL2_mixer
  - SDL2_image
  - SDL2_ttf
  - spdlog
  - nlohmann-json3-dev
  - libwxgtk3.2-dev
    - To install them on Debian/Pi OS: `sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev libspdlog-dev nlohmann-json3-dev libwxgtk3.2-dev` 
  4. Build the code
  ```bash
  export BUILDTYPE=Debug # or Release
  mkdir -p build/${BUILDTYPE}
  cd build/${BUILDTYPE}
  cmake -DCMAKE_BUILD_TYPE=${BUILDTYPE} -DCMAKE_INSTALL_PREFIX=./inst [SOURCE_DIR]
  make
  make install
  ```
  5. Magic Carpet 2 is now built. you can find it in `build/Debug/inst/bin`
     - You can also run the code with sanitizers (leak, address, undefined behaviour, pointers) by passing `-DUSE_SANITIZERS=True` to CMake

- Building a [flatpak](https://flatpak.org/)
  1. Pull the development branch
  2. Build the flatpak
  ```bash
  cd flatpak
  ./build.sh
  ```
  3. Run the `remc2` flatpak via
  ```bash
  flatpak run com.github.thobbsinteractive.magic-carpet-2-hd
  ```

- Running clang-tidy for static code analysis
  1. Run CMake with the flag `CMAKE_EXPORT_COMPILE_COMMANDS` for exporting the build commands like this
  ```
  cmake -GNinja -DUSE_SANITIZERS=True -DCMAKE_BUILD_TYPE=${BUILDTYPE} -DCMAKE_INSTALL_PREFIX=./inst -DUNIT_TESTS=True  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../..
  ```
  2. Run clang-tidy
  ```
  run-clang-tidy -p . -j 4 -export-fixes clang-tidy-fixes.yaml
  ```

### Providing the original game assets to `remc2` and running the game

In order to run the game you need to own a copy of Magic Carpet 2. We provide a script to extract the assets from the GOG version. The following steps extract the required files from the original.
  1. Purchase a copy of Magic Carpet 2 from GOG here: https://www.gog.com/game/magic_carpet_2_the_netherworlds
  2. Download the Windows "Offline Backup Game Installer"
  - Open GOGGalaxy
  - Install the game
  3. In order to retrieve the original game's assets run the following script located in the root of the repository:
  ```
  bash check_install.sh -s [directory where GOG installed MC2] -d [destination directory]
  # for example :
  bash check_install.sh -s "${HOME}/.wine/drive_c/games/Magic Carpet 2/" -d "build/${BUILDTYPE}/inst/bin/NETHERW"
  # or :
  bash check_install.sh -s "/mnt/c/Program Files (x86)/GOG Galaxy/Games/Magic Carpet 2/" -d "build/${BUILDTYPE}/inst/bin/NETHRW"
  ```
  Please note that if you have used any other method to get the assets, at least run a check to make sure that remc2 has access to every file it needs:
  ```
  bash check_install.sh -d [destination directory]
  # for example:
  bash check_install.sh -d "build/${BUILDTYPE}/inst/bin/"
   [ ok ] CD_Files directory
   [ ok ] GAME directory
  ```
  this script will fix the permissions, make everything uppercase, check all file hashes and complain if any file is missing.
  it's also recommended that before the first run you remove the file GAME/NETHERW/CONFIG.DAT if it exists.
  4. NOTE: The game will search in the following locations (and in this particular order) for the game assets. For the flatpak only the first two locations can be used.
     1. `$XDG_DATA_HOME/remc2/`
     2. `$HOME/.local/share/remc2`
     3. next to the `remc2` binary
  5. Run the `remc2` executable in install directory
  ```
  cd magic-carpet-2-hd/build/${BUILDTYPE}/inst/bin
  ./remc2
  ```

#### Configuring `remc2`

Some settings can be configured via the file `config.json`. An example for this file can be found in the root directory of the `remc2` repository.
The game will search for this file in the following locations and this particular order. For the flatpak only the first two locations can be used.
1. `$XDG_CONFIG_HOME/remc2`
2. `$HOME/.config/remc2`
3. next to the `remc2` binary

## Development Guide ##

Contributions to this fork are welcome - open an issue or a pull request here.
For the base project, contact the Magic Carpet 2 HD maintainers at
[their repository](https://github.com/thobbsinteractive/magic-carpet-2-hd)
rather than here.

The conventions below are the base project's and this fork follows them:

- The Project is compiled as C++17.
- If you re-name a method include the id from the original method name as this makes it easier to track changes from the generated code.
e.g. `void sub_19CA0_sound_proc5(unsigned __int8 a1)` was renamed to `void ChangeSoundLevel_19CA0(uint8_t option)`
- Please follow the general style of the refactored code. Upper Camel Case (Pascal Case) for Class/Method names. Camel Case for variables. 'm_' for class members. `GameRenderHD.cpp` is a good example of the style.
- Where possible (if writting new code) please use the fixed width types. https://en.cppreference.com/w/cpp/types/integer
- For each commit please use the Semantic Commit Messages: https://gist.github.com/joshbuchea/6f47e86d2510bce28f8e7f42ae84c716
- Be careful with making logic changes to the code and Test, Test, Test! I recommend playing the first level all the way though. Then the first Cave level (4) and I also recomend Level 5 as you have a nice mix of AI to kill and a cutscene at level completion.
- Please build and run the remc2-regression-test project BEFORE making a pull request. This must pass and since it needs the game data cannot be placed in the Github Actions.

# Roadmap of this fork #

Done:

- [x] Palette resolve and upscaling on the GPU
- [x] Terrain and world polygons on the GPU, one draw call per frame
- [x] World sprites on the GPU, draw order exact against the terrain
- [x] Exact destination blending through rasterizer ordered views
- [x] Sky on the GPU
- [x] View distance up to 4x, switchable in game
- [x] Exit warp blur on the GPU, so the level end keeps its frame rate - an RGB trail behind the palette resolve, frame rate independent and tunable

Next:

- [ ] Explosions and particles with real alpha blending instead of the sprite path
- [ ] Minimap markers scaled with the UI
- [ ] Emulate the per scanline DDA to remove the last sub-pixel sampling offset
- [ ] Linux/Vulkan or OpenGL backend alongside the D3D11 one

Being investigated: whether the engine's projection can carry a real 3D camera
with a per-eye asymmetric frustum - the one question that decides whether a
native VR port is possible. The renderer has no depth buffer and its correctness
rests on draw order, which is what makes this interesting.

For the roadmap of the upstream project this fork builds on, see
[thobbsinteractive/magic-carpet-2-hd](https://github.com/thobbsinteractive/magic-carpet-2-hd).

# Credits #

This fork stands entirely on other people's work.

- **Bullfrog Productions** - Magic Carpet 2: The Netherworlds (1995), the game itself.
- **Tomas Vesely** ([turican0](https://github.com/turican0)) - reverse engineered the
  original from assembler into C/C++ in [remc2](https://github.com/turican0/remc2).
  Without that there is nothing to build on. His decompiled rasteriser is also
  what the GPU renderer in this fork is verified against, pixel by pixel.
- **thobbsinteractive** and the contributors to
  [magic-carpet-2-hd](https://github.com/thobbsinteractive/magic-carpet-2-hd) -
  resolution support, modern controls, controller support, sound and music work,
  the configurator, the installer and LAN multiplayer. This fork branches from
  their work and follows their code style and conventions.
- **Moburma** - documentation of cut levels, level data structures and missing
  graphics at [TCRF](https://tcrf.net/Magic_Carpet_2:_The_Netherworlds).
- The **remc2 and Magic Carpet 2 HD community** on
  [Discord](https://discord.gg/GR55HCbJJ4).

Bug reports about the GPU renderer belong in this fork, not upstream.

# License #
## Original Source Code is Copyright 1995 Bullfrog Productions ##

## Additonal Code is Licensed under the following MIT Licence: ##
Copyright 2026 Magic Carpet 2 HD

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

