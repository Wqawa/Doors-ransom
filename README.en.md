# Ransom_dev

[中文](README.md) | **English**

A Windows prank program that turns the **Ransom (A-90) encounter** from *DOORS* into something that actually messes with your desktop.

A face surfaces at a random spot on your screen → you have to freeze → a red stop sign flashes for the verdict:
stay still and it withdraws; move and popups flood the screen, your desktop shortcuts get marked "encrypted",
every open program is swept into the taskbar, and you have 90 seconds to scrape together 500 Gold to buy
everything back — otherwise a jumpscare ends the show and the locked shortcuts go to the Recycle Bin.

Written in C++20 with Win32 + GDI+. **All assets are embedded in the executable**, so the artifact is a single file.

> ⚠️ **Read this first**: this program really does change your desktop — it moves icons, blocks right-clicks,
> minimizes your windows, and on timeout throws shortcuts into the Recycle Bin. Every action is reversible;
> see [Safety valve and rollback](#safety-valve-and-rollback). **Read that section before running it for the
> first time.**

---

## ⚠️ About the assets in this repository (important)

The images and audio in this repository **are not my work**:

| Content | Source | License |
|---|---|---|
| `assets/image/`, `assets/audio/`, `assets/ransom.ico` | *DOORS* (a game on Roblox) | **No permission** — copyright belongs to the original creators |
| `assets/RobotoMono-VariableFont_wght.ttf` | Google Fonts | Roboto Mono, SIL OFL 1.1, freely distributable |
| `Ransom_dev/src/third_party/` | stb_vorbis, minimp3 | MIT / public domain / CC0, see [NOTICE.md](NOTICE.md) |

**So: this is a personal, study-oriented fan project.** I release the *code* under MIT, but the *assets* come
with no permission of any kind. If you want to host this repository publicly, consider one of these:

- Drop `assets/image/` and `assets/audio/` and keep only the code — it still compiles and runs; you lose the
  sound effects, but **the face and the stop sign are still drawn** (`face.cpp` generates them procedurally
  rather than loading the original art);
- Or replace them with your own assets, keeping the same file names — `tools/gen_assets.ps1` picks them up
  automatically.

The asset copyright belongs to the original creators. If someone shared this repository with you, be aware of that.

---

## Requirements

| Item | Requirement |
|---|---|
| OS | Windows 10 1809 or later (uses `SetProcessDpiAwareness`, layered windows, low-level input hooks) |
| Compiler | Visual Studio 2022 (v143 toolset). The project file names the `v145` toolset; when opening with an older VS, right-click the project → "Retarget solution" |
| Components | The "Desktop development with C++" workload plus a Windows 10 SDK |
| Language standard | C++20 (the project already sets `/std:c++20`) |
| Runtime | None. The CRT is **statically linked** (`/MT`), so you can copy it anywhere and run it |
| Other | PowerShell (the build automatically runs the asset-packing script) |

The third-party decoders (ogg / mp3) are **inlined as source** and already live in
`Ransom_dev/src/third_party/` — nothing extra to download, no package manager involved.

## Building

```powershell
# Visual Studio: open Ransom_dev.sln, pick Release | x64, build
# Command line:
msbuild Ransom_dev.sln /p:Configuration=Release /p:Platform=x64 /m
```

The output paths are **pinned inside the project file** rather than following the solution file:

```
build\x64\Release\Ransom_dev.exe     ← the only file you need; every asset is inside it
build\x64\Release\Ransom_dev.pdb
build\obj\...                        ← intermediate files
```

Only `x64|Release` is a release configuration; `Win32` and `Debug` are kept around for debugging.

### How the assets get in

At build time `tools/gen_assets.ps1` scans `assets/` and generates `Ransom_dev/assets_gen.rc`
(one `RCDATA` entry plus a numeric ID per asset) and `assets_gen.txt` (the name → ID manifest).
At startup `assets.cpp` reads manifest entry ID 1000 to build its table, then fetches bytes with
`FindResourceW` — so the program **does not depend on the `assets/` directory at runtime**, and
changing assets means rebuilding (deliberate: a single-file artifact is worth more).

> The script decides whether to rewrite those two files by **comparing content, not timestamps**.
> New assets can easily arrive with old timestamps (`Copy-Item` and unzip both preserve them),
> and a timestamp comparison would silently miss them.

## Running it

Just double-click `Ransom_dev.exe`. It shows a settings screen first and only starts the show once you confirm.

Run it in this order the first time, so you can see what it does before it does it:

```powershell
# 1. Just look at the generated face, without touching the desktop
Ransom_dev.exe --face-demo idle

# 2. Just look at the ransom window layout (renders a PNG and exits)
Ransom_dev.exe --ui-preview preview.png --ui-grid

# 3. Time for the real thing — read "Safety valve and rollback" first
Ransom_dev.exe
```

The startup settings screen lets you adjust the **ransom gold target (10–1000, default 500)**, the coin
denominations, volumes, the popup interval, and a **photosensitive-safe mode** (damps the flashing and
noise). `--no-setup` skips it.

## Safety valve and rollback

Every way this program touches your desktop has a way back:

| Situation | How to get out |
|---|---|
| **Want to stop immediately, any time** | Press `Ctrl + Alt + Shift + Q` (global hotkey, exempt from the freeze check, so it never counts as "moving") |
| Gold shortcuts left behind | `Ransom_dev.exe --clean-gold` |
| Shortcuts went to the Recycle Bin | `Ransom_dev.exe --restore` |
| Taken-away windows never came back | The normal exit path **always restores them**; killing it via Task Manager does not — in that case just click the taskbar |
| Desktop icons got moved | Right-click the desktop → "View" → check "Auto arrange icons", then uncheck it — they snap back |

The normal exit paths (paid up / timed out / safety valve / window closed) all restore windows, delete every
generated gold shortcut, and put the confiscated shortcuts back according to the manifest.

> A payment is only honored when it carries the correct passphrase, so dropping a shortcut with the right
> name on your desktop will not fool it.

## Show flow

```
IDLE ──> face surfaces at a random spot FACE ──> teleports to center CENTER (stop sign flashes)
                                                    │
                                  stop sign shows ~1 second: "don't move"
                                                    │
                                    ┌───────────────┴───────────────┐
                              still │                               │ moved
                                    ▼                               ▼
                              ESCAPED                          CAUGHT
                     shows its head once more          jumpscare ──> loading screen
                      and leaves                                              │
                                                                              ▼
                                                                   RANSOM (90 seconds)
                                                     · popups everywhere + desktop icons marked "encrypted"
                                                     · every open program swept into the taskbar
                                                     · click the generated gold shortcuts to reach the target
                                                                              │
                                                          ┌───────────────────┴───────────────────┐
                                                      paid│                                       │timeout
                                                          ▼                                       ▼
                                                     PAID                                    PUNISH
                                          thanks screen + full restore        jumpscare + shortcuts to the Recycle Bin
```

The "moved" test is generous: mouse displacement beyond the tolerance (20px by default), or any key press,
gets you caught. `--tolerance N` loosens it.

## Command-line switches

### Debug and self-check (none of these touch your desktop; they render and exit)

| Switch | Effect |
|---|---|
| `--phase NAME\|N` | Jump straight to a phase and hold there. Names: `潜伏` / `任意位置浮现` / `瞬移到中央` / `停牌判定` / `避开` / `被抓` / `付清` / `惩罚`, or the numbers 0-7 |
| `--no-auto` | Don't advance phases automatically (use with `--phase` to study a single frame) |
| `--face-demo MODE` | Show one face: `idle` / `stop` / `attack` / `thanks` / `loading` |
| `--face-dump DIR` | Export the procedurally generated faces to PNG files, then exit |
| `--ui-preview PATH` | Render the ransom window layout to a PNG, then exit (no need to run the whole show) |
| `--ui-grid` | With `--ui-preview`, overlay a coordinate grid |
| `--payup` | With `--ui-preview`, preview the payment screen instead |
| `--setup-ui PATH` | Render the **startup settings screen** to a PNG, then exit |
| `--setup-grid` | With `--setup-ui`, overlay a coordinate grid |
| `--notice-ui PATH` | Render the **notice/prompt screen** to a PNG, then exit |
| `--fx-demo NAME` | Pick an effect layer: `glow` (corner red glow) / `black` (blackout + noise) / `stop` (red screen) |
| `--fx-dump PATH` | **Two independent switches**: `--fx-demo` picks the effect, `--fx-dump` gives the output path. You need both to get an image |
| `--audio-dump PATH` | Render every sound effect offline to a WAV, then exit (for checking waveforms) |
| `--theme-dump PATH` | Render the processed theme song to a WAV, then exit (for listening) |
| `--diag PATH` | Write the diagnostic log to a file. A GUI-subsystem app has no console, so this is how you debug |

### Show switches

| Switch | Effect |
|---|---|
| `--tolerance N` | Mouse tolerance in pixels, default 20 |
| `--no-setup` | Skip the startup settings screen and go straight into the show |
| `--no-audio` | Don't start audio |
| `--no-overlay` | Don't start the desktop overlay (no boxes drawn, no clicks intercepted) |
| `--overlay-topmost` | Keep the overlay topmost (by default it sits at the desktop layer and normal windows cover it) |
| `--no-block-menu` | Don't block the right-click menu on encrypted icons (**for troubleshooting**) |
| `--no-lockdown` | Don't minimize other programs during the ransom (**for troubleshooting**) |
| `--no-guardian` | Don't start the guardian process (**for troubleshooting**, see below) |
| `--image-dir DIR` / `--audio-dir DIR` | Override the asset directories (default: the embedded resources inside the exe) |

### Emergency and internal switches

| Switch | Effect |
|---|---|
| `--clean-gold` | Scan for and delete leftover gold shortcuts, then exit |
| `--restore` | Restore confiscated shortcuts from the Recycle Bin using the manifest, then exit |
| `--pay N --token T` | Deliver a payment to an **already running** show (`N` = gold amount). Requires an instance holding a `RansomDevIpcWnd` window |
| `--guardian PID GEN` | Guardian-process mode, launched by the main program itself; **you normally never use this by hand** |

> **What the guardian is**: once the show starts, the main program spawns a copy of itself to watch over it.
> If you kill the main process from Task Manager, the guardian immediately rules it "cheating" and attacks.
> `--no-guardian` turns that behavior off.

## Project layout

```
Ransom_dev.sln
├─ Ransom_dev/                  project files
│  ├─ Ransom_dev.vcxproj        output dirs, toolset, and the asset-packing target all live here
│  ├─ Ransom_dev.rc             app icon + #include of the asset manifest
│  ├─ assets_gen.rc / .txt      ← generated; don't edit by hand
│  └─ src/
│     ├─ entity_main.cpp        entry point: command line, message loop, panic hotkey, IPC
│     ├─ director.cpp           encounter scheduler (that flow chart above is this file)
│     ├─ face.cpp               A-90's face, generated procedurally
│     ├─ fx.cpp                 full-screen effect layers: corner glow, blackout, film-grain noise
│     ├─ popup.cpp              popup flood + the ransom window
│     ├─ aero_window.cpp        window wrapper with a custom-drawn title bar
│     ├─ ui_layout.cpp          data-driven layout of the ransom window
│     ├─ setup_ui.cpp           startup settings screen
│     ├─ settings.cpp           settings persistence (stored under %LOCALAPPDATA%)
│     ├─ desktop_overlay.cpp    desktop icon overlay + input interception (all the hooks)
│     ├─ lockdown.cpp           sweeps other programs into the taskbar and keeps them there
│     ├─ guardian.cpp           guardian process: turns on you if the main process is killed
│     ├─ gold.cpp               gold shortcuts (double-clicking one = paying up)
│     ├─ recycle.cpp            timeout punishment: sends shortcuts to the Recycle Bin
│     ├─ motion.cpp             the "did you move" test for mouse/keyboard
│     ├─ audio.cpp / audio_clip.cpp   mixing, decoding (ogg/mp3)
│     ├─ assets.cpp / image_blob.cpp  reading the embedded assets
│     ├─ entity_log.cpp         diagnostic log
│     └─ third_party/           stb_vorbis (ogg), minimp3 (mp3), inlined as source
├─ assets/                      assets (packed into the exe)
│  ├─ main_window.ini           ransom window layout
│  ├─ payup.ini                 payment screen layout
│  ├─ image/  audio/            images, audio
│  ├─ ransom.ico                app icon
│  └─ dump/                     faces the program generated itself (sample exports, deletable)
└─ tools/gen_assets.ps1         scans assets\ and generates the resource manifest
```

## A few design tradeoffs (why it is written this way)

**Layout isn't hard-coded.** The ransom window's positions, sizes and colors all live in
`assets/main_window.ini`, so you can change the layout without recompiling; `--ui-preview` renders the
layout straight to a PNG with an optional coordinate grid for reading off numbers.

**Changing the desktop has to happen at the system's lowest level.** "Icons won't drag" and "no context
menu appears" both work by swallowing messages in a `WH_MOUSE_LL` low-level hook. One counter-intuitive
point: **dragging must be intercepted on button-down, not button-up** — the drag loop runs on this
process's UI thread, and once it exceeds the system's hook timeout (~1 second) the hook is **silently
unhooked**; intercepting on button-up means "drag slowly enough" gets around it. Being unhooked is
nasty in its own right, so the overlay's timer re-installs the hook on every tick as a fallback.

**The hit-test rectangles and the drawn rectangles are two different sets.** The boxes the overlay draws
jitter randomly frame to frame (it makes the icons look agitated), but hit-testing must use the
non-jittering set — otherwise the icons wiggle, the hit area drifts with them, and clicks on an icon
get missed.

**Clearing the field has to avoid whack-a-mole.** After minimizing every window, a few UWP windows pop
themselves back. A watchdog that just pushes them down again turns into a futile 150ms loop (one run
was measured at 1159 attempts in a single show). So there is a "give up" rule: if the same window won't
stay down after 6 tries, abandon it and write its name to the log.

**Payments need a passphrase.** The main program exposes itself as a message window and verifies the
passphrase when accepting money, so anyone dropping a same-named shortcut on the desktop can't just
"pay" the ransom.

## Known limitations

- **Architecture**: only the x64 release configuration is done. Win32 compiles but is untested.
- **Multiple monitors**: face positions and popup distribution are computed from the primary screen;
  nothing happens on secondary displays.
- **Taskbar**: clicking a taskbar button can still pull a swept-away window up for a **flicker** — that
  is an ordinary mouse message to explorer and outside the keyboard hook's reach. Closing it properly
  would require a `WH_SHELL` hook.
- **UWP windows**: see the whack-a-mole note above; a few UWP apps won't stay down.
- **Force-kill doesn't restore**: killing the process from Task Manager leaves confiscated windows and
  gold shortcuts behind — clean up with `--clean-gold`.
- **explorer restart**: the overlay finds the desktop window again, but icon positions are lost for
  one frame.

## License

Code: MIT, see [LICENSE](LICENSE).
Third-party and assets: see [NOTICE.md](NOTICE.md) — **the game assets are not covered by MIT**.
