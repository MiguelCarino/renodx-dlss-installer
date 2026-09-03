# dlss5-linux

DLSS 5 "Neural Rendering" on Fedora/Linux via Proton, on an RTX 40 (Ada) card.

This is the `linux/` directory of a fork of
[yumlevi/renodx-dlss-installer](https://github.com/yumlevi/renodx-dlss-installer).
The repository root is that project and its README: a Windows-only C# WPF
installer. Nothing here replaces it — this directory is a second implementation
of the same idea, for people playing the game on Linux under Proton.

The upstream repo is ~99% a vendored copy of
[crosire/ReShade](https://github.com/crosire/reshade); the only original logic is
`../reshade/setup/MainWindow.Custom.cs`, and it does nothing but copy files. The
scripts here do the same file placement natively on Linux, plus the Proton wiring
the Windows installer never had to think about.

## Quick start

Every path below is relative to this directory.

```bash
cd linux
./dlss5-gui.py                                    # web GUI: scan, probe, install, launch
```

Or from the shell:

```bash
./dlss5-linux.sh --check                          # preflight only
./dlss5-linux.sh --game-dir=/path/to/game --probe # will this game work?
./dlss5-linux.sh --game-dir=/path/to/game         # install (optiscaler engine)
./dlss5-linux.sh --game-dir=/path/to/game --uninstall
```

Then set the launch options the script prints, and pick GE-Proton under
Properties → Compatibility.

`--help` lists every option. Three that the examples above do not show:

| Flag | Effect |
|---|---|
| `--dry-run` | Print every download, copy, extraction, deletion and ini edit that would happen. Changes nothing on disk. Combines with `--configure`, `--uninstall` and `--install-proton`. |
| `-y`, `--yes` | Answer prompts yes. There is one: "an install already exists here, uninstall first?" |
| `-h`, `--help` | The full option list, then exit. |

## Web GUI

`./dlss5-gui.py` serves a local page that walks every Steam library, probes each
installed game, and installs/uninstalls/launches without you typing a path.
Standard library only — no pip, no venv.

It binds to `127.0.0.1` and mints a fresh token per run; the URL printed in the
terminal is the only way in. Install is disabled outright for any game where the
probe finds anti-cheat.

"Launch with overrides" runs the game through Proton directly, with the
environment applied by the GUI process, so nothing edits Steam's config files.
It sets `SteamAppId` / `SteamGameId` / `STEAM_COMPAT_APP_ID` so `steam_api64.dll`
can still attach to the running Steam client, and `STEAM_COMPAT_MOUNTS` so the
library gets a drive letter instead of being reached through `Z:\`.

**Games with Denuvo or strict Steam DRM will still refuse a launch that did not
come from Steam** — they exit within a second, before creating a graphics device,
which makes it look like the injector failed when it never got a chance to run.
For those, copy the Steam launch-option string instead and start from Steam. The
GUI flags this: a launch that ends in under 15 seconds gets an explicit note.

```
./dlss5-gui.py --port 8777 --no-open
```

### Folders and logs

**Open folder** opens the resolved binary directory in your file manager — the
directory the probe picked, not the one you started from, which is the one the
DLLs actually landed in.

**Open log file** opens `OptiScaler.log` (or `ReShade.log`) in your default
editor. **Open captures** appears only once `dlssnr-capture/` exists — the
before/after frame pairs the neural pass writes each session.

**Copy log** copies whichever tab is showing, straight to the clipboard, with a
`document.execCommand` fallback for when the clipboard API refuses (it rejects
if the document is not focused).

The log panel has two tabs. **Output** is the installer/uninstaller job stream.
**Game log** is the game's own log, tailed from disk. Launching from the GUI
switches to it automatically and follows it live at 1.5 s while the game runs,
so the overlay's verdict — "the model would not initialise" and friends — shows
up without going to find a file. Only the last 160 KB is sent to the browser;
the panel says when it is showing a tail.

OptiScaler ships `LogToFile=true` at `LogLevel=2` (Info), so the log exists after
a run with no configuration. Set `LogLevel=0` in `OptiScaler.ini` for trace
detail when something is failing and Info is not saying why.

## Configuring OptiScaler.ini

Neural rendering ships **off** (`[DlssNr] Enabled=auto`, and auto means false).
Rather than hunting through a 1,600-line ini or an overlay:

```bash
./dlss5-linux.sh --game-dir=PATH --configure --nr=on
./dlss5-linux.sh --game-dir=PATH --configure --upscaler=dlss
```

The GUI exposes both as an "Enable Neural Rendering" checkbox and a "DX12
upscaler" dropdown once the game is installed.

`--upscaler` matters under Proton. OptiScaler probes NVAPI during startup to
decide what the GPU can do, and that probe can run *before* the game loads
`nvapi64.dll` — it then logs `Upscaler support - dlss: false` and quietly falls
back to XeSS even on a 4090. Forcing `dlss` sidesteps the detection.

The editor is section-aware (`OptiScaler.ini` has seven different keys named
`Enabled=`) and writes the file back with its CRLF endings and UTF-8 BOM intact,
so a change rewrites one line and leaves the other 1,600 byte-identical.

The BOM sits on line 0, which in `OptiScaler.ini` is a section header, so it is
stripped before the header is matched — otherwise `[Upscalers]` would not parse
as one and every key under it would be unreachable. It is written back intact.

A key the editor cannot find is a failure, not a warning: `--configure` says
`warn [DlssNr] Enabled not found` and exits 1, so the GUI reports it instead of
showing a success it did not get.

## Probe

`--probe` reads a game folder and changes nothing. Add `--json` for the
machine-readable form the GUI consumes.

| Verdict | Meaning |
|---|---|
| `supported` | D3D12 + a temporal upscaler. Install it. |
| `partial` | D3D11. The game's own DLSS can't be ridden; OptiScaler must substitute its FSR/XeSS "w/Dx12" backend and the pass rides that bridge. |
| `unsupported` | No upscaler, Vulkan, or a native Linux build — nothing to hook. |
| `blocked` | Anti-cheat found. Install refuses; `--force` overrides at your own risk. |
| `unknown` | An upscaler is there, but the binary's graphics API could not be pinned down — it imports nothing recognisable, or it resolves to OpenGL. Not a refusal: install and see. |

`blocked` is checked first, then `unsupported`, so an anti-cheat hit outranks
everything else the probe found.

Detection is best-effort in one direction: a game with engine-integrated FSR2 and
no separate DLL will read as `unsupported` even though it might work. The probe
says so in its reasons.

Resolving the real game binary is the fiddly part, and the probe handles the
layouts that actually occur: launcher-in-root (`bin/x64`), RGG's `runtime/media`,
Unreal shim launchers whose binary sits deeper, executables that import no
graphics API at all and resolve D3D12 through `LoadLibrary` (Cyberpunk 2077),
bundled redistributables larger than the game itself (Red Dead Redemption 2's
Social Club installer), and titles shipping both a DX11 and a DX12 binary
(The Witcher 3 next-gen, where only `bin/x64_dx12` is usable).

Anti-cheat is scanned from the **game root**, not from the resolved binary
directory — `find` only descends, and Halo MCC keeps `EasyAntiCheat/` three
levels above `MCC/Binaries/Win64`. The probe works the root out for itself
rather than trusting the path you typed: it walks up from that path until the
parent is a launcher's game container (`steamapps/common`, `compatdata`,
`Heroic`, `GOG Games`, `Program Files` and so on), takes the directory it came
from as the root, and scans five levels down from there.

`Program Files` has to be in that list for the same reason `drive_c` is in it:
in a Wine prefix a game sits *under* `drive_c/Program Files/`, so stopping at
`drive_c` would hand back `Program Files` itself and every game in the prefix
would inherit its neighbours' anti-cheat.

The walk is bounded, because one level too far lands in the container itself and
the neighbouring game's anti-cheat would then read as this game's. It gives up
after six levels and stops dead at `$HOME` or `/`. If it never finds a container
it keeps the path it was given — so a game installed somewhere the list does not
name, probed at `Binaries/Win64`, is scanned only from `Binaries/Win64`. Point
it at the game root yourself in that case.

## Two engines

| | `--engine=optiscaler` (default) | `--engine=reshade` |
|---|---|---|
| Injector | [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) | ReShade 6.8.0 add-on build + RenoDX `renodx-dlss5` |
| Layers | one | two (ReShade hooks, then the add-on) |
| Reported perf | ~2× the ReShade route | baseline |
| Overlay key | `Insert` | `Home` |
| Linux support | ships its own `setup_linux.sh` | none upstream; this script supplies it |

Switch engines with `--uninstall` then reinstall. Both write a
`.dlss5-linux.state` file so uninstall knows which proxy DLL to remove.

`--uninstall` resolves the game directory the same way installing does, so the
game root works as well as the exact directory the files went into. A state file
in the path you gave is taken at face value; otherwise it resolves. If that file
exists but names no proxy DLL, uninstall stops and says so rather than reporting
that it removed nothing — an empty name would otherwise resolve to the game
directory itself. Under `--dry-run` it says *would remove*, because it did not.

## Requirements

- **RTX 40 series (Ada, compute capability 8.9).** The `nvngx_dlssnr.dll` the
  installer downloads has been patched to `sm_89` cubins only — verified:
  15 fatbins, all `sm_89`, zero `sm_120`, zero PTX. It will not run on Blackwell
  or on RTX 20/30.
- NVIDIA driver with the Wine NGX bridge: `/usr/lib64/nvidia/wine/{nvngx,_nvngx}.dll`
  and `libnvidia-ngx.so`.
- **Proton 9+ or GE-Proton.** Proton 7 predates the NGX bridging this relies on.
  `--install-proton` fetches the latest GE-Proton for you.
- **A D3D12 game that already runs DLSS.** The neural pass reads the depth and
  motion vectors the game hands to DLSS every frame. No DLSS to intercept, no effect.
- `curl unzip binutils python3` (and `p7zip p7zip-plugins` for the reshade engine).

## How it works on Linux

Proton executes NGX snippets as Windows DLLs inside Wine and bridges them to the
native driver via `nvngx.dll` → `_nvngx.dll` → `libnvidia-ngx.so`. Your driver
shipping `nvngx_dlssg.dll` as a PE DLL is direct evidence of that path.

`nvngx_dlssnr.dll` imports only `VERSION/ADVAPI32/USER32/KERNEL32` — no `d3d12.dll`,
no `nvcuda.dll`. It takes the D3D12 device as a COM pointer, so there is no
import-resolution wall under Wine.

**The one genuine unknown:** under Proton that D3D12 device is vkd3d-proton, not
real D3D12. NVIDIA's bridge plumbs the `dlss` / `dlssg` / `dlssd` features; the
Neural Rendering feature ID is new and may not be wired through. This cannot be
settled by reading code — only by running it. If it fails, the overlay states the
reason under the enable checkbox, and `OptiScaler.log` has more.

## Launch options

```
WINEDLLOVERRIDES="dxgi=n,b" PROTON_ENABLE_NVAPI=1 PROTON_ENABLE_NGX_UPDATER=1 \
VKD3D_CONFIG=dxr11 PROTON_HIDE_NVIDIA_GPU=0 %COMMAND%
```

Match `dxgi` to whatever `--hook` you chose. `PROTON_ENABLE_NGX_UPDATER` manages
`nvngx_dlss/dlssg/dlssd` and does not touch `nvngx_dlssnr.dll`.

## Caveats

This repository holds no binaries. Every file the installer puts in your game
folder is fetched at run time, over https, from somebody else's release page,
unpacked, and copied in. There is no checksum, no signature check and no pinning
beyond the literal URLs in `dlss5-linux.sh` — a release that is retagged or
replaced upstream changes what you install, and you would not be told.

| Host | What comes from it |
|---|---|
| `github.com/Dagherbou/OptiScaler_DLSSNR` | the OptiScaler build carrying the neural pass — the default engine |
| `github.com/yumlevi/renodx-dlss-installer` | `streamline.zip`, which is where `nvngx_dlssnr.dll` comes from, and the RenoDX `renodx-dlss5` add-on |
| `reshade.me` | the ReShade 6.8.0 add-on installer, unpacked for `ReShade64.dll` — reshade engine only |
| `github.com/GloriousEggroll/proton-ge-custom` | the GE-Proton tarball — `--install-proton` only |

The exact URLs are the `readonly` constants under "Upstream sources" at the top
of `dlss5-linux.sh`. Read them before you run it.

`nvngx_dlssnr.dll` is leaked pre-release NVIDIA code, originally from an NBA 2K27
early-access build, patched for Ada by
[dev-camo/dlssnr-ada-patcher](https://github.com/dev-camo/dlssnr-ada-patcher).
Expect a large frame-rate cost — roughly 39% has been reported on Ada, since the
architecture genuinely lacks the throughput for the model.

**Single-player only. Never with anti-cheat.**
