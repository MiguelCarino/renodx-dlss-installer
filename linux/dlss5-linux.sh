#!/usr/bin/env bash
#
# dlss5-linux.sh - DLSS 5 "Neural Rendering" on Fedora/Linux via Proton, on RTX 40 (Ada).
#
# Linux equivalent of yumlevi/renodx-dlss-installer's Windows-only C# setup
# (reshade/setup/MainWindow.Custom.cs). Supports two injection engines:
#
#   optiscaler  Dagherbou's OptiScaler_DLSSNR fork. No ReShade layer, ~2x the
#               framerate of the ReShade route, and OptiScaler already ships a
#               setup_linux.sh. Recommended.
#   reshade     ReShade 6.8.0 (add-on build) + RenoDX renodx-dlss5 add-on +
#               the full Streamline DLL set. Faithful port of the Windows path.
#
# Both engines need the same thing from you: a D3D12 game that ALREADY runs DLSS.
# The neural pass reads the depth + motion vectors the game hands to DLSS each
# frame; with no DLSS to intercept, it does nothing.
#
# nvngx_dlssnr.dll is not distributed with this script; it is downloaded at run
# time from a third-party GitHub release. It is leaked pre-release NVIDIA code.
# Single-player only, never with anti-cheat.

set -euo pipefail

readonly VERSION="1.0.0"
readonly CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/dlss5-linux"
readonly STATE_FILE=".dlss5-linux.state"

# --- Upstream sources ---------------------------------------------------------
readonly OPTI_URL="https://github.com/Dagherbou/OptiScaler_DLSSNR/releases/download/v0.1.1.5-dlssnr/OptiScaler-DLSSNR-v0.1.1.5-dlssnr.zip"
readonly STREAMLINE_URL="https://github.com/yumlevi/renodx-dlss-installer/releases/download/latest/streamline.zip"
readonly ADDON_URL="https://github.com/yumlevi/renodx-dlss-installer/releases/download/latest/renodx-dlss5-v2.5.addon64"
readonly RESHADE_URL="https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe"
# The feeder engine: DLSS 5 for games with no DLSS of their own. Pinned, and
# fetched only from the projects' own GitHub - DLSS5-Feeder warns that lookalike
# zips (including copies of its repository) circulate with malware in them.
readonly FEEDER_VERSION="1.19.0"
readonly FEEDER_URL="https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/download/v${FEEDER_VERSION}/DLSS5-Feeder-${FEEDER_VERSION}.zip"
readonly LUMENITE_REF="f80e98d716ad72e12af6f7baa727bcfeb4be5094"
readonly LUMENITE_URL="https://codeload.github.com/umar-afzaal/LumeniteFX/zip/${LUMENITE_REF}"
readonly RESHADE_HEADERS_URL="https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders"
# Microsoft's d3dcompiler_47, as winetricks fetches it (Mozilla's fxc2 mirror,
# same pinned hash). ReShade compiles its effects through d3dcompiler_47, and
# Wine's builtin one is vkd3d-shader's HLSL front end, which rejects the
# [fastopt] ReShade emits for every loop ("E5017: Aborting due to not yet
# implemented feature: Unhandled attribute 'fastopt'") - so under Proton the
# feed and the motion-vector shader never compiled and nothing was fed.
readonly D3DCOMPILER_URL="https://raw.githubusercontent.com/mozilla/fxc2/master/dll/d3dcompiler_47.dll"
readonly D3DCOMPILER_SHA256="4432bbd1a390874f3f0a503d45cc48d346abc3a8c0213c289f4b615bf0ee84f3"
readonly GE_API="https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest"

# --- Defaults -----------------------------------------------------------------
ENGINE="optiscaler"
GAME_DIR=""
HOOK_DLL="dxgi.dll"
DO_UNINSTALL=0
DO_INSTALL_PROTON=0
DRY_RUN=0
FORCE=0
NR_SET=""
TOGGLE_KEY=""
# "Section.Key" names a reinstall carried over from the previous OptiScaler.ini,
# so the install-time defaults below know not to overwrite the user's choice.
PRESERVED=" "
UPSCALER=""
CONFIG_ONLY=0
ASSUME_YES=0
GPU_NAME=""

# --- Output -------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_RED=$'\e[31m'; C_GRN=$'\e[32m'; C_YEL=$'\e[33m'
    C_BLU=$'\e[34m'; C_BLD=$'\e[1m'; C_RST=$'\e[0m'
else
    C_RED=""; C_GRN=""; C_YEL=""; C_BLU=""; C_BLD=""; C_RST=""
fi

log()  { printf '%s==>%s %s\n' "$C_BLU" "$C_RST" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$C_GRN" "$C_RST" "$*"; }
warn() { printf '%swarn%s %s\n' "$C_YEL" "$C_RST" "$*" >&2; }
die()  { printf '%s err%s %s\n' "$C_RED" "$C_RST" "$*" >&2; exit 1; }
step() { printf '\n%s%s%s\n' "$C_BLD" "$*" "$C_RST"; }

run() {
    if (( DRY_RUN )); then
        printf '     [dry-run] %s\n' "$*"
    else
        "$@"
    fi
}

confirm() {
    (( ASSUME_YES )) && return 0
    local reply
    read -r -p "     $1 [y/N]: " reply
    [[ "${reply,,}" == y || "${reply,,}" == yes ]]
}

# The name to show in usage and hints: the packaged launchers export
# DLSS5_PROG (dlss5-linux, or "<AppImage> cli") because $0 there is the script
# file in /usr/share or the AppImage mount, which is not what the user typed.
PROG="${DLSS5_PROG:-$0}"

usage() {
    # Quoted heredoc, so nothing in it expands. Run from the repository the
    # text prints exactly as written; under a launcher the name is swapped in
    # afterwards, with \, & and the | delimiter escaped for sed.
    local name="${DLSS5_PROG:-dlss5-linux.sh}"
    name="${name//\\/\\\\}"; name="${name//&/\\&}"; name="${name//|/\\|}"
    cat <<'USAGE' | sed "s|dlss5-linux\.sh|$name|g"
dlss5-linux.sh - DLSS 5 Neural Rendering on Linux/Proton for RTX 40 (Ada)

Usage:
  dlss5-linux.sh --game-dir=PATH [options]
  dlss5-linux.sh --game-dir=PATH --probe [--json]
  dlss5-linux.sh --game-dir=PATH --uninstall
  dlss5-linux.sh --game-dir=PATH --configure --nr=on --upscaler=dlss
  dlss5-linux.sh --install-proton
  dlss5-linux.sh --check

Options:
  --game-dir=PATH     Folder holding the game's .exe. For Unreal titles this is
                      <Game>/Binaries/Win64; for Cyberpunk 2077 it is bin/x64.
                      The script auto-corrects if you point it at a launcher.
  --engine=NAME       optiscaler (default) | reshade | feeder
                      feeder: for D3D11/D3D12 games with no DLSS of their own
                      (ReShade + DLSS5-Feeder + OptiScaler DLSS-NR)
  --hook=NAME         DLL name to masquerade as: dxgi.dll (default), d3d12.dll,
                      version.dll, winmm.dll, dbghelp.dll, wininet.dll, winhttp.dll
  --install-proton    Fetch the latest GE-Proton into compatibilitytools.d
  --uninstall         Remove everything this script installed from --game-dir
  --probe             Report whether a game qualifies. Reads only, changes
                      nothing. Verdicts: supported / partial / unsupported /
                      blocked (anti-cheat) / unknown
  --json              Machine-readable output (used by the web GUI)
  --check             Run preflight checks only and exit
  --dry-run           Print what would happen, change nothing
  --force             Install even if anti-cheat is detected (ban risk)
  --nr=on|off         Set [DlssNr] Enabled in OptiScaler.ini. Neural
                      rendering ships off; this is the toggle.
  --upscaler=NAME     Set [Upscalers] Dx12Upscaler: dlss, xess, fsr21,
                      fsr22, ffx, auto. Force dlss if OptiScaler
                      mis-detects the GPU and falls back to XeSS.
  --overlay=on|off    Set [Menu] OverlayMenu, OptiScaler's in-game menu
                      (Insert). Installs default it to off: its input
                      hooks can deadlock a controller connecting during
                      startup. The ini settings above still apply.
  --toggle-key=KEY    Bind [DlssNr] ToggleKey, which switches the neural
                      pass on and off in game with the overlay off: F1-F12,
                      none, or a hex virtual-key code. Installs default to F9.
  --configure         Apply --nr / --upscaler / --overlay / --toggle-key
                      to an existing install
                      and exit, without reinstalling
  -y, --yes           Assume yes for prompts
  -h, --help          This message

Notes:
  Both engines require a D3D12 game that already runs DLSS.
  Switch engines by running --uninstall first, then reinstalling.
USAGE
    exit 0
}

# --- Argument parsing ---------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        --game-dir=*)     GAME_DIR="${arg#*=}" ;;
        --engine=*)       ENGINE="${arg#*=}" ;;
        --hook=*)         HOOK_DLL="${arg#*=}" ;;
        --install-proton) DO_INSTALL_PROTON=1 ;;
        --uninstall)      DO_UNINSTALL=1 ;;
        --probe)          DO_PROBE=1 ;;
        --json)           JSON_OUT=1 ;;
        --check)          CHECK_ONLY=1 ;;
        --dry-run)        DRY_RUN=1 ;;
        --force)          FORCE=1 ;;
        --nr=*)           NR_SET="${arg#*=}" ;;
        --upscaler=*)     UPSCALER="${arg#*=}" ;;
        --overlay=*)      OVERLAY_SET="${arg#*=}" ;;
        --toggle-key=*)   TOGGLE_KEY="${arg#*=}" ;;
        --configure)      CONFIG_ONLY=1 ;;
        -y|--yes)         ASSUME_YES=1 ;;
        -h|--help)        usage ;;
        *) die "Unknown argument: $arg (try --help)" ;;
    esac
done
CHECK_ONLY="${CHECK_ONLY:-0}"
DO_PROBE="${DO_PROBE:-0}"
JSON_OUT="${JSON_OUT:-0}"

case "$ENGINE" in
    optiscaler|reshade|feeder) ;;
    *) die "--engine must be 'optiscaler', 'reshade' or 'feeder', got '$ENGINE'" ;;
esac

# on/off is what a user types; true/false is what OptiScaler.ini wants.
case "$NR_SET" in
    "")    ;;
    on)    NR_SET=true ;;
    off)   NR_SET=false ;;
    *)     die "--nr must be 'on' or 'off', got '$NR_SET'" ;;
esac

case "${OVERLAY_SET:=}" in
    "")    ;;
    on)    OVERLAY_SET=true ;;
    off)   OVERLAY_SET=false ;;
    *)     die "--overlay must be 'on' or 'off', got '$OVERLAY_SET'" ;;
esac

# A function key by name, "none" to unbind, or a raw virtual-key code. F-keys
# map to VK_F1 (0x70) onwards; "auto" is OptiScaler's own word for unbound.
case "$TOGGLE_KEY" in
    "")              ;;
    none)            TOGGLE_KEY=auto ;;
    F[1-9]|F1[0-2])  TOGGLE_KEY=$(printf '0x%X' $(( 0x6F + ${TOGGLE_KEY#F} ))) ;;
    0x[0-9A-Fa-f]|0x[0-9A-Fa-f][0-9A-Fa-f]) ;;
    *) die "--toggle-key must be F1-F12, none, or a hex virtual-key code like 0x78" ;;
esac

case "$UPSCALER" in
    ""|dlss|xess|fsr21|fsr22|ffx|auto) ;;
    *) die "--upscaler must be one of: dlss xess fsr21 fsr22 ffx auto" ;;
esac

case "$HOOK_DLL" in
    dxgi.dll|d3d12.dll|version.dll|winmm.dll|dbghelp.dll|wininet.dll|winhttp.dll) ;;
    *) die "--hook '$HOOK_DLL' is not a supported proxy name" ;;
esac

# Both flags write OptiScaler.ini, which the reshade engine never creates.
# Catch it here rather than after a full install has already been performed.
if [[ "$ENGINE" == reshade && ( -n "$NR_SET" || -n "$UPSCALER" || -n "$OVERLAY_SET" || -n "$TOGGLE_KEY" ) ]]; then
    die "--nr / --upscaler / --overlay / --toggle-key edit OptiScaler.ini and do not apply to --engine=reshade.
     On the reshade engine, neural rendering is toggled in the ReShade overlay
     (Home > Add-ons > RenoDX DLSS5) and there is no upscaler to select."
fi

# --- Preflight ----------------------------------------------------------------

require_tools() {
    local missing=()
    for t in curl unzip objdump python3; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    if [[ "$ENGINE" == reshade || "$ENGINE" == feeder ]] && ! command -v 7z >/dev/null 2>&1; then
        missing+=("7z")
    fi
    if (( ${#missing[@]} )); then
        die "Missing tools: ${missing[*]}
     Install with: sudo dnf install curl unzip binutils python3 p7zip p7zip-plugins"
    fi
}

# The Ada-patched nvngx_dlssnr.dll contains sm_89 cubins only. Confirm the GPU
# actually is Ada, because the patched model will not run on anything else.
check_gpu() {
    command -v nvidia-smi >/dev/null 2>&1 || die "nvidia-smi not found; is the NVIDIA driver installed?"

    local name driver cc
    name=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)
    driver=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
    cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ')

    ok "GPU: $name (driver $driver, compute capability ${cc:-unknown})"
    GPU_NAME="$name"   # verify_dlssnr names the GPU when the model will not run

    if [[ "$cc" != "8.9" ]]; then
        warn "The nvngx_dlssnr.dll this downloads is Ada-patched (sm_89 cubins only)."
        warn "Compute capability $cc is not 8.9 - the model will fail to initialise."
        warn "On Blackwell (12.0) use the driver's own nvngx_dlssnr.dll instead."
    fi
}

# Proton runs NGX snippets as Windows DLLs inside Wine and bridges them to the
# native driver through nvngx.dll -> _nvngx.dll -> libnvidia-ngx.so. Without
# these, nothing DLSS-related works under Proton at all.
check_ngx_bridge() {
    local found="" d
    for d in /usr/lib64/nvidia/wine /usr/lib/nvidia/wine /usr/lib/x86_64-linux-gnu/nvidia/wine; do
        if [[ -f "$d/nvngx.dll" && -f "$d/_nvngx.dll" ]]; then
            found="$d"
            break
        fi
    done

    if [[ -n "$found" ]]; then
        ok "NGX Wine bridge: $found"
        [[ -f "$found/nvngx_dlssg.dll" ]] && ok "Driver ships nvngx_dlssg.dll (frame gen available)"
    else
        warn "nvngx.dll / _nvngx.dll not found under any nvidia/wine directory."
        warn "On Fedora install: sudo dnf install nvidia-driver-libs.i686 xorg-x11-drv-nvidia-libs"
    fi

    ls /usr/lib64/libnvidia-ngx.so.* >/dev/null 2>&1 \
        && ok "Native NGX runtime present" \
        || warn "libnvidia-ngx.so not found; DLSS will not initialise"
}

steam_root() {
    local d
    for d in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"; do
        [[ -d "$d/steamapps" ]] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

# Proton 7 predates the NGX bridging this depends on. Anything below 9 is a
# waste of time; GE-Proton is the most permissive about DLL overrides.
check_proton() {
    local root
    if ! root=$(steam_root); then
        warn "No Steam installation found."
        return
    fi

    local -a found=()
    local p
    for p in "$root"/steamapps/common/Proton* "$root"/compatibilitytools.d/*; do
        [[ -d "$p" ]] && found+=("$(basename "$p")")
    done

    if (( ! ${#found[@]} )); then
        warn "No Proton runtimes found. Run: $PROG --install-proton"
        return
    fi

    log "Proton runtimes: ${found[*]}"

    local modern=0 n
    for n in "${found[@]}"; do
        if [[ "$n" =~ GE-Proton ]]; then
            modern=1
        elif [[ "$n" =~ Proton[[:space:]]([0-9]+) ]] && (( ${BASH_REMATCH[1]} >= 9 )); then
            modern=1
        fi
    done

    if (( modern )); then
        ok "A modern Proton is available"
    else
        warn "Only old Proton found. NGX bridging needs Proton 9+ or GE-Proton."
        warn "Run: $PROG --install-proton"
    fi
}

install_proton_ge() {
    step "Installing GE-Proton"

    local root
    root=$(steam_root) || die "No Steam installation found; cannot place GE-Proton."

    # GitHub rate-limits unauthenticated callers at 60/hour, and the 403 body it
    # returns is still JSON - just without a tag_name. Both that and an outright
    # curl failure must read as an error, not as a Python traceback.
    local tag url dest
    tag=$(curl -fsSL "$GE_API" 2>/dev/null | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin)["tag_name"])
except Exception:
    pass' || true)

    [[ -n "$tag" ]] || die "Could not read the latest GE-Proton release from GitHub.
     The API is probably rate-limited (60 requests/hour per IP) or unreachable.
     Download the release manually from
     https://github.com/GloriousEggroll/proton-ge-custom/releases
     and extract it into $root/compatibilitytools.d"
    url="https://github.com/GloriousEggroll/proton-ge-custom/releases/download/${tag}/${tag}-x86_64.tar.gz"
    dest="$root/compatibilitytools.d"

    if [[ -d "$dest/$tag" ]]; then
        ok "$tag already installed at $dest/$tag"
        return
    fi

    log "Latest is $tag (~530 MB)"
    fetch "$url" "$CACHE_DIR/${tag}-x86_64.tar.gz"

    run mkdir -p "$dest"
    log "Extracting into $dest ..."
    run tar -xzf "$CACHE_DIR/${tag}-x86_64.tar.gz" -C "$dest"
    ok "$tag installed. Restart Steam, then pick it under Properties > Compatibility."
}

# --- Downloads ----------------------------------------------------------------

fetch() {
    local url="$1" dest="$2"
    mkdir -p "$(dirname "$dest")"

    if [[ -s "$dest" ]]; then
        ok "cached $(basename "$dest")"
        return
    fi

    log "Downloading $(basename "$dest") ..."
    if (( DRY_RUN )); then
        printf '     [dry-run] curl -L %s -> %s\n' "$url" "$dest"
        return
    fi

    curl -fL --retry 3 --progress-bar -o "$dest.part" "$url" \
        || die "Download failed: $url"
    mv "$dest.part" "$dest"
}

# --- Game directory resolution ------------------------------------------------

# Port of FindActualGameExecutable() from MainWindow.Custom.cs: if the folder
# holds only a launcher, look in the usual binary subdirectories for an exe that
# actually imports a graphics API.
pe_dll_names() {
    objdump -p "$1" 2>/dev/null | sed -n 's/.*DLL Name: //p'
}

# An engine that lives in its own DLL imports no graphics API into the .exe.
# Control is the clean example: Control_DX12.exe imports d3d_rmdwin10_f.dll and
# nothing else that renders, and only that DLL imports d3d12.dll. So the
# executable's imports are followed one level into DLLs sitting beside it.
#
# One level, and only siblings: it answers "what does this binary's own engine
# use" without walking into the Windows import graph, and it stays cheap enough
# to run over every .exe in a folder.
pe_imports_deep() {
    local exe="$1" dir names
    dir=$(dirname "$exe")
    names=$(pe_dll_names "$exe")

    local dll
    while IFS= read -r dll; do
        [[ -n "$dll" ]] || continue
        # Skip anything Windows ships; only the game's own DLLs are opened.
        case "${dll,,}" in
            kernel32.dll|user32.dll|advapi32.dll|version.dll|ole32.dll|shell32.dll|\
            gdi32.dll|ws2_32.dll|msvcp*|vcruntime*|api-ms-*|ucrtbase.dll|ntdll.dll) continue ;;
        esac
        # Case-insensitively, because a PE import table's spelling and the
        # filename on a case-sensitive filesystem often disagree.
        local found
        found=$(find "$dir" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null) || true
        [[ -n "$found" ]] && names+=$'\n'$(pe_dll_names "$found")
    done <<<"$names"

    printf '%s\n' "$names"
}

pe_imports_graphics() {
    pe_imports_deep "$1" \
        | grep -qixE 'd3d1[12]\.dll|d3d12core\.dll|d3d11_[0-9]\.dll|dxgi\.dll|vulkan-1\.dll|opengl32\.dll'
}

pe_imports_d3d12() {
    objdump -p "$1" 2>/dev/null \
        | grep -i 'DLL Name' \
        | grep -qiE 'd3d12|dxgi'
}

resolve_game_dir() {
    local dir="$1"
    [[ -d "$dir" ]] || die "Not a directory: $dir"

    # Already holds a graphics-API executable? Use it.
    local exe
    while IFS= read -r -d '' exe; do
        if pe_imports_graphics "$exe"; then
            printf '%s' "$dir"
            return
        fi
    done < <(find "$dir" -maxdepth 1 -iname '*.exe' -print0 2>/dev/null)

    # Otherwise walk the usual launcher-hides-the-game subdirectories.
    local sub
    # bin/x64_dx12 comes first deliberately: The Witcher 3 next-gen ships both a
    # DX11 and a DX12 binary, and only the DX12 one is usable here.
    for sub in bin/x64_dx12 bin/x64 bin/win64 bin Binaries/Win64 binaries runtime/media x64 win64 game; do
        local cand="$dir/$sub"
        [[ -d "$cand" ]] || continue
        while IFS= read -r -d '' exe; do
            local base
            base=$(basename "${exe,,}")
            [[ "$base" =~ (crash|setup|unins|launcher|redist|eac|config|helper|report|diagnostics) ]] && continue
            if pe_imports_graphics "$exe"; then
                warn "Selected folder looked like a launcher; using $cand instead"
                printf '%s' "$cand"
                return
            fi
        done < <(find "$cand" -maxdepth 1 -iname '*.exe' -print0 2>/dev/null)
    done

    # Nothing matched a known layout. Fall back to the largest executable
    # anywhere in the tree and use its directory. This is what catches RGG's
    # runtime/media, Unreal shim launchers whose real binary sits deeper, and
    # titles like Cyberpunk 2077 that import no graphics API at all because they
    # resolve D3D12 through LoadLibrary.
    # Bundled redistributables are often larger than the game binary itself
    # (Red Dead Redemption 2 ships a 127 MB Social Club installer next to an
    # 89 MB RDR2.exe), so exclude them by path as well as by name.
    local deep best="" bestsize=0 sz base depth
    for depth in 1 4; do
        best=""; bestsize=0
        while IFS= read -r -d '' deep; do
            case "${deep,,}" in
                */redistributable*|*/_commonredist/*|*/redist/*|*/directx/*|*/vcredist*) continue ;;
            esac
            base=$(basename "${deep,,}")
            if [[ "$base" =~ (crash|unins|redist|setup|touchup|installer|prelauncher|launcher|startup|errorreport|ubisoftconnect|epicgames|easyanticheat|activation|cleanup|diagnostic|vulkanrt) ]]; then
                continue
            fi
            sz=$(stat -c%s "$deep" 2>/dev/null || echo 0)
            if (( sz > bestsize )); then best="$deep"; bestsize=$sz; fi
        done < <(find "$dir" -maxdepth "$depth" -iname '*.exe' -print0 2>/dev/null)

        # 4 MB filters out helper stubs without excluding genuinely small games.
        # Depth 1 wins outright when it produces a candidate: a game binary
        # sitting in the folder we were given beats anything found deeper.
        if [[ -n "$best" ]] && (( bestsize > 4000000 )); then
            local bdir
            bdir=$(dirname "$best")
            if [[ "$bdir" != "$dir" ]]; then
                warn "Game executable found deeper in the tree; using $bdir"
            fi
            printf '%s' "$bdir"
            return
        fi
    done

    warn "No executable importing a graphics API found under $dir - continuing anyway."
    printf '%s' "$dir"
}

# The inverse of resolve_game_dir. --help tells people to pass
# <Game>/Binaries/Win64, and find only DESCENDS, so anti-cheat sitting at the
# game root is invisible from there - Halo MCC's easyanticheat/ is three levels
# above MCC-Win64-Shipping.exe. Walk UP until the PARENT is a launcher's game
# container (steamapps/common, Heroic, ...); the directory we came from is the
# game root.
#
# Ascending one level too far lands in the container itself, where every
# neighbouring game's anti-cheat would read as this game's, so the walk is
# bounded, stops dead at $HOME and /, and falls back to what it was given
# rather than guessing.
game_root_of() {
    local dir="$1" parent i

    dir=$(cd "$dir" 2>/dev/null && pwd) || { printf '%s' "$1"; return; }

    for (( i = 0; i < 6; i++ )); do
        parent=$(dirname "$dir")
        [[ "$parent" == "$dir" ]] && break

        case "$(basename "$parent")" in
            common|steamapps|compatdata|shadercache|drive_c|Heroic|Games|games|\
            "Program Files"|"Program Files (x86)"|\
            "GOG Games"|"Origin Games"|"EA Games"|"Epic Games"|"My Games")
                printf '%s' "$dir"
                return
                ;;
        esac

        [[ "$parent" == "$HOME" || "$parent" == "/" ]] && break
        dir="$parent"
    done

    printf '%s' "$1"
}

# The neural pass hooks the game's own DLSS calls. No DLSS, no effect.
check_game_preconditions() {
    local dir="$1"
    local has_d3d12=0 has_dlss=0 exe

    while IFS= read -r -d '' exe; do
        pe_imports_d3d12 "$exe" && has_d3d12=1
    done < <(find "$dir" -maxdepth 1 -iname '*.exe' -print0 2>/dev/null)

    # pipefail: find exits non-zero on an unreadable subdirectory even when it
    # printed matches, which reads back as "this game has no DLSS". Swallow it
    # the way the other find calls do.
    if { find "$dir" -maxdepth 3 \( -iname 'nvngx_dlss*.dll' -o -iname 'sl.dlss*.dll' \) 2>/dev/null || true; } | grep -q .; then
        has_dlss=1
    fi

    (( has_d3d12 )) && ok "Game imports D3D12/DXGI" \
                    || warn "No D3D12/DXGI import found - the add-on is D3D12-only."
    if (( has_dlss )); then
        ok "Game ships DLSS libraries"
    elif [[ "$ENGINE" == feeder ]]; then
        ok "No DLSS of its own - the feeder engine supplies the DLSS request"
    else
        warn "No nvngx_dlss*.dll found. If the game has no DLSS, the neural pass has nothing to hook - see --engine=feeder."
    fi
}

# --- Engine: OptiScaler -------------------------------------------------------

install_optiscaler() {
    local dir="$1"
    step "Installing OptiScaler_DLSSNR into $dir"

    fetch "$OPTI_URL" "$CACHE_DIR/optiscaler-dlssnr.zip"
    fetch "$STREAMLINE_URL" "$CACHE_DIR/streamline.zip"


    log "Extracting OptiScaler package ..."
    # The archive uses backslash separators; unzip warns but extracts correctly.
    run unzip -o -q "$CACHE_DIR/optiscaler-dlssnr.zip" -d "$dir" || true

    log "Extracting Ada-patched nvngx_dlssnr.dll ..."
    run unzip -o -j -q "$CACHE_DIR/streamline.zip" 'streamline/nvngx_dlssnr.dll' -d "$dir"

    if (( ! DRY_RUN )); then
        verify_dlssnr "$dir/nvngx_dlssnr.dll"
    fi

    log "Running OptiScaler's own setup_linux.sh (non-interactive) ..."
    if (( ! DRY_RUN )); then
        ( cd "$dir" && bash ./setup_linux.sh \
            --filename="$HOOK_DLL" \
            --overwrite=y \
            --using_nvidia=y >/dev/null )
    fi

    run rm -f "$dir/!! EXTRACT ALL FILES TO GAME FOLDER !!" "$dir/setup_windows.bat"

    restore_settings "$dir"

    write_state "$dir" optiscaler
    ok "OptiScaler installed as $HOOK_DLL"
}

# --- Engine: ReShade + RenoDX -------------------------------------------------

install_reshade() {
    local dir="$1"
    step "Installing ReShade 6.8.0 + RenoDX DLSS5 add-on into $dir"

    fetch "$RESHADE_URL" "$CACHE_DIR/ReShade_Setup_6.8.0_Addon.exe"
    fetch "$ADDON_URL" "$CACHE_DIR/renodx-dlss5-v2.5.addon64"
    fetch "$STREAMLINE_URL" "$CACHE_DIR/streamline.zip"

    log "Extracting ReShade64.dll from the installer ..."
    if (( ! DRY_RUN )); then
        rm -rf "$CACHE_DIR/reshade-extract"
        mkdir -p "$CACHE_DIR/reshade-extract"
        7z e -y -o"$CACHE_DIR/reshade-extract" \
            "$CACHE_DIR/ReShade_Setup_6.8.0_Addon.exe" ReShade64.dll >/dev/null
        [[ -f "$CACHE_DIR/reshade-extract/ReShade64.dll" ]] \
            || die "Could not extract ReShade64.dll from the installer."
        cp "$CACHE_DIR/reshade-extract/ReShade64.dll" "$dir/$HOOK_DLL"
    fi
    ok "ReShade installed as $HOOK_DLL"

    log "Installing RenoDX DLSS5 add-on ..."
    run cp "$CACHE_DIR/renodx-dlss5-v2.5.addon64" "$dir/renodx-dlss5.addon64"

    log "Extracting Streamline / DLSS DLL set ..."
    run unzip -o -j -q "$CACHE_DIR/streamline.zip" 'streamline/*' -d "$dir"

    if (( ! DRY_RUN )); then
        verify_dlssnr "$dir/nvngx_dlssnr.dll"
        write_reshade_ini "$dir"
    fi

    write_state "$dir" reshade
    ok "ReShade + RenoDX add-on installed"
}

# Mirrors SeedAddonStartDisabled() from MainWindow.Custom.cs: RenoDX resolves
# non-global settings from whichever preset section is active and falls back to
# its built-in default when the key is absent, so seed every candidate section.
# DLSS 5 for a game that has no DLSS. DLSS5-Feeder (a ReShade add-on) builds
# the DLSS request the game never makes - its frame, ReShade's depth buffer and
# motion vectors estimated by LumeniteFX - and the OptiScaler DLSS-NR fork,
# loaded under a second proxy name, answers it with real DLSS and then the
# neural pass. Proven under Proton with the feeder's own --test: 300/300
# evaluates, the neural model created, DLSS 310.x underneath, no game needed.
install_feeder() {
    local dir="$1"
    step "Installing DLSS5-Feeder $FEEDER_VERSION + OptiScaler DLSS-NR into $dir"

    fetch "$FEEDER_URL" "$CACHE_DIR/DLSS5-Feeder-${FEEDER_VERSION}.zip"
    fetch "$LUMENITE_URL" "$CACHE_DIR/LumeniteFX-${LUMENITE_REF:0:12}.zip"
    fetch "$RESHADE_URL" "$CACHE_DIR/ReShade_Setup_6.8.0_Addon.exe"
    fetch "$OPTI_URL" "$CACHE_DIR/optiscaler-dlssnr.zip"
    fetch "$STREAMLINE_URL" "$CACHE_DIR/streamline.zip"
    local h
    for h in ReShade.fxh ReShadeUI.fxh DrawText.fxh; do
        fetch "$RESHADE_HEADERS_URL/$h" "$CACHE_DIR/reshade-headers/$h"
    done
    fetch "$D3DCOMPILER_URL" "$CACHE_DIR/d3dcompiler/d3dcompiler_47.dll"
    if (( ! DRY_RUN )); then
        local sum
        sum=$(sha256sum "$CACHE_DIR/d3dcompiler/d3dcompiler_47.dll" | cut -d' ' -f1)
        [[ "$sum" == "$D3DCOMPILER_SHA256" ]] || {
            rm -f "$CACHE_DIR/d3dcompiler/d3dcompiler_47.dll"
            die "d3dcompiler_47.dll does not match the hash winetricks pins ($sum)."
        }
    fi

    # OptiScaler needs a name the game imports at start, other than the one
    # ReShade has: the feeder's own installer picks it the same way. Yakuza 0
    # imports none of winmm/version but does import dbghelp.
    local opti="" name imports
    imports=$(find "$dir" -maxdepth 1 -iname '*.exe' -print0 2>/dev/null \
        | xargs -0 -r -n1 objdump -p 2>/dev/null | sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)
    for name in winmm.dll version.dll dbghelp.dll winhttp.dll wininet.dll; do
        [[ "$name" == "${HOOK_DLL,,}" ]] && continue
        if grep -qx "$name" <<< "$imports"; then opti="$name"; break; fi
    done
    [[ -n "$opti" ]] || die "The game imports none of winmm/version/dbghelp/winhttp/wininet.dll,
     so there is no second proxy name for OptiScaler to load under."
    log "OptiScaler will load as $opti (the game imports it); ReShade as $HOOK_DLL"

    if (( DRY_RUN )); then
        printf '     [dry-run] would install ReShade, the feeder, LumeniteFX and OptiScaler into %s\n' "$dir"
        INSTALLED_HOOKS="$HOOK_DLL,$opti"
        return 0
    fi

    # ReShade with add-on support, as the first proxy.
    rm -rf "$CACHE_DIR/reshade-extract"
    mkdir -p "$CACHE_DIR/reshade-extract"
    7z e -y -o"$CACHE_DIR/reshade-extract" "$CACHE_DIR/ReShade_Setup_6.8.0_Addon.exe" ReShade64.dll >/dev/null
    [[ -f "$CACHE_DIR/reshade-extract/ReShade64.dll" ]] || die "Could not extract ReShade64.dll from the installer."
    cp "$CACHE_DIR/reshade-extract/ReShade64.dll" "$dir/$HOOK_DLL"
    ok "ReShade 6.8.0 installed as $HOOK_DLL"

    # The feeder: the add-on beside the exe, its effect with the shaders.
    local work shaders="$dir/reshade-shaders/Shaders" textures="$dir/reshade-shaders/Textures"
    work=$(mktemp -d)
    unzip -o -q "$CACHE_DIR/DLSS5-Feeder-${FEEDER_VERSION}.zip" -d "$work/feeder" 2>/dev/null || true
    mkdir -p "$shaders" "$textures"
    cp "$work/feeder/dlss5-feed.addon64" "$dir/"
    cp "$work/feeder/reshade-shaders/Shaders/DLSS5_Feed.fx" "$shaders/"
    cp "$CACHE_DIR"/reshade-headers/*.fxh "$shaders/"
    ok "DLSS5-Feeder $FEEDER_VERSION installed"

    # LumeniteFX: the motion-vector provider the feeder is tuned on.
    unzip -o -q "$CACHE_DIR/LumeniteFX-${LUMENITE_REF:0:12}.zip" -d "$work/lumenite"
    local lum
    lum=$(find "$work/lumenite" -maxdepth 1 -mindepth 1 -type d | head -1)
    # Only the kernel the preset enables: every .fx in the folder is compiled
    # at startup, and the rest of LumeniteFX is unrelated effects.
    cp "$lum/Shaders/lumenite_Kernel.fx" "$shaders/"
    [[ -d "$lum/Shaders/include" ]] && cp -r "$lum/Shaders/include" "$shaders/"
    cp "$lum/Textures/lumenite_bluenoise256.png" "$textures/"
    ok "LumeniteFX installed"
    rm -rf "$work"

    # OptiScaler DLSS-NR, the neural consumer, under the second name. Its own
    # setup_linux.sh is not run: it wants dxgi.dll, which ReShade holds.
    unzip -o -q "$CACHE_DIR/optiscaler-dlssnr.zip" -d "$dir" || true
    mv -f "$dir/OptiScaler.dll" "$dir/$opti"
    rm -f "$dir/!! EXTRACT ALL FILES TO GAME FOLDER !!" "$dir/setup_windows.bat"
    unzip -o -j -q "$CACHE_DIR/streamline.zip" 'streamline/nvngx_dlssnr.dll' -d "$dir"
    verify_dlssnr "$dir/nvngx_dlssnr.dll"

    # A DLSS runtime for the feeder's request. These games ship none; one
    # that does keeps its own, and only a copy put here is recorded as ours.
    local ours=""
    if [[ ! -f "$dir/nvngx_dlss.dll" ]]; then
        unzip -o -j -q "$CACHE_DIR/streamline.zip" 'streamline/nvngx_dlss.dll' -d "$dir"
        ours="nvngx_dlss.dll"
    fi
    # Microsoft's shader compiler, for ReShade (see D3DCOMPILER_URL). A game
    # that ships its own keeps it - that one is Microsoft's too.
    if [[ ! -f "$dir/d3dcompiler_47.dll" ]]; then
        cp "$CACHE_DIR/d3dcompiler/d3dcompiler_47.dll" "$dir/"
        ours+="${ours:+,}d3dcompiler_47.dll"
    fi
    ok "OptiScaler DLSS-NR installed as $opti"

    # The keys a bare NGX client needs, as the feeder's installer sets them.
    local kv
    for kv in DlssNr:Enabled:true DlssNr:ScanExposure:false \
              Log:LogToFile:true Log:LogLevel:2 \
              Spoofing:Dxgi:false Spoofing:StreamlineSpoofing:false \
              Inputs:EnableXeSSInputs:false Inputs:EnableFsr2Inputs:false \
              Inputs:EnableFsr3Inputs:false Inputs:EnableFfxInputs:false \
              Hotfix:CheckForUpdate:false; do
        IFS=: read -r s k v <<< "$kv"
        ini_set "$dir/OptiScaler.ini" "$s" "$k" "$v" >/dev/null || warn "[$s] $k could not be set"
    done
    restore_settings "$dir"
    write_feeder_reshade_ini "$dir"

    INSTALLED_HOOKS="$HOOK_DLL,$opti"
    write_state "$dir" feeder "$INSTALLED_HOOKS" "$ours"
    ok "Feeder engine installed: ReShade ($HOOK_DLL) + DLSS5-Feeder + OptiScaler DLSS-NR ($opti)"
}

# ReShade.ini and ReShadePreset.ini as the feeder's installer writes them: the
# add-ons beside the exe, Lumenite's kernel ahead of the feed, provider 3.
write_feeder_reshade_ini() {
    local dir="$1"
    if [[ ! -f "$dir/ReShade.ini" ]]; then
        printf '%s\r\n' '[ADDON]' 'AddonPath=.\' '' '[DEPTH]' 'DepthCopyBeforeClears=0' '' \
            '[GENERAL]' 'EffectSearchPaths=.\reshade-shaders\Shaders\**' \
            'TextureSearchPaths=.\reshade-shaders\Textures\**' 'PresetPath=.\ReShadePreset.ini' \
            'SkipLoadingDisabledEffects=0' '' '[OVERLAY]' 'TutorialProgress=4' > "$dir/ReShade.ini"
        ok "Wrote ReShade.ini"
    else
        warn "ReShade.ini already exists; leaving it alone."
    fi
    if [[ ! -f "$dir/ReShadePreset.ini" ]]; then
        printf '%s\r\n' \
            'Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx' \
            'TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Feed_Debug@DLSS5_Feed.fx' \
            '' '[DLSS5_Feed.fx]' 'DEBUG_VIEW=0' 'MV_SCALE=1.000000' 'MV_SIGN=1.000000,1.000000' \
            'PreprocessorDefinitions=DLSS5_MV_PROVIDER=3' > "$dir/ReShadePreset.ini"
        ok "Wrote ReShadePreset.ini: Lumenite kernel, then DLSS5 Feed"
    fi
}

write_reshade_ini() {
    local ini="$1/ReShade.ini"

    if [[ -f "$ini" ]]; then
        warn "ReShade.ini already exists; leaving it alone."
        return
    fi

    cat > "$ini" <<'INI'
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
PresetPath=.\ReShadePreset.ini

[ADDON]
AddonPath=.

[OVERLAY]
TutorialProgress=4

[DLSSNR.Enabled]
DLSSNR.Enabled=0

[RenoDX.DLSS5]
DLSSNR.Enabled=0

[RenoDX.DLSS5-preset1]
DLSSNR.Enabled=0

[RenoDX.DLSS5-preset2]
DLSSNR.Enabled=0

[RenoDX.DLSS5-preset3]
DLSSNR.Enabled=0
INI
    ok "Wrote ReShade.ini with neural rendering starting off"
}

# --- Shared helpers -----------------------------------------------------------

# Confirms the downloaded model really is the Ada-patched build before the user
# spends an hour wondering why the overlay says "the model would not initialise".
verify_dlssnr() {
    local f="$1"
    [[ -f "$f" ]] || die "nvngx_dlssnr.dll missing at $f"

    # The arch appears inside the embedded fatbin compile line ("-arch sm_89"),
    # not on a line of its own, so match the token anywhere.
    local arches sm89 sm120
    arches=$(strings -n 5 "$f" 2>/dev/null | grep -oE '\bsm_[0-9]{2,3}\b' || true)
    sm89=$(grep -cx 'sm_89'  <<<"$arches" || true)
    sm120=$(grep -cx 'sm_120' <<<"$arches" || true)

    if (( sm89 > 0 && sm120 == 0 )); then
        ok "nvngx_dlssnr.dll is Ada-patched ($sm89 sm_89 cubins, no sm_120)"
    elif (( sm120 > 0 && sm89 == 0 )); then
        warn "nvngx_dlssnr.dll is the stock Blackwell build (sm_120 only)."
        warn "It will NOT run on ${GPU_NAME:-this Ada GPU}. Patch it with dev-camo/dlssnr-ada-patcher."
    else
        warn "Could not classify nvngx_dlssnr.dll (sm_89=$sm89 sm_120=$sm120)"
    fi
}

# hook= is the proxy DLL, or for the feeder engine a comma list of both
# (ReShade's and OptiScaler's): uninstall removes every one and the launch
# options override every one. ours= lists extra top-level files that engine
# put down and that a game might otherwise ship itself.
write_state() {
    (( DRY_RUN )) && return 0
    printf 'engine=%s\nhook=%s\ninstalled=%s\n' \
        "$2" "${3:-$HOOK_DLL}" "$(date -Iseconds)" > "$1/$STATE_FILE"
    [[ -n "${4:-}" ]] && printf 'ours=%s\n' "$4" >> "$1/$STATE_FILE"
    return 0
}

# "dxgi.dll,dbghelp.dll" -> "dxgi=n,b;dbghelp=n,b"
dll_overrides() {
    local list="$1" out="" name
    IFS=',' read -r -a names <<< "$list"
    for name in "${names[@]}"; do
        [[ -n "$name" ]] && out+="${out:+;}${name%.dll}=n,b"
    done
    printf '%s' "$out"
}

print_launch_options() {
    local hooks="${INSTALLED_HOOKS:-$HOOK_DLL}"
    local hook="${hooks//.dll/}" overrides
    overrides=$(dll_overrides "$hooks")
    # The feeder engine's ReShade needs Microsoft's compiler, not Wine's.
    [[ "$ENGINE" == feeder ]] && overrides+=";d3dcompiler_47=n,b"

    step "Steam launch options"
    cat <<EOF

Set these under the game's Properties > General > Launch Options:

  ${C_BLD}WINEDLLOVERRIDES="${overrides}" PROTON_ENABLE_NVAPI=1 PROTON_ENABLE_NGX_UPDATER=1 VKD3D_CONFIG=dxr11 PROTON_HIDE_NVIDIA_GPU=0 %command%${C_RST}

What each one does:
  WINEDLLOVERRIDES  load '${hook}' (our proxy DLL) before the builtin. Required.
  NVAPI             expose DXVK-NVAPI so the game is offered DLSS at all.
  NGX_UPDATER       let Proton manage nvngx_dlss/dlssg/dlssd.
                    It does not touch nvngx_dlssnr.dll, so our model survives.
  VKD3D_CONFIG      enable DXR in vkd3d-proton, needed by most DLSS+RT titles.
  HIDE_NVIDIA_GPU=0 do not mask the GPU from the game.

Also set Compatibility to a modern Proton (GE-Proton preferred, Proton 9+ minimum).

EOF

    if [[ "$ENGINE" == feeder ]]; then
        cat <<'EOF'
In game: the feed starts on its own (ReShadePreset.ini enables LUMENITE: Kernel
and DLSS 5 Feed). Home opens ReShade's overlay to check them; F9 switches the
neural pass. dlss5-feed.log beside the exe should say "frame N delivered".
Turn the game's own MSAA/SSAA off. Expect ghosting on fast motion: the motion
vectors are estimated from the picture, not taken from the game.
EOF
    elif [[ "$ENGINE" == optiscaler ]]; then
        cat <<'EOF'
Neural rendering starts as OptiScaler.ini says: --configure --nr=on, or the
checkbox in dlss5-gui.py. In game, F9 switches it on and off (--toggle-key).
The overlay (Insert) is off by default - its input hooks can freeze a game
when a controller connects at startup. Want it anyway: --configure
--overlay=on, then Insert > DLSS Neural Rendering for the live sliders.
A reinstall keeps every setting you changed in OptiScaler.ini.
EOF
    else
        cat <<'EOF'
In game: press Home to open the ReShade overlay, go to the Add-ons tab, and
enable "Enable DLSS Neural Rendering" under RenoDX DLSS5. It starts off.
EOF
    fi
}

do_uninstall() {
    local dir="$1"
    step "Uninstalling from $dir"

    local hook="$HOOK_DLL" engine=""
    if [[ -f "$dir/$STATE_FILE" ]]; then
        # pipefail turns a state file with no hook= line into a silent exit 1
        # that removes nothing, and an empty hook= makes "$dir/$hook" the game
        # directory itself. Neither may reach the rm loop below.
        hook=$(grep '^hook=' "$dir/$STATE_FILE" | cut -d= -f2 || true)
        [[ -n "$hook" ]] || die "$dir/$STATE_FILE has no usable 'hook=' line, so the
     proxy DLL this install left behind cannot be identified. Delete that file
     and re-run --uninstall with the --hook=NAME you installed with, or remove
     the proxy DLL by hand."
        log "State file says the proxy is $hook"
        engine=$(grep '^engine=' "$dir/$STATE_FILE" | cut -d= -f2 || true)
    fi

    # Only names an engine of ours actually writes to the top level, which is
    # the same split installed_by_us() makes - it is the removal loop that
    # never asked. libxess*, libxell.dll and amd_fidelityfx_* were in here and
    # are in no engine's output: the OptiScaler package keeps its runtime
    # libraries in OptiScaler/ (OptiDllPath in OptiScaler.ini defaults to
    # .\OptiScaler) and streamline.zip ships neither, so a copy at the top
    # level is always the game's. Removing them cost Resident Evil Requiem
    # four of re9.exe's static imports and left it exiting on c0000135
    # before it drew a frame. The OptiScaler directory below clears ours.
    local -a hooks
    IFS=',' read -r -a hooks <<< "$hook"
    local -a targets=(
        "${hooks[@]}" "$STATE_FILE"
        OptiScaler.ini OptiScaler.log OptiScaler.dll "nvngx.dll_dlssnr.dll"
        remove_optiscaler.sh setup_linux.sh "READ ME - DLSS Neural Rendering.txt"
        ReShade.ini ReShade.log ReShadePreset.ini
        renodx-dlss5.addon64
        nvngx_dlssnr.dll
    )

    # install_reshade unpacks the whole streamline set to the top level;
    # install_optiscaler takes only nvngx_dlssnr.dll out of it and leaves the
    # rest. So these are ours under reshade and the game's under optiscaler -
    # Control ships its own nvngx_dlss.dll and would lose it otherwise.
    if [[ "$engine" == feeder ]]; then
        targets+=(dlss5-feed.addon64 dlss5-feed.log dlss5-feed.cfg setup_windows.bat)
        # nvngx_dlss.dll only when this install supplied it; a game with its
        # own keeps it.
        local ours
        ours=$(grep '^ours=' "$dir/$STATE_FILE" 2>/dev/null | cut -d= -f2 || true)
        [[ -n "$ours" ]] && IFS=',' read -r -a extra <<< "$ours" && targets+=("${extra[@]}")
    fi

    if [[ "$engine" == reshade ]]; then
        targets+=(
            nvngx_dlss.dll nvngx_dlssg.dll
            sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.dlss_nr.dll
            sl.interposer.dll sl.nis.dll sl.pcl.dll sl.reflex.dll
            nis.license.txt nvngx_dlss.license.txt reflex.license.txt
        )
    fi

    local t removed=0
    for t in "${targets[@]}"; do
        if [[ -e "$dir/$t" ]]; then
            run rm -f "$dir/$t"
            (( ++removed ))
        fi
    done

    local d
    for d in Licenses OptiScaler D3D12_OptiScaler DlssOverrides dlssnr-capture reshade-shaders; do
        if [[ -d "$dir/$d" ]]; then
            run rm -rf "$dir/$d"
            (( ++removed ))
        fi
    done

    if (( DRY_RUN )); then
        ok "Would remove $removed items."
    else
        ok "Removed $removed items. Clear the Steam launch options too."
    fi
}

# --- OptiScaler.ini ------------------------------------------------------------
#
# OptiScaler.ini has several keys literally named "Enabled=auto" in different
# sections, so edits must be section-aware. sed would happily hit the wrong one.

ini_set() {
    local file="$1" section="$2" key="$3" value="$4" result

    if [[ ! -f "$file" ]]; then
        warn "OptiScaler.ini not found at $file"
        return 1
    fi

    # The Python below opens the file "w", so --dry-run has to stop short of
    # calling it at all rather than relying on run().
    if (( DRY_RUN )); then
        printf '     [dry-run] set [%s] %s = %s in %s\n' "$section" "$key" "$value" "$file"
        return 0
    fi

    result=$(python3 - "$file" "$section" "$key" "$value" <<'PY'
import re, sys

path, section, key, value = sys.argv[1:5]

# newline="" disables universal-newline translation. OptiScaler.ini is a CRLF
# file with a UTF-8 BOM; without this, writing it back rewrites all 1600 lines
# to LF, which is a pointless whole-file diff at best.
with open(path, encoding="utf-8", errors="replace", newline="") as handle:
    lines = handle.read().split("\n")

current, done, last_in_section = None, False, None
for i, line in enumerate(lines):
    # The BOM lands on line 0, which is a section header here; leaving it on
    # makes startswith("[") fail and every edit silently no-op.
    stripped = line.strip().lstrip("\ufeff")
    if stripped.startswith("[") and stripped.endswith("]"):
        current = stripped[1:-1]
        continue
    if current == section and "=" in stripped and not stripped.startswith(";"):
        last_in_section = i
    if current == section and re.match(r"\s*%s\s*=" % re.escape(key), line):
        eol = "\r" if line.endswith("\r") else ""
        lines[i] = "%s=%s%s" % (key, value, eol)
        done = True
        break

# A key the shipped template leaves out of its section - [DlssNr] ToggleKey,
# which the template files under [FrameGen] although OptiScaler only reads it
# from [DlssNr] - is added after the section's last setting.
if not done and last_in_section is not None:
    eol = "\r" if lines[last_in_section].endswith("\r") else ""
    lines.insert(last_in_section + 1, "%s=%s%s" % (key, value, eol))
    done = True

if done:
    with open(path, "w", encoding="utf-8", errors="replace", newline="") as handle:
        handle.write("\n".join(lines))

print("ok" if done else "missing")
PY
)

    if [[ "$result" == ok ]]; then
        ok "[$section] $key = $value"
    else
        warn "[$section] $key not found in $(basename "$file")"
        return 1
    fi
}

# Every key moved off "auto", as "Section<TAB>Key<TAB>Value" lines.
ini_snapshot() {
    python3 - "$1" <<'PY'
import sys
current = None
with open(sys.argv[1], encoding="utf-8", errors="replace") as handle:
    for line in handle:
        s = line.strip().lstrip("\ufeff")
        if s.startswith("[") and s.endswith("]"):
            current = s[1:-1]
        elif current and "=" in s and not s.startswith(";"):
            key, value = s.split("=", 1)
            if value.strip().lower() != "auto":
                print("%s\t%s\t%s" % (current, key.strip(), value.strip()))
PY
}

# The package ships its own OptiScaler.ini, so a reinstall used to hand back
# neural rendering switched off and every other choice reset - Control came
# back "switched off" right after a Reinstall. main() reads out everything the
# user moved off "auto" before anything is touched - before the pre-install
# uninstall too, which deletes the file - and the engines write it back here.
SAVED_INI=""
snapshot_settings() {
    local ini="$1/OptiScaler.ini"
    [[ -f "$ini" ]] && (( ! DRY_RUN )) || return 0
    SAVED_INI="$(mktemp)"
    ini_snapshot "$ini" > "$SAVED_INI"
}

restore_settings() {
    [[ -n "$SAVED_INI" && -f "$SAVED_INI" && -f "$1/OptiScaler.ini" ]] || return 0
    ini_restore "$1/OptiScaler.ini" "$SAVED_INI"
    rm -f "$SAVED_INI"
    SAVED_INI=""
}

# Write a snapshot back, recording what was carried over in PRESERVED.
ini_restore() {
    local ini="$1" saved="$2" section key value count=0
    while IFS=$'\t' read -r section key value; do
        [[ -n "$key" ]] || continue
        if ini_set "$ini" "$section" "$key" "$value" >/dev/null; then
            PRESERVED+="$section.$key "
            count=$(( count + 1 ))
        fi
    done < "$saved"
    (( count )) && ok "Kept $count setting(s) from the previous OptiScaler.ini"
    return 0
}

apply_ini_settings() {
    # Two statements deliberately: bash expands every word of a `local` command
    # before the builtin runs, so `local dir="$1" ini="$dir/..."` would expand
    # $dir against the unset global and trip set -u.
    local dir="$1"
    local ini="$dir/OptiScaler.ini"

    [[ -z "$NR_SET" && -z "$UPSCALER" && -z "$OVERLAY_SET" && -z "$TOGGLE_KEY" ]] && return 0

    step "Configuring OptiScaler.ini"

    # Under --dry-run the install never extracted the package, so a missing
    # OptiScaler.ini here is the expected state, not a failure.
    if [[ ! -f "$ini" ]]; then
        if (( DRY_RUN )); then
            printf '     [dry-run] would edit %s\n' "$ini"
            return 0
        fi
        die "No OptiScaler.ini in $dir - install the optiscaler engine first."
    fi

    local rc=0
    if [[ -n "$NR_SET" ]]; then
        ini_set "$ini" DlssNr Enabled "$NR_SET" || rc=1
    fi
    if [[ -n "$UPSCALER" ]]; then
        # OptiScaler falls back to XeSS whenever it decides the GPU cannot do
        # DLSS, which under Proton it can get wrong: its NVAPI probe runs before
        # the game loads nvapi64.dll. Forcing the key sidesteps the detection.
        ini_set "$ini" Upscalers Dx12Upscaler "$UPSCALER" || rc=1
    fi
    if [[ -n "$OVERLAY_SET" ]]; then
        ini_set "$ini" Menu OverlayMenu "$OVERLAY_SET" || rc=1
    fi
    if [[ -n "$TOGGLE_KEY" ]]; then
        ini_set "$ini" DlssNr ToggleKey "$TOGGLE_KEY" || rc=1
    fi
    return $rc
}

# --- Probe --------------------------------------------------------------------
#
# Answers "will this game work?" without touching the game folder. The rule the
# neural pass actually imposes is not "has DLSS" but "has a temporal upscaler
# whose inputs OptiScaler can reach, on a D3D12 path" - and, for anything we are
# willing to inject into, no anti-cheat.

P_DIR=""; P_EXE=""; P_API="unknown"; P_UPSCALERS=""; P_ANTICHEAT=""
P_ENGINE=""; P_HOOK=""; P_VERDICT=""; P_HINT=""
declare -a P_REASONS=()

# Import tables are the reliable signal, but plenty of modern titles resolve
# d3d12.dll through LoadLibrary and import nothing but dxgi (or nothing at all),
# so fall back to scanning the executable for the loader strings.
detect_api_for() {
    local exe="$1" imports own strs
    # Deep, so a game whose renderer sits in a companion DLL is read correctly.
    # It also keeps the two binaries of a dual-API title apart: Control_DX12.exe
    # reaches d3d12.dll through d3d_rmdwin10_f.dll while Control_DX11.exe reaches
    # d3d11.dll through d3d_rmdwin7_f.dll, which scanning the folder would blur.
    imports=$(pe_imports_deep "$exe" 2>/dev/null || true)
    own=$(pe_dll_names "$exe" 2>/dev/null || true)

    # Only a Direct3D import is trustworthy on its own. An opengl32 or vulkan-1
    # import proves nothing - Ubisoft titles import opengl32 for the overlay
    # while rendering through D3D11 loaded via LoadLibrary - so the string scan
    # has to run before those are believed.
    # Anchored, because the deep scan lists the game's own DLL names too and a
    # substring test reads "d3d_rmdwin10_f.dll" or "d3d11on12.dll" as an answer.
    # D3D12Core.dll is the Agility SDK runtime and means D3D12 just as plainly.
    if grep -qixE 'd3d12\.dll|d3d12core\.dll' <<<"$imports"; then printf 'd3d12'; return; fi

    strs=$(strings -n 8 "$exe" 2>/dev/null \
        | grep -oiE 'D3D12CreateDevice|d3d12\.dll|D3D11CreateDevice|d3d11\.dll|vkCreateInstance' \
        | sort -u || true)

    # A d3d11 import does NOT settle it, and this is where most of the wrong
    # answers came from. Unreal imports d3d11.dll statically and reaches D3D12
    # through LoadLibrary, and RE Engine does the same; both were reading as
    # d3d11 and so as "partial" when they run D3D12 perfectly well.
    #
    # D3D12CreateDevice is the discriminator, not the string "d3d12": a renderer
    # that actually creates a D3D12 device must resolve that symbol by name.
    # Verified to be absent from genuinely D3D11 titles (Alien Isolation,
    # Borderlands 2) and present in Palworld and Resident Evil 4.
    if grep -qixE 'd3d11\.dll|d3d11_[0-9]\.dll' <<<"$imports"; then
        grep -qi 'D3D12CreateDevice' <<<"$strs" && { printf 'd3d12'; return; }
        printf 'd3d11'; return
    fi

    if grep -qi 'd3d12' <<<"$strs"; then printf 'd3d12'; return; fi
    if grep -qi 'd3d11' <<<"$strs"; then printf 'd3d11'; return; fi

    # These two are read from the executable's OWN imports, never the deep scan.
    # A Direct3D import is worth believing wherever it is found, but OpenGL and
    # Vulkan are not: Unity's stub launcher imports nothing at all while
    # UnityPlayer.dll beside it imports OPENGL32, which had every Unity game in
    # the library reporting "opengl" for a renderer that is really D3D11.
    # Falling through to "unknown" is the honest answer for a stub like that.
    if grep -qixE 'vulkan-1\.dll' <<<"$own" || grep -qi 'vkCreateInstance' <<<"$strs"; then
        printf 'vulkan'; return
    fi
    if grep -qixE 'opengl32\.dll' <<<"$own"; then printf 'opengl'; return; fi
    if grep -qixE 'dxgi\.dll' <<<"$imports"; then printf 'd3d11'; return; fi
    printf 'unknown'
}

# Whether an upscaler DLL found under the game root is one this script put
# there. It has to be exact in both directions: counting a game's own DLL as
# ours makes a supported game probe as unsupported, and counting ours as the
# game's makes an unsupported game probe as supported once it has been
# installed into.
#
# Two things decide it - which directory the file is in, and which engine the
# state file says was installed.
installed_by_us() {
    local path="$1" dir base engine owned=0

    dir=$(dirname "$path")
    # Directories the package creates itself. Anything inside one is ours by
    # construction, whatever it is called - the game did not put it there.
    # Peeling them also gets back to the directory the state file sits in.
    while :; do
        case "$(basename "$dir")" in
            Licenses|OptiScaler|D3D12_OptiScaler|DlssOverrides|dlssnr-capture)
                owned=1; dir=$(dirname "$dir") ;;
            *) break ;;
        esac
    done

    [[ -f "$dir/$STATE_FILE" ]] || return 1
    (( owned )) && return 0

    # At the top level, only the names the installed engine actually drops
    # there. The two engines differ: install_optiscaler takes nvngx_dlssnr.dll
    # out of streamline.zip and leaves the rest of it, while install_reshade
    # unpacks the whole set. Treating nvngx_dlss.dll as ours either way is what
    # made Control - which ships its own, and was installed with the optiscaler
    # engine - report "no upscaler runtime found" and probe as unsupported.
    engine=$(grep '^engine=' "$dir/$STATE_FILE" 2>/dev/null | cut -d= -f2 || true)
    base=$(basename "${path,,}")

    case "$base" in
        nvngx_dlssnr.dll|nvngx.dll_dlssnr.dll) return 0 ;;
    esac

    if [[ "$engine" == feeder ]]; then
        local ours
        ours=$(grep '^ours=' "$dir/$STATE_FILE" 2>/dev/null | cut -d= -f2 || true)
        [[ ",${ours,,}," == *",$base,"* ]] && return 0
    fi

    if [[ "$engine" == reshade ]]; then
        case "$base" in
            nvngx_dlss.dll|nvngx_dlssg.dll) return 0 ;;
            sl.common.dll|sl.dlss.dll|sl.dlss_g.dll|sl.dlss_nr.dll)  return 0 ;;
            sl.interposer.dll|sl.nis.dll|sl.pcl.dll|sl.reflex.dll)   return 0 ;;
        esac
    fi

    return 1
}

probe_scan() {
    # $1 is the resolved binary directory, $2 whatever the caller was given. The
    # two differ constantly (Cyberpunk's bin/x64, Halo's MCC/Binaries/Win64) and
    # anti-cheat lives at the game ROOT, which is above both - so ascend to it
    # here rather than trusting either caller to have done so. Scanning only the
    # directory we were handed is exactly the check we cannot afford to get wrong.
    local dir="$1" root
    root=$(game_root_of "${2:-$1}")
    P_DIR="$dir"; P_EXE=""; P_API="unknown"; P_UPSCALERS=""; P_ANTICHEAT=""
    P_ENGINE=""; P_HOOK=""; P_HINT=""; P_REASONS=()

    # Biggest executable that touches a graphics API is the game. Plenty of
    # titles import nothing and resolve everything through LoadLibrary, so keep
    # the biggest plausible executable as a fallback rather than giving up.
    # A title shipping both a D3D11 and a D3D12 binary is the case size alone
    # gets wrong: Control's two differ by 512 bytes and only the D3D12 one can
    # be ridden, so a D3D12 candidate outranks a larger D3D11 one outright.
    local exe best="" bestsize=0 bestd3d12=0 fallback="" fallbacksize=0 sz any=0 is12
    while IFS= read -r -d '' exe; do
        any=1
        local base; base=$(basename "${exe,,}")
        if [[ "$base" =~ (crash|unins|redist|vc_redist|dxsetup|touchup|installer|ubisoftconnect|epicgames|easyanticheat|activation|cleanup|diagnostic) ]]; then
            continue
        fi
        sz=$(stat -c%s "$exe" 2>/dev/null || echo 0)
        if pe_imports_graphics "$exe"; then
            is12=0; [[ "$(detect_api_for "$exe")" == d3d12 ]] && is12=1
            if (( is12 > bestd3d12 )) || { (( is12 == bestd3d12 )) && (( sz > bestsize )); }; then
                best="$exe"; bestsize=$sz; bestd3d12=$is12
            fi
        elif (( sz > fallbacksize )); then
            fallback="$exe"; fallbacksize=$sz
        fi
    done < <(find "$dir" -maxdepth 1 -iname '*.exe' -print0 2>/dev/null)

    if [[ -z "$best" && -n "$fallback" ]]; then
        best="$fallback"
    fi

    if [[ -n "$best" ]]; then
        P_EXE=$(basename "$best")
        P_API=$(detect_api_for "$best")
    elif (( any == 0 )); then
        # No Windows executable anywhere: almost certainly a native Linux build.
        if find "$dir" -maxdepth 3 -iname '*.exe' -print -quit 2>/dev/null | grep -q .; then
            P_REASONS+=("No executable in this folder; the game binary may be in a subdirectory.")
        elif find "$dir" -type f -print -quit 2>/dev/null | grep -q .; then
            # Files, but no Windows executable anywhere: a native Linux build.
            P_API="native"
        else
            # Nothing at all. Steam leaves the directory behind after an
            # uninstall, and calling that a native Linux build is just wrong.
            P_REASONS+=("This folder is empty - the game does not look installed.")
        fi
    else
        P_REASONS+=("No usable executable found in this folder.")
    fi

    # Upscaler runtimes. Engine-integrated FSR2 (common in Unreal) ships no DLL,
    # so absence here is suggestive, not conclusive. Our own drops are filtered
    # out: counting them means an unsupported game starts probing as supported
    # the moment it has been installed into once.
    local hits="" path ours=0
    while IFS= read -r path; do
        if installed_by_us "$path"; then ours=1; continue; fi
        hits+="$(basename "$path")"$'\n'
    # No depth limit. Unreal buries upscalers as plugins, and the depth is not a
    # number worth guessing at: Palworld's sits at
    # Pal/Plugins/DLSS/Binaries/ThirdParty/Win64 (7 levels), Expedition 33 adds a
    # vendor folder at Sandfall/Plugins/NVIDIA/DLSS/... (8), and a cap fitted to
    # one is wrong for the next. Both probed as having no upscaler at all.
    #
    # It costs nothing to stop capping it. Modern games pack their assets into a
    # handful of archives, so these trees are tiny: 181 files for Expedition 33,
    # 123 for Red Dead Redemption 2, and an unbounded scan of the largest tree
    # in the library (Left 4 Dead 2, 59,329 files) takes 0.05s.
    done < <(find "$root" \( \
            -iname 'nvngx_dlss.dll' -o -iname 'sl.dlss.dll' -o -iname '*_DLSS*.dll' \
            -o -iname 'libxess.dll' -o -iname 'libxess_dx11.dll' \
            -o -iname 'ffx_fsr2*.dll' -o -iname 'amd_fidelityfx*.dll' -o -iname 'ffx_fsr3*.dll' \
        \) -print 2>/dev/null | sort -u || true)

    grep -qiE 'dlss'            <<<"$hits" && P_UPSCALERS+="dlss "
    grep -qiE 'libxess'         <<<"$hits" && P_UPSCALERS+="xess "
    grep -qiE 'fsr|fidelityfx'  <<<"$hits" && P_UPSCALERS+="fsr "
    P_UPSCALERS="${P_UPSCALERS% }"

    if (( ours )) && [[ -z "$P_UPSCALERS" ]]; then
        P_REASONS+=("Upscaler DLLs installed here by this script were ignored; only the game's own count.")
    fi

    # Anti-cheat. Presence of any of these means we do not touch the folder.
    local ac
    ac=$(find "$root" -maxdepth 5 \( \
            -iname 'EasyAntiCheat*' -o -iname 'easyanticheat*' \
            -o -iname 'BEService*' -o -iname 'BEClient*' -o -iname 'BattlEye*' \
            -o -iname 'vanguard*' -o -iname 'anticheat*' -o -iname 'GameGuard*' \
        \) -printf '%f\n' 2>/dev/null | sort -u | head -4 || true)
    if [[ -n "$ac" ]]; then
        P_ANTICHEAT=$(tr '\n' ',' <<<"$ac" | sed 's/,$//')
    fi

    # Existing install by this script
    if [[ -f "$dir/$STATE_FILE" ]]; then
        P_ENGINE=$(grep '^engine=' "$dir/$STATE_FILE" | cut -d= -f2 || true)
        P_HOOK=$(grep '^hook=' "$dir/$STATE_FILE" | cut -d= -f2 || true)
    fi

    # A game shipping its own dxgi.dll proxy (REFramework, ENB, mod loaders)
    # would collide with ours, so steer to version.dll in that case.
    if [[ -f "$dir/dxgi.dll" && ",$P_HOOK," != *",dxgi.dll,"* ]]; then
        P_HINT="version.dll"
        P_REASONS+=("A dxgi.dll already exists here (mod loader?) - use --hook=version.dll.")
    else
        P_HINT="dxgi.dll"
    fi

    probe_verdict
}

probe_verdict() {
    if [[ -n "$P_ANTICHEAT" ]]; then
        P_VERDICT="blocked"
        P_REASONS+=("Anti-cheat present ($P_ANTICHEAT). Injecting a proxy DLL risks a ban.")
        return
    fi

    if [[ "$P_API" == native ]]; then
        P_VERDICT="unsupported"
        P_REASONS+=("This is a native Linux build - there is no Windows executable to inject a proxy DLL into.")
        return
    fi

    if [[ -z "$P_UPSCALERS" ]]; then
        # No DLSS to ride, but a D3D11/D3D12 game can still get the pass from
        # the feeder engine, which builds the DLSS request itself.
        if [[ "$P_API" == d3d11 || "$P_API" == d3d12 ]]; then
            P_VERDICT="feeder"
            P_REASONS+=("No upscaler of its own - the optiscaler engine has nothing to hook here.")
            P_REASONS+=("Install with the feeder engine: it builds the DLSS request itself from ReShade's depth buffer and estimated motion. Expect ghosting on fast motion and roughly half the frame rate.")
            return
        fi
        P_VERDICT="unsupported"
        P_REASONS+=("No upscaler runtime found. The neural pass borrows depth and motion vectors from an existing temporal upscaler; with none, it has nothing to hook.")
        P_REASONS+=("If the game has an engine-integrated FSR2 with no separate DLL, this check cannot see it - try anyway.")
        return
    fi

    case "$P_API" in
        d3d12)
            P_VERDICT="supported"
            P_REASONS+=("D3D12 with $P_UPSCALERS - this is the supported configuration.")
            ;;
        d3d11)
            P_VERDICT="partial"
            P_REASONS+=("D3D11: the game's own DLSS cannot be ridden. OptiScaler must replace it with the FSR or XeSS 'w/Dx12' backend and the pass rides that bridge.")
            ;;
        vulkan)
            P_VERDICT="unsupported"
            P_REASONS+=("Vulkan is not wired up in the DLSSNR pass yet.")
            ;;
        *)
            P_VERDICT="unknown"
            P_REASONS+=("Could not determine the graphics API. Install and see.")
            ;;
    esac
}

probe_report_text() {
    local badge
    case "$P_VERDICT" in
        supported)   badge="${C_GRN}SUPPORTED${C_RST}" ;;
        partial)     badge="${C_YEL}PARTIAL${C_RST}" ;;
        feeder)      badge="${C_YEL}FEEDER (no DLSS - use --engine=feeder)${C_RST}" ;;
        blocked)     badge="${C_RED}BLOCKED${C_RST}" ;;
        unsupported) badge="${C_RED}UNSUPPORTED${C_RST}" ;;
        *)           badge="${C_YEL}UNKNOWN${C_RST}" ;;
    esac

    step "Probe: $P_DIR"
    printf '  verdict    %b\n' "$badge"
    printf '  executable %s\n' "${P_EXE:-none found}"
    printf '  api        %s\n' "$P_API"
    printf '  upscalers  %s\n' "${P_UPSCALERS:-none detected}"
    printf '  anticheat  %s\n' "${P_ANTICHEAT:-none detected}"
    if [[ -n "$P_ENGINE" ]]; then
        printf '  installed  %s (as %s)\n' "$P_ENGINE" "$P_HOOK"
    else
        printf '  installed  no\n'
    fi
    printf '  hook       %s\n' "$P_HINT"

    printf '\n'
    local r
    for r in "${P_REASONS[@]}"; do
        printf '  - %s\n' "$r"
    done
    printf '\n'
}

probe_report_json() {
    python3 - "$P_DIR" "$P_EXE" "$P_API" "$P_UPSCALERS" "$P_ANTICHEAT" \
                "$P_ENGINE" "$P_HOOK" "$P_VERDICT" "$P_HINT" "${P_REASONS[@]}" <<'PY'
import json, sys
a = sys.argv[1:]
print(json.dumps({
    "dir":        a[0],
    "executable": a[1] or None,
    "api":        a[2],
    "upscalers":  a[3].split() if a[3] else [],
    "anticheat":  a[4].split(",") if a[4] else [],
    "installed":  {"engine": a[5], "hook": a[6]} if a[5] else None,
    "verdict":    a[7],
    "hook":       a[8],
    "reasons":    a[9:],
}, indent=2))
PY
}

do_probe() {
    local root="$1" dir
    dir="$(resolve_game_dir "$root" 2>/dev/null)"
    probe_scan "$dir" "$root"
    if (( JSON_OUT )); then
        probe_report_json
    else
        probe_report_text
    fi
}

# --- Main ---------------------------------------------------------------------

main() {
    # Probe short-circuits everything: it reads only, needs no preflight, and in
    # --json mode must emit nothing but JSON.
    if (( DO_PROBE )); then
        (( JSON_OUT )) && { C_RED=""; C_GRN=""; C_YEL=""; C_BLU=""; C_BLD=""; C_RST=""; }
        [[ -n "$GAME_DIR" ]] || die "--probe needs --game-dir"
        GAME_DIR="$(cd "$GAME_DIR" 2>/dev/null && pwd)" || die "Cannot resolve --game-dir"
        do_probe "$GAME_DIR"
        exit 0
    fi

    printf '%sdlss5-linux.sh v%s%s - DLSS 5 Neural Rendering on Linux/Proton\n' \
        "$C_BLD" "$VERSION" "$C_RST"

    require_tools
    mkdir -p "$CACHE_DIR"

    if (( DO_INSTALL_PROTON )); then
        install_proton_ge
        [[ -z "$GAME_DIR" ]] && exit 0
    fi

    step "Preflight"
    check_gpu
    check_ngx_bridge
    check_proton

    if (( CHECK_ONLY )); then
        printf '\n'
        ok "Preflight complete."
        exit 0
    fi

    [[ -n "$GAME_DIR" ]] || die "--game-dir is required (try --help)"
    GAME_DIR="$(cd "$GAME_DIR" 2>/dev/null && pwd)" || die "Cannot resolve --game-dir"

    if (( DO_UNINSTALL )); then
        # The install lives in the resolved directory, not necessarily the one
        # given. Trust an explicit path that already holds a state file, else
        # resolve the way the installer did.
        if [[ ! -f "$GAME_DIR/$STATE_FILE" ]]; then
            GAME_DIR="$(resolve_game_dir "$GAME_DIR" 2>/dev/null)"
        fi
        do_uninstall "$GAME_DIR"
        exit 0
    fi

    if (( CONFIG_ONLY )); then
        apply_ini_settings "$(resolve_game_dir "$GAME_DIR" 2>/dev/null)" || exit 1
        exit 0
    fi

    step "Resolving game directory"
    local game_root="$GAME_DIR"
    GAME_DIR="$(resolve_game_dir "$GAME_DIR")"
    ok "Target: $GAME_DIR"

    # Refuse outright on anti-cheat rather than leaving it to the user to notice
    # a warning. Scanned from the original root, where the anti-cheat lives.
    probe_scan "$GAME_DIR" "$game_root"
    if [[ -n "$P_ANTICHEAT" ]] && (( ! FORCE )); then
        die "Anti-cheat detected: $P_ANTICHEAT

     Injecting a proxy DLL into an anti-cheat protected game risks a ban, and
     the nvngx_dlssnr.dll this downloads is leaked pre-release NVIDIA code.
     Refusing.

     Override with --force if you are certain this title is single-player only."
    fi

    check_game_preconditions "$GAME_DIR"

    snapshot_settings "$GAME_DIR"

    if [[ -f "$GAME_DIR/$STATE_FILE" ]]; then
        warn "An existing install was found in this folder."
        confirm "Run --uninstall first and then reinstall?" && do_uninstall "$GAME_DIR"
    fi

    case "$ENGINE" in
        optiscaler) install_optiscaler "$GAME_DIR" ;;
        reshade)    install_reshade "$GAME_DIR" ;;
        feeder)     install_feeder "$GAME_DIR" ;;
    esac

    # OptiScaler decides what the GPU can do by probing NVAPI while it loads,
    # and under Proton that probe runs before the game has loaded nvapi64.dll.
    # It then records "Upscaler support - dlss: false" on a 4090 and quietly
    # picks XeSS. Observed in Expedition 33: the probe failed at 21:09:10 and
    # the game loaded nvapi64.dll at 21:09:15, five seconds too late.
    #
    # Nothing about that is recoverable at runtime, so the choice is written
    # into the ini at install time instead. Only when the user did not ask for
    # a specific upscaler, and only on NVIDIA, where dlss is the right answer.
    if [[ -z "$UPSCALER" && "$PRESERVED" != *" Upscalers.Dx12Upscaler "* ]] \
       && [[ "$GPU_NAME" == *NVIDIA* || "$GPU_NAME" == *GeForce* ]]; then
        UPSCALER="dlss"
        log "Forcing Dx12Upscaler=dlss (NVAPI is probed too early under Proton to detect it)"
    fi

    # OptiScaler's overlay menu installs window, raw-input and XInput hooks.
    # With them going in during startup, an 8BitDo controller that connected a
    # few seconds after launch deadlocked Wine's xinput_cs and left Like a
    # Dragon Gaiden on a black window; with the menu off the same controller
    # connected mid-game without trouble. Nothing the neural pass needs lives in
    # the menu - [DlssNr] and the upscaler are ini settings the GUI writes - so
    # a fresh install leaves it off unless --overlay=on asks otherwise.
    if [[ "$ENGINE" != reshade && -z "$OVERLAY_SET" && "$PRESERVED" != *" Menu.OverlayMenu "* ]]; then
        OVERLAY_SET="false"
        log "Disabling the OptiScaler overlay menu (--overlay=on to keep it)"
    fi

    # With the overlay off, this key is the only way to switch the pass in game.
    # It works without the menu: F9 toggled it in Like a Dragon Gaiden with
    # OverlayMenu=false, once the key sat in [DlssNr] where OptiScaler reads it.
    if [[ "$ENGINE" != reshade && -z "$TOGGLE_KEY" && "$PRESERVED" != *" DlssNr.ToggleKey "* ]]; then
        TOGGLE_KEY="0x78"
        log "Binding F9 to switch neural rendering in game (--toggle-key to change)"
    fi

    apply_ini_settings "$GAME_DIR"
    print_launch_options
}

main "$@"
