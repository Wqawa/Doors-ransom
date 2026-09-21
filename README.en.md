# Ransom_dev

[中文](README.md) | **English**

(Archived. Please go to the main repository: https://github.com/Wqawa/Doors_RanSoM)

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

| Content | Source | Notes |
|---|---|---|
| `assets/image/`, `assets/audio/`, `assets/ransom.ico` | *DOORS* (a game on Roblox) | A-90's appearance, the stop sign, the theme song and sound effects are all the original work's copyrighted assets |
| `assets/RobotoMono-VariableFont_wght.ttf` | Google Fonts | Roboto Mono, SIL Open Font License 1.1, freely distributable |
| `Ransom_dev/src/third_party/` | stb_vorbis, minimp3 | Public domain / MIT / CC0 dual-licensed, see [NOTICE.md](NOTICE.md) |

**So: this is a personal, study-oriented fan project.** I release the *code* under MIT, but the *assets* come
with no permission of any kind. Decide for yourself whether you want to host this repository publicly. If you
do, consider one of these:

- Drop `assets/image/` and `assets/audio/` from the repository and keep only the code (the program will not be
  able to draw the face or play sound, but it still compiles);
- Or replace them with your own assets before publishing.

**If someone shared this repository with you, be aware that the asset copyright belongs to the original creators.**

---

## Requirements

| Item | Requirement |
|---|---|
| OS | Windows 10 1809 or later (uses `SetProcessDpiAwareness`, layered windows, low-level input hooks) |
| Compiler | Visual Studio 2022 (v143 toolset). The project file names the `v145` toolset; when opening with an older VS, right-click the project → "Retarget solution" |
| Components | The "Desktop development with C++" workload plus a Windows 10 SDK |
| Language standard | C++20 (the project already sets `/std:c++20`; no manual setup needed) |
| Runtime | None. The CRT is **statically linked** (`/MT`), so you can copy it anywhere and run it |
| Other | PowerShell (the build automatically runs the asset-packing script) |

The third-party decoders (ogg / mp3) are **inlined as source** and already live in
`Ransom_dev/src/third_party/` — there is nothing extra to download and no package manager involved.

## Building

### Visual Studio

Open `Ransom_dev.sln`, select `Release | x64`, and build.

### Command line

```powershell
msbuild Ransom_dev.sln /p:Configuration=Release /p:Platform=x64 /m
```

### Where the output goes

The output directories are **pinned explicitly** in the project file, so they do not follow the solution file around:

```
build\x64\Release\Ransom_dev.exe     <- this one file is all you need; every asset is inside it
build\x64\Release\Ransom_dev.pdb
build\obj\...                        <- intermediate files
```

Only `x64|Release` is a release configuration; the `Win32` and `Debug` configurations are kept around for debugging.

### How the assets get inside

At build time `tools/gen_assets.ps1` scans `assets/` and generates:

- `Ransom_dev/assets_gen.rc` — one `RCDATA` entry per asset, each with a numeric ID
- `Ransom_dev/assets_gen.txt` — the name → ID manifest

`assets.cpp` reads resource ID 1000 (that manifest) once at startup to build a table, then pulls bytes with
`FindResourceW`. So at runtime the program does **not** depend on the `assets/` directory, and changing an asset
requires a rebuild (this is deliberate: a single-file artifact is worth more).

> The script decides whether to rewrite those two files by **comparing content**, not timestamps — a new asset
> can easily arrive carrying an old timestamp (`Copy-Item` and unzip both preserve the original), and a
> timestamp-based check would silently skip the new file.

## Running it

Just double-click `Ransom_dev.exe`. It lurks for a few seconds and then the show starts.

**For a first run, look around in this order:**

```powershell
# 1. Just look at the generated face, without touching your desktop
Ransom_dev.exe --face-demo idle

# 2. Look at the ransom window layout (renders to a PNG and exits)
Ransom_dev.exe --ui-preview preview.png --ui-grid

# 3. Now run the whole thing — but read "Safety valve" below first
Ransom_dev.exe
```

## Safety valve and rollback

Every place where the program touches your desktop has a way back:

| Situation | How to get out |
|---|---|
| **Want to stop immediately, any time** | Press `Ctrl + Alt + Shift + Q` (a global hotkey, exempt from the movement check, so it never counts against you) |
| Gold shortcuts left behind | `Ransom_dev.exe --clean-gold` |
| Shortcuts were sent to the Recycle Bin | `Ransom_dev.exe --restore` |
| Windows we swept away did not come back | Every normal exit path **always restores them**; being force-killed from Task Manager does not — in that case just click them on the taskbar once |
| Desktop icons were moved | Right-click the desktop → "View" → toggle "Auto arrange icons" on and off again; the icons snap back |

Every normal exit path (paid, timed out, safety-valve hotkey, window closed) will: restore the windows, delete
every generated gold shortcut, and put the confiscated shortcuts back where they came from, following the manifest.

## The show

```
IDLE ──> face surfaces at a random spot FACE ──> teleports to center CENTER (stop sign appears)
                                                        │
                                    stop sign shows for ~1s: "do not move"
                                                        │
                                        ┌───────────────┴───────────────┐
                             didn't move│                               │moved
                                        ▼                               ▼
                                  ESCAPED                           CAUGHT
                            face shows once more              jumpscare ──> loading screen
                             and then leaves                          │
                                                                      ▼
                                                            RANSOM (90 seconds)
                                              · popups everywhere + desktop icons marked "encrypted"
                                              · every open program swept into the taskbar
                                              · click the gold shortcuts that spawn on the desktop to reach 500 Gold
                                                                      │
                                              ┌───────────────────────┴───────────────────────┐
                                        paid  │                                               │timeout
                                              ▼                                               ▼
                                            PAID                                           PUNISH
                                    thank-you screen + full restore            jumpscare + shortcuts to the Recycle Bin
```

The "you moved" check is generous: mouse movement beyond the tolerance (10 px by default) or any keypress gets
you caught. `--tolerance N` loosens it.

## Command-line options

| Option | Effect |
|---|---|
| `--phase NAME\|N` | Jump straight to a phase and stop there. Names: `潜伏 / 任意位置浮现 / 瞬移到中央 / 停牌判定 / 避开 / 被抓 / 付清 / 惩罚`, or the number 0-7 |
| `--no-auto` | Do not advance phases automatically (use with `--phase` to study a single frame) |
| `--tolerance N` | Mouse tolerance in pixels, 20 by default |
| `--no-audio` | Do not start audio |
| `--no-overlay` | Do not start the desktop overlay (no boxes drawn, no clicks blocked) |
| `--overlay-topmost` | Keep the overlay on top (by default it sits at the desktop layer and normal windows cover it) |
| `--no-block-menu` | Do not block the right-click menu on encrypted icons (**for troubleshooting**) |
| `--no-lockdown` | Do not minimize other programs during the ransom (**for troubleshooting**) |
| `--diag PATH` | Write the diagnostic log to a file. A GUI subsystem has no console, so this is how you debug |
| `--face-demo MODE` | Show one face only: `idle` / `stop` / `attack` / `thanks` / `loading` |
| `--face-dump DIR` | Export the procedurally generated faces to PNG and exit |
| `--audio-dump PATH` | Render every sound effect offline to a WAV and exit (for checking waveforms) |
| `--theme-dump PATH` | Export the processed theme song to a WAV and exit (for listening) |
| `--ui-preview PATH` | Render the ransom window layout to a PNG and exit (for tweaking the layout without running the show) |
| `--ui-grid` | With `--ui-preview`, overlay a coordinate grid on the preview |
| `--payup` | With `--ui-preview`, preview the payment screen layout instead |
| `--fx-demo NAME PATH` | Export the effect layer to a PNG on its own: `glow` / `black` / `stop` |
| `--clean-gold` | Just scan for and delete leftover gold shortcuts, then exit |
| `--restore` | Restore confiscated shortcuts from the Recycle Bin using the manifest, then exit |
| `--image-dir DIR` / `--audio-dir DIR` | Override the asset directories (by default assets come from the embedded resources) |

## Project layout

```
Ransom_dev.sln
├─ Ransom_dev/                  project files
│  ├─ Ransom_dev.vcxproj        output dirs, toolset and the asset-packing target all live here
│  ├─ Ransom_dev.rc             app icon + #include of the asset manifest
│  ├─ assets_gen.rc / .txt      <- generated, do not edit by hand
│  └─ src/
│     ├─ entity_main.cpp        entry point: command line, message loop, panic hotkey
│     ├─ director.cpp           the encounter scheduler (the flowchart above is this file)
│     ├─ face.cpp               A-90's face, procedurally generated
│     ├─ fx.cpp                 full-screen effect layer: corner glow, black veil, noise
│     ├─ popup.cpp              the popup storm + the ransom window
│     ├─ aero_window.cpp        window wrapper with a custom-drawn title bar
│     ├─ ui_layout.cpp          **data-driven layout** for the ransom window
│     ├─ desktop_overlay.cpp    desktop icon overlay + input blocking (all the hooks live here)
│     ├─ lockdown.cpp           sweeps other programs into the taskbar during the ransom, and keeps them there
│     ├─ gold.cpp               gold shortcuts (double-clicking one = paying the ransom)
│     ├─ recycle.cpp            timeout punishment: shortcuts to the Recycle Bin
│     ├─ motion.cpp             the "did you move?" mouse/keyboard detection
│     ├─ audio.cpp / audio_clip.cpp   mixing and decoding (ogg/mp3)
│     ├─ assets.cpp / image_blob.cpp  reading the embedded assets
│     ├─ entity_log.cpp         diagnostic log
│     └─ third_party/           stb_vorbis (ogg), minimp3 (mp3), inlined as source
├─ assets/                      assets (packed into the executable)
│  ├─ main_window.ini           layout of the ransom window
│  ├─ payup.ini                 layout of the payment screen
│  ├─ image/  audio/            images and audio
│  ├─ ransom.ico                application icon
│  └─ dump/                     exported sample faces (safe to delete)
└─ tools/gen_assets.ps1         scans assets\ and generates the resource manifest
```

## A few design trade-offs (and why)

Some pitfalls were hit along the way. They are all recorded in the code comments; here are the interesting ones:

**The layout is not hard-coded.** Every position, size and colour of the ransom window lives in
`assets/main_window.ini`, so tweaking it needs no rebuild; combined with `--ui-preview` you can render the layout
straight to a PNG, grid overlay included, to read off coordinates.

**Touching the desktop has to happen at the lowest level of the OS.** "The icon cannot be dragged" and "no
right-click menu appears" are both implemented by swallowing messages in a `WH_MOUSE_LL` low-level hook. One
counter-intuitive point: **drag prevention must intercept the button *down*,** not the button *up* — the
drag-and-drop loop runs on this process's UI thread, and once it runs past the timeout the system gives a hook
(about one second) the hook is **silently removed**. Intercepting on button-up would let anyone bypass it simply
by dragging slowly. Hooks being dropped is a nasty problem in itself, so the overlay's timer re-installs the
hooks on every tick as a safety net.

**There are two sets of rectangles: one for drawing, one for hit testing.** The boxes the overlay draws carry a
per-frame random jitter (so the icons look restless), but hit testing must use the un-jittered set — otherwise
the detection region drifts along with the jitter and a click that clearly lands on an icon gets missed.

**The lockdown has to guard against whack-a-mole.** After minimizing every window during the ransom, a few UWP
windows bounce right back. If the watchdog just shoved them down again blindly, it would become a futile loop
every 150 ms (measured: 1159 rounds in a single show). So it knows when to give up: after 6 attempts on the same
window it lets that one go, and writes the name to the log.

## Known limitations

- **Architecture**: only the x64 release configuration is maintained. The Win32 configuration compiles but is untested.
- **Multiple monitors**: face positions and popup distribution are computed from the primary monitor's size; nothing happens on secondary monitors.
- **Taskbar**: clicking a taskbar button can still pull a swept-away window up for a **brief flash** — that click is an ordinary mouse message sent to explorer and outside the jurisdiction of a keyboard hook. Closing that hole completely would require a `WH_SHELL` hook.
- **UWP windows**: see the whack-a-mole note above; a few UWP apps cannot be kept down.
- **Force-kill does not restore**: if the process is force-killed from Task Manager, the swept-away windows and gold shortcuts are left behind; clean up with `--clean-gold`.
- **explorer restart**: the overlay re-finds the desktop window, but icon position information is lost for one frame.

## License

Code: MIT, see [LICENSE](LICENSE).
Third-party and assets: see [NOTICE.md](NOTICE.md) — **the game assets are not covered by the MIT license**.
