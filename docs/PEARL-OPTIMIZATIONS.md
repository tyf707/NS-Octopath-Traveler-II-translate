# Pokémon Shining Pearl: optimizations on this branch

Target: `010018e011d92000`, version 1.3.0 (update 393216), Unity 2019.4.27f1.
Device used for every number below: Xiaomi 17 Ultra (Adreno 840, 2608×1200, Android 17).

## Summary

| Area | Before | After |
|---|---|---|
| GPU per frame (town scene) | 26.2 ms | **17.4 ms** |
| Frame rate, light scenes | 30 fps | **58–60 fps** |
| Frame rate, town | 30 fps | **40–47 fps** |
| Frame rate, indoor scenes | 25–26 fps | **27–35 fps** |
| Warm scene transition | 6.8 s | **5.8 s** |
| Display | 16:9 letterboxed | **stretch / fit / crop**, switchable |
| Virtual buttons | always visible | **hidden while a gamepad is connected** |
| Backgrounding | host-thread crash on surface destroy | no crash |

## 1. Scene transitions (indoor ↔ outdoor)

Measured by sampling the screen every 0.3 s and timing the fade-to-black and the
first bright frame.

The important finding: **scene loading is frame-bound**. Unity's `LoadSceneAsync`
advances a fixed amount of work per frame, so the transition time scales with the
frame rate that the port can sustain during the load:

| Pacing | Actual fps during the scene | Transition time |
|---|---|---|
| Game's own interval (30 Hz) | 30 | 6.8 s |
| `interval=1` (60 Hz paced) | ~40 | 5.2 s |
| `interval=1`, warm cache | 42–47 | **5.8 s** |

`6.8 × 30/40 ≈ 5.1` — the measured 5.2 s matches a pure frame-bound model, so raising
the frame rate is also the transition optimization.

Cold vs warm matters: the first load of an area after an app start takes about
**14.6 s**, and loading a save takes about **27.4 s**. During those loads the host's
I/O is only 20–25 ms per batch and shader compilation is near zero, so the remaining
time is the game's own per-frame scene construction.

## 2. Frame rate

- Present pacing defaults to `interval=1` (no 30 Hz throttle, capped at 60 Hz).
  `debug.ot2.interval=-1` restores the game's own 30 Hz.
- The game's logic is **time-based**, not frame-based: holding a direction for a fixed
  time moves the player the same distance at 30 fps and at 44 fps (verified by
  block-matching the camera shift: −268/−272 px at 30 fps, −272 px at 44 fps). Raising
  the frame rate therefore does not speed up gameplay — only the loads get shorter.

**Clear fast paths** (the largest GPU win):

- Full-image colour/depth clears now use `vkCmdClearColorImage` /
  `vkCmdClearDepthStencilImage` instead of a full-screen triangle with `LOAD`/`STORE`.
- Depth-only clears on a `DEPTH_STENCIL` image keep the stencil intact by using a
  `LOAD`/`STORE` attachment plus `vkCmdClearAttachments` with only the depth aspect.

Effect measured with `profile-gpu`:

| | GPU per frame | `clear-draw:4096x4096` | `clear-draw:1280x720` |
|---|---|---|---|
| Before | 26.2 ms | 7.0 ms (2×/frame) | 2.2 ms (6×/frame) |
| After | **17.4 ms** | gone | gone |

Remaining heavy regions are the game's own render passes (3.7 ms, 2.2 ms, 1.2 ms …);
indoor scenes still cost about 23 ms of GPU per frame.

- Geometry cache budget raised from 64 to 128 MiB — the cache used to sit at
  63.8/64 MiB and evict continuously.

## 3. Virtual buttons hidden with a gamepad

- A connected gamepad (gamepad/joystick input source, virtual `uinput-` devices
  ignored) hides the on-screen pad; it returns when the gamepad disconnects.
- A 2 s poll covers devices that do not send `onInputDeviceAdded`.
- `files/shaders/hide-controls` forces the same state for testing.
- Verified by forcing the hide state: the frame contains no virtual buttons at all, and
  the pad reappears within 2 s after the marker is removed.

## 4. Display fill modes

The source is 16:9; the panel is 2608×1200 (20:9).

| Mode | Behaviour |
|---|---|
| `stretch` (default) | fills the screen, ~22% horizontal stretch, all UI visible |
| `fit` | keeps the aspect ratio, black bars left/right |
| `crop` | fills the screen, trims the top/bottom (cuts dialog boxes) |

`debug.ot2.aspect` selects the mode; the in-app panel switches it live.

## 5. On-device settings panel

Press the on-screen **“−” and “+”** buttons together (two fingers, or within 0.7 s) to
open or close it. Nothing is shown until it is opened — the trigger area is invisible and
the buttons keep working as normal game input.

| Option | Values |
|---|---|
| Display fill | stretch (no bars) / fit (letterbox) / crop (trim) |
| Frame-rate ceiling | 60 fps (default, faster loads) / follow the game (30 fps) |
| Virtual buttons | auto (hide with gamepad) / always show / always hide |

- Changes apply immediately without restarting: switching to `fit` produces black bars on
  the next frame, switching to 30 fps changes `PRESENT_RATE` to 30.00 at once.
- The panel is an in-activity view, not a dialog, so the game keeps rendering behind it.
- Choices are stored in `SharedPreferences` (`ot2-settings`) and restored at launch;
  they take precedence over the built-in defaults.

## 6. Stability and diagnostics

- Presentation is suspended unless the activity is resumed and focused. Before this,
  backgrounding the app destroyed the surface while the presenter still used it, which
  crashed a host thread with `Signal 11 … Signal outside guest thread`.
- `profile-gpu` / `profile-cpu-detail` / `verbose-stats` markers print per-region GPU
  time and per-frame CPU breakdown; `debug.ot2.servicecheck=1` lists SDK symbols the
  host does not hook.

## What was not improved

- Indoor scenes remain GPU-bound (~23 ms/frame); the remaining cost is spread over
  ~50 render passes that the game issues.
- Cold loads (14–27 s) are dominated by the game's frame-bound scene construction.
- The settings panel is per app instance; the values live in the app's private storage.
