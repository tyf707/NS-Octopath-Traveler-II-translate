# NS-translate-Android

**Android translation layer and build tool for Nintendo Switch games.**

This repository turns your own NSP files, updates and DLC into an Android app that runs
offline. The original ARM64 game code executes natively, the host implements Horizon
system services, and NVN graphics calls are translated to Vulkan. Game shaders are
recovered from the retail data during the build and translated to SPIR-V ahead of time.

Android 13+ · ARM64 · Vulkan · MIT · **Bring your own game files and keys**

This repository contains source code, tools and compatibility profiles. It does not
contain or distribute game programs, assets, shaders or keys. Use files from games you
legally own; generated packages contain game content and must not be redistributed.

## Supported games

| Game | Engine | Version | Title ID | Status |
|---|---|---|---|---|
| OCTOPATH TRAVELER II | Unreal Engine 4 | 1.1.1 (update 131072) | `0100a3501946e000` | playable |
| TRIANGLE STRATEGY | Halley (shader container) | 1.1.1 (update 262144) | `0100cc80140f8000` | playable |
| Pokémon Shining Pearl | Unity 2019.4 | 1.3.0 (update 393216) | `010018e011d92000` | experimental |

Each title needs a profile in [profiles/](profiles/). The profile declares the shader
adapter (`resource_adapter`), the supported main-module build IDs and the per-title
patches (`patches`). Unknown build IDs are refused instead of silently assumed
compatible.

Input format is NSP with common tickets. XCI, NSZ, personalized tickets and DataPatch
packages are not supported.

## What this branch adds

The core runtime (loader, ARM64 execution, NVN→Vulkan translation, system services) is the
Switch2Android core; the work below is layered on top of it.

**Unity support**
- Shader recovery from UnityFS bundles (`Shader.compressedBlob`) into `{key}.control/.code`
  for the offline translator: `tools/extract_unity_shaders.py`.
- Unity shader identity handling (`tools/unity_shader_identity.py`) and the
  `resource_adapter: "unity"` profile path.

**Engine adapter layer** (`app/.../EngineHooks.java`, `native/game_profile.*`)
- Engines are identified from the profile (`resource_adapter` / `engine.kind`) and fall
  back to content inspection; `debug.ot2.engine=unity|ue4|generic` overrides it for
  testing.
- Engine-level policy lives in one place: default present interval, whether a frame-rate
  patch exists (UE4 `t.MaxFPS` CVar), save-directory convention.
- Per-title patches are declarative (`game-build.json` → `"patches": [...]`) instead of
  hard-coded title checks: `settime-host`, `fade-instant`.
- Boot report: `SERVICE_COVERAGE sdk_exports=… hooked=… native=…`, and
  `debug.ot2.servicecheck=1` lists SDK symbols the host does not hook.

**Graphics fast paths**
- Full-image clears now use `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage`;
  depth-only clears on `DEPTH_STENCIL` targets use `vkCmdClearAttachments` so stencil is
  preserved. Measured on Shining Pearl: **GPU 26.2 → 17.4 ms per frame** in a town scene.
- Geometry cache budget raised from 64 to 128 MiB.
- Present-interval override (`debug.ot2.interval`, default `1` = up to 60 Hz) and display
  aspect modes (`debug.ot2.aspect` = `fit` / `crop` / `stretch`).

**Runtime robustness**
- Presentation is suspended unless the activity is resumed and focused, which fixes a
  host-thread crash when the surface is destroyed after backgrounding.

**On-device settings** (no rebuild, no ADB)
- Press the on-screen **“−” and “+”** buttons together (two fingers, or within 0.7 s) to
  open a panel: display fill mode, frame-rate ceiling and virtual-button visibility.
  Choices apply immediately and persist.

## Requirements

**Android device:** Android 13 or later, ARM64 and Vulkan. 8 GB RAM recommended.
Free storage to install: about 14 GB for OCTOPATH TRAVELER II, 20 GB for TRIANGLE
STRATEGY, 7 GB for Pokémon Shining Pearl (content plus a translated shader cache).

**Build tools:**

- Python 3.11+, Git, Make and a C/C++ compiler.
- JDK 21 and CMake.
- Android SDK platform 36, NDK 28.2.13676358, SDK CMake 3.22.1 and platform-tools.
- .NET SDK 10, Rust/Cargo and SPIRV-Tools (`spirv-val`).

The commands below use macOS and Homebrew. Install the Android SDK and its command-line
tools first, then set `ANDROID_HOME` to your SDK directory:

```sh
brew install python@3.13 openjdk@21 cmake rust dotnet spirv-tools
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager 'platforms;android-36' 'ndk;28.2.13676358' 'cmake;3.22.1' 'platform-tools'
sdkmanager --licenses
```

Check that `dotnet --list-sdks` includes 10.x. The first build needs internet access to
fetch development dependencies; the resulting app runs offline. Allow about 60 GB of build
storage for OCTOPATH TRAVELER II (Shining Pearl needs about 12 GB).

## Build your app

Run the following commands from the repository root. Keep game files, keys, build work and
generated packages outside the source directory.

### 1. Prepare your game files

Put the base NSP and any update or DLC NSPs in one directory. Files are identified by their
content metadata, so their names do not matter.

```text
~/ns-input/
  base.nsp
  update.nsp
  dlc.nsp
~/keys/prod.keys
```

Include the update version listed in the profile for the game you are building. DLC is
optional.

### 2. Set up the tools

```sh
S2A_WORK="$HOME/ns-work"
S2A_OUT="$HOME/ns-output"
./scripts/bootstrap-tools.sh "$S2A_WORK"
.venv/bin/python switch2android.py doctor
```

(`switch2android.py` is the packager entry point; the name is kept from the base project.)
The setup script builds pinned versions of hactool and repak and installs Python
dependencies into `.venv`. Resolve any `MISSING` entries reported by `doctor` before
continuing.

### 3. Build

```sh
.venv/bin/python switch2android.py build "$HOME/ns-input" \
  --keys "$HOME/keys/prod.keys" \
  --work "$S2A_WORK" --output "$S2A_OUT"
```

The tool selects the latest update and matching DLC, extracts and verifies the game files,
translates shaders and builds a signed Release APK. The first build takes longer; later
builds reuse the work directory.

| Option | Purpose |
|---|---|
| `--title-id 010018e011d92000` | Select a game when the input directory contains more than one |
| `--version 393216` | Select a specific update |
| `--no-dlc` | Exclude DLC |
| `--shader-cache DIR` | Reuse an already validated offline shader set |
| `--embed never` | Keep game resources in a separate `.s2a` file |

The output is written to a game-specific directory:

```text
ns-output/010018e011d92000-v393216/
  game.apk
  game.s2a
  package.json
  INSTALL.txt
```

Resource bundles up to 1 GiB are embedded in the APK by default. Larger games use an APK
plus a `.s2a` resource file. Existing output directories are not overwritten; use a new
`--output` directory when rebuilding and keep the same `--work` directory to reuse cached
files.

## Install and play

Copy the generated files to your Android device, install `game.apk` and open it. If
prompted, select the `game.s2a` file from the same build. Resources are verified and
imported on first launch.

Alternatively, with USB debugging enabled:

```sh
adb devices
.venv/bin/python switch2android.py install \
  "$S2A_OUT/010018e011d92000-v393216" --serial YOUR_DEVICE_SERIAL
```

The command installs the APK, transfers resources if needed and opens the app.

**Storage:** only one imported content set is kept. Importing a newer bundle deletes the
previous one and the temporary `.s2a`, so no manual cleanup is needed.

**Updating:** install the new APK over the existing app with the same signing key. Do not
uninstall first: Android removes app saves on uninstall. Each game has its own application
ID and saves.

Local builds use `~/.android/debug.keystore` for signing; the app itself is an optimized,
non-debuggable Release build. Back up this key and keep it for future updates.

## Tools and development

Inspect NSP metadata or verify a generated resource bundle:

```sh
.venv/bin/python switch2android.py inspect "$HOME/ns-input" \
  --keys "$HOME/keys/prod.keys" --hactool "$S2A_WORK/tools/hactool/hactool"
.venv/bin/python switch2android.py verify "$S2A_OUT/010018e011d92000-v393216/game.s2a"
```

The packaging tools are in `s2a/`, the Android app in `app/`, the native runtime and
Vulkan backend in `native/`, and the offline shader tools in `tools/`.

### Shader adapters

Which adapter runs is chosen by the profile's `resource_adapter`:

| Engine | Source of the NVN binaries | Adapter |
|---|---|---|
| Unreal Engine | `RomFS/**/*.pak` plus `GlobalShaderCache-SF_GLSL_SWITCH.bin` | `tools/extract_shaders.py` |
| Unity | `Shader.compressedBlob` inside UnityFS bundles | `tools/extract_unity_shaders.py` |
| Halley | `shaders.dat` (`HALLEYPKG`) | handled by the offline tools |

Unity keeps each platform's compiled programs in a compressed sub-program blob. On Switch
those programs are NVN shader binaries framed as
`[u32 control_size][u32 code_size][control][code]`; the extractor walks the bundles,
decompresses the Switch blob, carves every well-formed binary and writes
`{key}.control/.code/.asm`. The key is the FNV-1a hash of the control block, so it matches
what the runtime computes at draw time. Retail builds strip the assembly text, so an empty
`.asm` means the texture and image slot tables cannot be inferred and the translator runs
with empty bindings.

Prefer `--shader-cache DIR` when only metadata changed: it reuses a validated shader set
instead of re-walking a multi-gigabyte RomFS.

### Runtime switches

```sh
adb shell setprop debug.ot2.aspect stretch     # fit | crop | stretch (default)
adb shell setprop debug.ot2.interval 1         # ≤60 Hz (default); -1 = follow the game (30 Hz)
adb shell setprop debug.ot2.engine unity       # force engine adapter: unity | ue4 | generic
adb shell setprop debug.ot2.servicecheck 1     # list SDK symbols the host does not hook
adb shell setprop debug.ot2.fixtime 0          # disable the settime-host patch
```

Settings changed in the in-app panel are stored per app and take precedence over the
built-in defaults.

### Checks without game files

```sh
source .venv/bin/activate
./scripts/check.sh
./gradlew :app:assembleRelease :app:lintRelease
dotnet run --project tools/shader_tests/ShaderRegressionTests.csproj -c Release
```

When reporting an issue, include the game version, device model, Android version and steps
to reproduce it. Do not attach game files or keys.

## Known limitations (Pokémon Shining Pearl)

- Indoor scenes are the heaviest: 27–35 fps (GPU ≈ 23 ms/frame). Light scenes reach
  58–60 fps and towns 40–47 fps.
- Scene transitions are frame-bound. Warm transitions take about 5.8 s; the first load of
  an area or a save can take 15–27 s.
- The 16:9 source is stretched to fill the screen by default. `fit` restores letterboxing;
  `crop` fills the screen but trims the top and bottom.

## Credits

Built on the [Switch2Android](https://github.com/n06ody/switch2android) core by n06ody
(ARM64 execution, NVN→Vulkan translation, Horizon services, packaging workflow), which in
turn builds on the work of open-source authors and maintainers:

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile): native host architecture, runtime services, NVN frontend and offline shader tools.
- [D2R Android](https://github.com/dodola/d2r-mobile): Android/Vulkan compatibility and input design.
- [nxnative](https://github.com/seedds/nxnative): game profiles and the local packaging workflow.
- Ryujinx / Ryubing: the standalone shader translator and SPIR-V generator.
- deko3d, nvn.h, bcdec, LZ4, nlohmann JSON and SPIR-V Headers: graphics interfaces, texture handling and supporting libraries.
- hactool, repak, SPIRV-Tools, .NET and Gradle: extraction, validation and build tools.

## License

[MIT](LICENSE). Third-party licenses and attribution are collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and included in the app.
