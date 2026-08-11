# Magic Carpet 2 HD - Reloaded fork 1.0

A fork of **Magic Carpet 2 HD** that moves the world renderer from the CPU to
the GPU (Direct3D 11) while reproducing the original software renderer pixel
for pixel.

Lineage of this work:

* **Bullfrog Productions** wrote Magic Carpet 2 (1995).
* **Tomas Vesely** reverse engineered it from assembler to C/C++: https://github.com/turican0/remc2
* **thobbsinteractive and contributors** turned that into Magic Carpet 2 HD - resolution support, modern controls, sound, installer, multiplayer: https://github.com/thobbsinteractive/magic-carpet-2-hd
* This fork adds the GPU renderer on top.

None of this would exist without the work above. See [Credits](#credits).

### find us on Discord here: https://discord.gg/GR55HCbJJ4 ###

## New in this fork ##
- **Direct3D 11 world renderer** - terrain, world sprites and sky rasterised on the GPU
- **Exact draw order** between terrain and sprites, so nothing floats in front of geometry it should be hidden behind
- **Exact destination blending** (translucent water, shadows) via rasterizer ordered views
- **Roughly 30x less CPU time per world frame**, e.g. 11.85 ms -> 0.40 ms at 1280x720
- **The exit warp keeps its frame rate** - it used to drop the world onto the software rasteriser, 60 fps to 3.5 at 4K; its blur is an RGB trail behind the palette resolve, tunable via `--warp_strength` / `--warp_decay_ms`
- **View distance up to 4x**, switchable in game
- **Frame rate counter**, switchable in game
- Every stage sits behind its own feature flag; with all of them off you get the unchanged software renderer

Technical documentation, measurements and known limitations:
[docs/GPU-RENDERER.md](docs/GPU-RENDERER.md)

## Current Features ##
- **Support for HD, 2k and even 4k gameplay**
- Modern Controls
- Easy to use **Configurator:**
<img width="200" height="253" alt="image" src="https://github.com/user-attachments/assets/c6507af5-3be5-4a24-806f-9bb5338ece3b" />

## Download the latest Beta Here (now with MSI Installer)! ##
https://github.com/thobbsinteractive/magic-carpet-2-hd/releases/latest

## Install Guide for GOG Edition or from Magic Carpet CD ##
https://github.com/thobbsinteractive/magic-carpet-2-hd/wiki/Windows-Installation-Guide

## Installing this fork ##
**You need your own copy of Magic Carpet 2.** No game data is contained in this
repository and none will ever be - the GOG edition or an original CD is
required.

1. Install Magic Carpet 2 HD first, using the installer and guide linked above.
   That sets up the game data, the configurator and everything else.
2. Build this fork (see [Build](#build)) or take `remc2.exe` from a release of
   this repository.
3. Replace the `remc2.exe` of your Magic Carpet 2 HD installation with it.
4. Enable the GPU renderer in `config.json` under `graphics`:

```json
"gpuPalettePresentation": true,
"gpuWorldGeometry": true,
"gpuSprites": true,
"gpuSky": true
```

`gpuExactBlend` defaults to on. If your GPU has no support for rasterizer
ordered views the renderer says so in the log and falls back automatically, so
the flag is safe to leave alone.

Requirements: Windows with Direct3D 11. `gpuSky` and the exact blend modes need
feature level 11_1 hardware, which in practice means anything from roughly 2013
onwards. Without it the other stages still work.

To go back to the original software renderer at any time, set all `gpu*` flags
to `false`.

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
## Status ##

|Branch|Windows|Linux|
|------|:-----:|:---:|
|master|[![MSBuild CI](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/msbuild.yml/badge.svg?branch=master)](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/msbuild.yml)|[![Linux 64bit CI](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/linux.yml/badge.svg?branch=master)](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/linux.yml)|
|development|[![MSBuild CI](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/msbuild.yml/badge.svg?branch=development)](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/msbuild.yml)|[![Linux 64bit CI](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/linux.yml/badge.svg?branch=development)](https://github.com/thobbsinteractive/magic-carpet-2-hd/actions/workflows/linux.yml)|

#### STATUS: Code now runs and all of MC2 (in both Windows and Linux) seems to be playable. Anyone with the GOG edition can download this repo, extract the Game Assets (from a legal GOG copy of the game) and run it. ####

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
**If you know a bit about game development or want to help out, branch away or email me here: thobbsinteractive@gmail.com**
- The Project is compiled as C++17.
- If you re-name a method include the id from the original method name as this makes it easier to track changes from the generated code.
e.g. `void sub_19CA0_sound_proc5(unsigned __int8 a1)` was renamed to `void ChangeSoundLevel_19CA0(uint8_t option)`
- Please follow the general style of the refactored code. Upper Camel Case (Pascal Case) for Class/Method names. Camel Case for variables. 'm_' for class members. `GameRenderHD.cpp` is a good example of the style.
- Where possible (if writting new code) please use the fixed width types. https://en.cppreference.com/w/cpp/types/integer
- For each commit please use the Semantic Commit Messages: https://gist.github.com/joshbuchea/6f47e86d2510bce28f8e7f42ae84c716
- Be careful with making logic changes to the code and Test, Test, Test! I recommend playing the first level all the way though. Then the first Cave level (4) and I also recomend Level 5 as you have a nice mix of AI to kill and a cutscene at level completion.
- Please build and run the remc2-regression-test project BEFORE making a pull request. This must pass and since it needs the game data cannot be placed in the Github Actions.

# ROADMAP #

## MILESTONE 1 ##
- [x] Get solution runnable from Visual Studio 2019 build, with minimum of setup. Cut down on unnecessary extra files and libraries and use nuget instead.
- [x] Refactor reverse engineered code into seperate classes where possible.

## MILESTONE 2 ##
- [x] Add resolution support
- [ ] Implement Open GL render
- [X] Implement Controller Support
- [X] Implement a (platform independent) Launch menu to adjust settings in config.json before launch

## MILESTONE 3 ##
- [X] Improve sounds and music using updated original scores and directional sounds in game - In Review!
- [X] Implement a wix sharp .msi installation for new .exe to make patching the and running existing game simple and something similar for the Linux versions

## MILESTONE 4 ##
- [X] Get basic LAN/IPv4 multiplayer working again

## MILESTONE 5 ##
- [ ] Get Magic Carpet 1 working using this engine. Ideally with original music and graphics.

## GPU RENDERER (this fork) ##
- [x] Palette resolve and upscaling on the GPU
- [x] Terrain and world polygons on the GPU, one draw call per frame
- [x] World sprites on the GPU, draw order exact against the terrain
- [x] Exact destination blending through rasterizer ordered views
- [x] Sky on the GPU
- [x] View distance up to 4x, switchable in game
- [x] Exit warp blur on the GPU, so the level end keeps its frame rate - an RGB trail behind the palette resolve, frame rate independent and tunable
- [ ] Explosions and particles with real alpha blending instead of the sprite path
- [ ] Minimap markers scaled with the UI
- [ ] Emulate the per scanline DDA to remove the last sub-pixel sampling offset
- [ ] Linux/Vulkan or OpenGL backend alongside the D3D11 one

## LONG TERM GOALS ##
- Add VR support back into the game (yes it was originally supported! This game was waaay ahead of its time)<br />
- Implement online multiplayer match making

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
