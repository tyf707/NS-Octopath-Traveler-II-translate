# Pearl Android port — 2026-10-20 progress

Goal: run Pokémon Shining Pearl natively on the connected Xiaomi 17 Ultra, without a Switch emulator. **Not achieved yet:** the real language menu is visually verified and virtual A advances its confirmation dialog; gameplay and save/reload remain unverified.

Input: title `010018e011d92000`, game 1.3.0 / version `393216`, no DLC, main build ID `38f59cbda2eb9c44b72f94c4d25935a2…`. Source baseline is `6c0f1bd6ec69d92bdcfb19a4d119d4b578d2039b` plus the dirty working tree. Build host: macOS; target: Android 17, device `4c5f417d` / model `2512BPNDAC` (Xiaomi 17 Ultra). APK version code `393217` is a package version, not the game content version.

## Verified milestones

- Corrected `sm:` and `fsp-srv` HIPC/CMIF responses and routed `nn::fs::IsMounted` to the host mount table. The game reads Unity metadata rather than stopping on its ROM mount.
- Replaced the provisional `SVC 0x32` with synchronized pause/context/resume handling. `SVC 0x33` now returns a captured libnx-layout `ThreadContext`; the optimized SVC gate captures the complete guest register/FP state. The game passed both calls repeatedly on-device.
- Initialized NVN device, queue, guest-backed buffer objects, texture/sampler pools, and a three-texture 1920×1080 window swapchain. Added debug-label APIs as metadata-only operations and a checked default point-size path. Color logic-op and multisample fields now connect to the existing render-state representation.
- Adapted the SDK account push/pop/list operations to the same local user exposed by the existing account host services. This removed a null Horizon account-service dereference.
- Added the plural handheld `GetNpadStates` host entry point after tracing the exact PLT import from the game. The prior HID break (`0x58cca`) no longer occurs.
- The game now opens `rom:/Data/level0`, `sharedassets0.assets`, and `Audio/GeneratedSoundBanks/Switch/Init.bnk`; Vulkan GPU images are created and audio output starts.
- Fixed Unity shader identity: runtime control bytes `0x7d0..0x7d7` differ even for identical machine code. Verified aliases include all other control bytes AND the complete code, and require identical SPIR-V/reflection. Existing Unreal cache identity is unchanged.
- Fixed extraction of extensionless Unity serialized resources (`unity default resources` and `unity_builtin_extra`). Full extraction now has 14,411 shaders from 12 files; all translate and pass `spirv-val`, yielding 3,156 verified aliases. Artifacts are under `work/pearl-shader-recovery-20261020/{full-raw,full-jobs,full-set}`.
- Active device cache ID: `8805ca1fc5ed5677c1975f73770169206f394ecdc567190ee11381703cc64ec0`. The cache is selected under the private per-game `shaders/active` file; the original game package is unchanged. The APK alone does not install this updated cache on a fresh device.
- Implemented quad-to-triangle index expansion for indexed/non-indexed NVN draws, preserving indices, base vertex and instance offsets. Host and Android standalone index tests pass.
- Fixed the later `SVC 0x32` race: a pause requested while the target is outside an SVC now waits for its next real context snapshot. The delayed-pause device contract test passes.
- Device log confirms `GAME_FRAME_PRESENTED frame=10 draw=3086` and, after the pause fix, `PRESENT_RATE frames=60 seconds=1.997 fps=30.04`. These are submission/presentation evidence, not a visual-correctness or playable-state claim.

## Current blocker

BCAT initialization now passes on-device using a mounted empty offline cache, NotFound for files/directories, zero enumeration, and explicit InternetRequestDenied for synchronization. Its host-service contract test passes. `nn::oe::SetExpectedVolumeBalance` is handled as a Horizon application/system-library mixing policy hint; the Android host has only the application's own audio stream, whose real volume remains handled by the audio backend.

The real Simplified Chinese language menu is visible (`after-volume.png`), and a 250ms virtual A press opens the language confirmation dialog (`language-held.png`). Confirming it loads further scene resources. The missing `nvnCommandBufferDownsample` entry now delegates to the existing texture-view transfer path, whose backend performs actual Vulkan multisample resolve with format/dimension checks. Its frontend recorder regression passes.

The missing stage-5 shader `ac232494e14d980f` was recovered byte-for-byte from `ComputeShader` kernels in `Data/Resources/unity default resources`. The extractor previously inspected only ordinary `Shader` objects; flat and variant compute-kernel layouts now have a regression test. A complete fresh extraction/translation under `work/pearl-shader-recovery-20261020/with-compute-*` has 14,430 shaders (19 added compute kernels), all translated and validated, with 3,175 aliases and 28,861 verified artifacts. New installed/active device cache ID: `b3fe242d6b2ac604684c8d7bc1869a14154330c70a7fc7181475fb81ea393a89`. Previous cache directories are retained.

With that cache the game reaches the opening dialogue and renders the real 3D Munchlax/professor scene (`intro-next.png`), then the eight-character selection menu (`intro-model.png`). `compute-scene.log` shows continued presentation near 30 FPS. Initial black opening dialogue does not persist after advancing. After selecting the default test character the professor scene remains, without a name dialog; further A presses have not advanced it. `name-entry.log` shows the process continues rendering at 30 FPS without STOP. The main module imports both ShowKeyboard and InlineKeyboard APIs, but only ShowKeyboard has a host adapter. Inline-keyboard involvement is a hypothesis, not yet proven by a call trace. Next: trace Initialize/Calc/Appear and callback readiness before implementing the appropriate keyboard adapter. Field traversal and save/reload remain untested.

Evidence directory: `/private/tmp/pearl-shaders.qVCCRI/`. `after-downsample-confirm.log` contains the latest missing compute-shader stop; `after-volume.png` and `language-held.png` establish real game menu rendering and input response. Screenshots showing other foreground apps after guest exit are not game evidence. Subsequent input and screenshot commands check the foreground package first.

## Verification

- `:app:assembleRelease` succeeds with the existing Pearl asset/build parameters; latest APK installed and run on device `4c5f417d`.
- `.venv/bin/python -m unittest discover -s tests -v`: 7 passing.
- Android standalone thread service contract test: passing on the device.
- Python tool regressions: 13 passing, including compute-kernel extraction, Unity source detection and alias collision/semantic checks.
- Full shader set: 14,411 translated/validated programs; 28,823 manifest/artifact files verified by the packaging checker.
- Host services contract with offline BCAT cases: passing.
- Quad indices contract: passing on host and Android.
- NVN frontend recorder contract including whole-texture Downsample: passing on host; opening animation on-device passes its former missing-entry stop.
- `git diff --check`: clean.

## Iteration review

Overall 3.8/5: accuracy 4 (real screenshots and passing tests; naming cause still unconfirmed), completeness 3 (field gameplay/save-reload not reached), clarity 4 (stage and evidence separated; legacy cache description retained for history), actionability 4 (updated APK/cache installed; standalone bundle still needs repacking), conciseness 4 (handoff is detailed but reproducible). Priorities: prove naming wait with call/callback trace, reach field and verify input/save/reload, then rebuild a self-contained resource bundle. This is a partial engineering milestone, not completed native-port delivery.

## Naming investigation — follow-up

Confirmed on-device: `KEYBOARD enter utf8=0 check=1 callback=... capacity=2004`, then an accepted response and success return without invoking the callback. The game's text-check callback (`main+0x23aed0`) must actually run to complete its naming workflow. `text_input.cpp` now dispatches through the existing real guest callback bridge with the SDK ABI (work buffer at arg+0x4e0, message-size output, String with candidate byte length). It commits only a validated candidate; nonzero check results reopen editing with a new request ID. No name content is logged. A regression first failed on the omitted callback, then passed; rejection/retry/cancel, stale request ID, original capacity/output preservation and callback outside the UI mutex are tested.

That exposed a second pre-existing issue: host `ProfanityFilterC1Ev=ok` left a null vtable, crashing at `main+0x15d5b2c`. Removed the empty constructor/destructor/mask overrides so real SDK objects and checks run. Initialize now explicitly reports SDK NotInitialized `0x292` rather than false success when no system dictionary exists. Host tests verify this result and absence of fake lifecycle/mask hooks.

Latest on-device outcome is **still blocked**: `filter-second-crash.log` shows `NGC_UNAVAILABLE result=0x292`, followed by the game's abort-on-result path (`main+0x15d5be0`, managed initialization at `main+0x1ac3674`). The callback no longer dereferences the null vtable, but the game requires a successfully initialized filter. This is not a completed naming fix.

Static SDK evidence: `ContentsReader::MountContents` at SDK `0x322580` queries/mounts system data title **0100000000000823** (instructions 0x322594/0x322598 construct the ID). Scoped search of this task's work/repository found no profanity dictionary or system-data archive. The next real implementation needs the user's lawfully dumped system-data contents and a read-only MountSystemData adapter; alternatively, substituting a different offline name policy requires an explicit user choice and must not be described as Nintendo dictionary validation.

Verification this follow-up: Release APK builds and is installed; host keyboard, filesystem/host-services and save-transaction contract suites pass; `git diff --check` passes. No coverage percentage measured. SDK full callback/file mounting and gameplay/save-reload remain unverified. No checkpoint commits created because this is a mixed pre-existing dirty checkout; RED/GREEN evidence is recorded here instead. Current evidence: `/private/tmp/pearl-shaders.qVCCRI/{keyboard-current.log,check-crash.log,filter-second-crash.log}`.

## Real system dictionary integration — follow-up

User supplied `/Users/tanyifan/Downloads/22.5.0(1)/22.5.0`. Read-only header inspection identified 238/238 NCA files, including PublicData title `0100000000000823`, file `00d460df4a07b37c6a39c3eb7187f771.nca`, SHA-256 `00d460df4a07b37c6a39c3eb7187f7712c141e9f6ea9e9f87dcbffd6efbc76d4`. Superblock and all six RomFS hash levels validate. Extracted 98 files / 92,011 bytes, including common AC dictionaries, tries and version.dat. Original firmware is unchanged. Derived files are outside the repository at `work/pearl-systemdata-20261020/0100000000000823`.

Added real `QueryMountSystemDataCacheSize` (zero SDK cache required for host directories) and both MountSystemData overloads. They only expose installed real directories under `user/SystemData/<16-hex-id>`, reject links/nonregular archive contents and mount read-only. Missing archives return PathNotFound. The NGC Initialize override was removed, so construction, initialization, dictionary matching and cleanup all run in the actual game SDK. Added settings GetLanguageCode matching the host's existing zh-Hans desired-language configuration to prevent incorrect set-service/FSP IPC routing.

Deployment correction: `user` is **files/games/010018e011d92000/userdata**, not its parent. Word files are now installed at `userdata/SystemData/0100000000000823`. An earlier copy in the parent SystemData directory is unused and retained; do not mistake it for the active location.

On-device `dictionary-language.log` / `naming-passed.log` prove `MOUNT NgWord readonly=1`, actual `OPEN NgWord:/ac_*` file reads, `KEYBOARD check result=0 message_bytes=2`, normal keyboard success and ACTIVITY_FOCUS state=1. Thus the dictionary-dependent crash chain is resolved for this test. However the game STILL remains on the professor scene after successful naming; this is not a naming-flow completion claim.

Further identified missing Resume notifications: only message 15 (focus) was implemented, and SetResumeNotificationEnabled was a no-op. Added subscription handling and coalesced message 16 on actual transition from nonforeground to foreground, after the focus message. Disabled/stable states cannot invent resumes. RED test failed on the missing notification; GREEN passes, as do read-only/missing/symlink archive, language-consistency and existing host-service regressions. Whether this fixes the naming wait is **unverified**.

Latest Release APK builds successfully at `work/android-build/app/outputs/apk/release/app-release.apk`, but installation of the Resume-notification build failed with `adb: device '4c5f417d' not found`. The phone still has the prior real-dictionary/language build, not this newest notification patch. Need reconnect/authorize ADB, install without uninstalling or clearing data, reproduce naming and check `APPLET_RESUME enabled/delivered`, KEYBOARD success and subsequent game progress. The full resource bundle still needs rebuilding for a fresh-device standalone install.

The repository already contained uncommitted DeepSeek changes when this work began. They have been preserved. The current uncommitted changes include the earlier HIPC/filesystem work and this turn's thread, account, HID, and graphics work; do not indiscriminately reset the tree.
