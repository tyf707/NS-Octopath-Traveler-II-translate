# Switch2Android

**An open-source Android compatibility layer and build tool for Nintendo Switch games.** Turn your own NSP files, updates and DLC into an Android app that runs offline. The original ARM64 game code executes natively, while the host provides system services and translates NVN graphics calls to Vulkan. Game shaders are translated to SPIR-V during the build.

Android 13+ · ARM64 · Vulkan · MIT · **Bring your own game files and keys**

This repository contains source code, tools and compatibility profiles. It does not contain or distribute game programs, assets, shaders or keys. Use files from games you legally own; generated packages contain game content and must not be redistributed.

## Supported games

| Game | Version | Title ID |
|---|---|---|
| OCTOPATH TRAVELER II | 1.1.1 (update 131072) | `0100a3501946e000` |
| TRIANGLE STRATEGY | 1.1.1 (update 262144) | `0100cc80140f8000` |

Compatibility depends on the game and version. Builds require a matching profile in [profiles/](profiles/).

The input format is NSP with common tickets. XCI, NSZ, personalized tickets and DataPatch packages are not supported.

## Requirements

**Android device:** Android 13 or later, an ARM64 processor and Vulkan support. At least 8 GB of RAM is recommended. Allow 14 GB of free storage to install OCTOPATH TRAVELER II, or 20 GB for TRIANGLE STRATEGY. No root access is needed.

**Build tools:**

- Python 3.11+, Git, Make and a C/C++ compiler.
- JDK 21 and CMake.
- Android SDK platform 36, NDK 28.2.13676358, SDK CMake 3.22.1 and platform-tools.
- .NET SDK 10, Rust/Cargo and SPIRV-Tools (`spirv-val`).

The commands below use macOS and Homebrew. Install the Android SDK and its command-line tools first, then set `ANDROID_HOME` to your SDK directory:

```sh
brew install python@3.13 openjdk@21 cmake rust dotnet spirv-tools
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager 'platforms;android-36' 'ndk;28.2.13676358' 'cmake;3.22.1' 'platform-tools'
sdkmanager --licenses
```

Check that `dotnet --list-sdks` includes 10.x. The first build needs internet access to fetch development dependencies; the resulting app runs offline. Allow about 60 GB of build storage for OCTOPATH TRAVELER II.

## Build your app

Prefer to let an agent handle setup? Give it the paths to your NSP directory and key file, then ask: "Read [INSTALL.txt](INSTALL.txt) and follow it to build the APK and install it on my Android device." The guide covers dependencies, packaging, installation and launch checks.

Run the following commands from the repository root. Keep game files, keys, build work and generated packages outside the source directory.

### 1. Prepare your game files

Put the base NSP and any update or DLC NSPs in one directory. Files are identified by their content metadata, so their names do not matter.

```text
~/switch2android-input/
  base.nsp
  update.nsp
  dlc.nsp
~/keys/prod.keys
```

For either supported game, include its 1.1.1 update. DLC is optional.

### 2. Set up the tools

```sh
S2A_WORK="$HOME/switch2android-work"
S2A_OUT="$HOME/switch2android-output"
./scripts/bootstrap-tools.sh "$S2A_WORK"
.venv/bin/python switch2android.py doctor
```

The setup script builds pinned versions of hactool and repak and installs Python dependencies into `.venv`. Resolve any `MISSING` entries reported by `doctor` before continuing.

### 3. Build

```sh
.venv/bin/python switch2android.py build "$HOME/switch2android-input" \
  --keys "$HOME/keys/prod.keys" \
  --work "$S2A_WORK" --output "$S2A_OUT"
```

The tool selects the latest update and matching DLC, extracts and verifies the game files, translates shaders and builds a signed Release APK. The first build takes longer; later builds reuse the work directory.

| Option | Purpose |
|---|---|
| `--title-id 0100a3501946e000` | Select a game when the input directory contains more than one |
| `--version 131072` | Select a specific update |
| `--no-dlc` | Exclude DLC |
| `--embed never` | Keep game resources in a separate `.s2a` file |

The output is written to a game-specific directory:

```text
switch2android-output/0100a3501946e000-v131072/
  game.apk
  game.s2a
  package.json
  INSTALL.txt
```

Resource bundles up to 1 GiB are embedded in the APK by default. Larger games, including both supported titles, use an APK plus a `.s2a` resource file. Embedded builds do not produce a separate `.s2a` file.

Existing output directories are not overwritten. Use a new `--output` directory when rebuilding; keep the same `--work` directory to reuse cached files.

## Install and play

Copy the generated files to your Android device, install `game.apk` and open it. If prompted, select the `game.s2a` file from the same build. Resources are verified and imported on first launch.

Alternatively, enable USB debugging, connect the device and install through ADB:

```sh
adb devices
.venv/bin/python switch2android.py install \
  "$S2A_OUT/0100a3501946e000-v131072" --serial YOUR_DEVICE_SERIAL
```

The command installs the APK, transfers resources if needed and opens the app. Once a manual import has completed, the copied `.s2a` file can be deleted from Downloads to reclaim space.

**Updating:** install the new APK over the existing app with the same signing key. Do not uninstall first: Android removes app saves on uninstall. Each game has its own application ID and saves.

Local builds use `~/.android/debug.keystore` for signing; the app itself is an optimized, non-debuggable Release build. Back up this key and retain it when moving to another computer. Already imported resources are reused when unchanged.

## Tools and development

Inspect NSP metadata or verify a generated resource bundle:

```sh
.venv/bin/python switch2android.py inspect "$HOME/switch2android-input" \
  --keys "$HOME/keys/prod.keys" --hactool "$S2A_WORK/tools/hactool/hactool"
.venv/bin/python switch2android.py verify \
  "$S2A_OUT/0100a3501946e000-v131072/game.s2a"
```

The packaging tools are in `s2a/`, the Android app in `app/`, the native runtime and Vulkan backend in `native/`, and the offline shader tools in `tools/`.

### Shader adapters

Shaders compiled for Horizon are recovered from the retail data and translated to SPIR-V ahead of time. Which adapter runs is chosen by the profile's `resource_adapter`:

| Engine | Source of the NVN binaries | Adapter |
|---|---|---|
| Unreal Engine | `RomFS/**/*.pak` plus `GlobalShaderCache-SF_GLSL_SWITCH.bin` | `tools/extract_shaders.py` |
| Unity | `Shader.compressedBlob` inside UnityFS bundles | `tools/extract_unity_shaders.py` |

Unity keeps each platform's compiled programs in a compressed sub-program blob. On Switch those programs are NVN shader binaries framed as `[u32 control_size][u32 code_size][control][code]` — the sizes precede the control block, whereas the Unreal framing has the code first. The extractor walks the bundles, decompresses the blob for the Switch platform, carves every well-formed binary and writes `{key}.control/.code/.asm` for the offline translator. As with Unreal data, the key is the FNV-1a hash of the control block, so it matches what the runtime computes at draw time. Unity control blocks also carry their assembly text, but retail builds strip it; an empty `.asm` means the texture and image slot tables cannot be inferred, so the translator runs with empty bindings.

A profile whose `resource_adapter` is `generic` uses neither adapter; that is the correct setting for engines whose shaders cannot be recovered this way.

To run the checks without game files:

```sh
source .venv/bin/activate
./scripts/check.sh
./gradlew :app:assembleRelease :app:lintRelease
dotnet run --project tools/shader_tests/ShaderRegressionTests.csproj -c Release
```

When reporting an issue, include the game version, device model, Android version and steps to reproduce it. Do not attach game files or keys.

## Acknowledgments

This project builds on the work of open-source authors and maintainers:

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile): native host architecture, runtime services, NVN frontend and offline shader tools.
- [D2R Android](https://github.com/dodola/d2r-mobile): Android/Vulkan compatibility and input design.
- [nxnative](https://github.com/seedds/nxnative): game profiles and the local packaging workflow.
- Ryujinx / Ryubing: the standalone shader translator and SPIR-V generator.
- deko3d, nvn.h, bcdec, LZ4, nlohmann JSON and SPIR-V Headers: graphics interfaces, texture handling and supporting libraries.
- hactool, repak, SPIRV-Tools, .NET and Gradle: extraction, validation and build tools.

## License

[MIT](LICENSE). Third-party licenses and attribution are collected in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and included in the app.
