#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Headless Wine runtime for nrstill.exe (DLSS 5 Neural Rendering on still images).
#
#   nr-runtime.sh status            prints "ready", or what is missing
#   nr-runtime.sh setup [DLL]       builds the Wine prefix (one time) and installs the NR DLL
#   nr-runtime.sh run LOG ARGS...   runs nrstill.exe; ARGS use Windows paths under C:\nr
#
# Nothing here opens a window: every Wine process runs with DISPLAY/WAYLAND_DISPLAY unset.
# The prefix lives in $DLSS5_STILL_DATA (default ~/.local/share/dlss5-still).
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DATA=${DLSS5_STILL_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/dlss5-linux/still}
PFX=$DATA/prefix
RUN=$PFX/drive_c/nr
PROTON=${DLSS5_STILL_PROTON:-}
INSTALLER_CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/dlss5-linux/streamline.zip

find_proton() {
    [[ -n "$PROTON" ]] && { echo "$PROTON"; return; }
    local d
    for d in "$HOME"/.local/share/Steam/compatibilitytools.d/GE-Proton* \
             "$HOME"/.steam/root/compatibilitytools.d/GE-Proton*; do
        [[ -x "$d/files/bin/wine" ]] && echo "$d"
    done | sort -V | tail -1
}

nv_wine_dir() {
    local glx
    glx=$(realpath /usr/lib64/libGLX_nvidia.so.0 2>/dev/null || realpath /usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0 2>/dev/null) || return 1
    [[ -f "$(dirname "$glx")/nvidia/wine/_nvngx.dll" ]] && echo "$(dirname "$glx")/nvidia/wine"
}

W() { env -u DISPLAY -u WAYLAND_DISPLAY -u VK_INSTANCE_LAYERS WINEDEBUG=-all DLSSNR_DISABLE=1 "$@"; }

status() {
    local ge nv
    ge=$(find_proton); nv=$(nv_wine_dir)
    [[ -n "$ge" ]] || { echo "missing: GE-Proton (install it in Steam's compatibilitytools.d)"; return 1; }
    [[ -n "$nv" ]] || { echo "missing: NVIDIA driver NGX files (nvidia/wine/_nvngx.dll)"; return 1; }
    grep -qs NGXCore "$PFX/system.reg" || { echo "missing: prefix"; return 1; }
    [[ -f "$RUN/nvngx_dlssnr.dll" ]] || { echo "missing: dll"; return 1; }
    echo ready
}

setup() {
    local src=${1:-} ge nv L SYS
    ge=$(find_proton) || true; nv=$(nv_wine_dir) || true
    [[ -n "$ge" ]] || { echo "GE-Proton not found"; return 1; }
    [[ -n "$nv" ]] || { echo "NVIDIA driver NGX files not found"; return 1; }
    export WINEPREFIX=$PFX WINEDLLOVERRIDES='mscoree,mshtml,winemenubuilder.exe='
    mkdir -p "$PFX"
    if [[ ! -f "$PFX/system.reg" ]]; then
        echo "Creating the Wine prefix with $(basename "$ge") ..."
        W "$ge/files/bin/wine" wineboot -u
        W "$ge/files/bin/wineserver" -w
    fi
    # What proton's setup_prefix() does for D3D12 + NVAPI + NGX.
    SYS=$PFX/drive_c/windows/system32; L=$ge/files/lib/wine
    install -m644 "$L/vkd3d-proton/x86_64-windows/d3d12.dll"     "$SYS/d3d12.dll"
    install -m644 "$L/vkd3d-proton/x86_64-windows/d3d12core.dll" "$SYS/d3d12core.dll"
    install -m644 "$L/dxvk/x86_64-windows/dxgi.dll"              "$SYS/dxgi.dll"
    install -m644 "$L/nvapi/x86_64-windows/nvapi64.dll"          "$SYS/nvapi64.dll"
    install -m644 "$L/dxvk/x86_64-windows/d3d11.dll"             "$SYS/d3d11.dll"
    install -m644 "$L/nvapi/x86_64-windows/nvofapi64.dll"        "$SYS/nvofapi64.dll"
    install -m644 "$nv/_nvngx.dll" "$SYS/_nvngx.dll"
    install -m644 "$nv/nvngx.dll"  "$SYS/nvngx.dll"
    # Without these, NGX core Init_Ext fails with PlatformError (0xBAD00002).
    local reg=$PFX/dlss5-still.reg
    cat >"$reg" <<'REG'
REGEDIT4

[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"d3d12"="native"
"d3d12core"="native"
"dxgi"="native"
"d3d11"="native"
"nvapi64"="native"
"nvofapi64"="native"
"mscoree"=""
"mshtml"=""

[HKEY_LOCAL_MACHINE\SOFTWARE\NVIDIA Corporation\Global\NGXCore]
"FullPath"="C:\\windows\\system32"
REG
    W "$ge/files/bin/wine" regedit /S "$reg"
    W "$ge/files/bin/wineserver" -w

    mkdir -p "$RUN/in" "$RUN/out"
    install -m755 "$HERE/bin/nrstill.exe" "$HERE/bin/nvngx.dll_nrshim.dll" "$RUN/"
    if [[ -n "$src" ]]; then
        case "$src" in
            *.zip) unzip -o -j -q "$src" 'streamline/nvngx_dlssnr.dll' -d "$RUN" || { echo "no streamline/nvngx_dlssnr.dll in $src"; return 1; } ;;
            *) install -m644 "$src" "$RUN/nvngx_dlssnr.dll" ;;
        esac
    elif [[ ! -f "$RUN/nvngx_dlssnr.dll" ]]; then
        [[ -f "$INSTALLER_CACHE" ]] || { echo "No nvngx_dlssnr.dll: choose the file, or run the DLSS 5 installer once so its cache has it."; return 1; }
        echo "Copying nvngx_dlssnr.dll from the installer cache ..."
        unzip -o -j -q "$INSTALLER_CACHE" 'streamline/nvngx_dlssnr.dll' -d "$RUN" || return 1
    fi
    echo "Runtime ready in $DATA"
}

run() {
    local log=$1; shift
    local ge nv rc
    ge=$(find_proton); nv=$(nv_wine_dir)
    # Keep the exe/shim in step with the app's bin/ (an updated app ships new ones).
    cmp -s "$HERE/bin/nrstill.exe" "$RUN/nrstill.exe" || install -m755 "$HERE/bin/nrstill.exe" "$RUN/"
    cmp -s "$HERE/bin/nvngx.dll_nrshim.dll" "$RUN/nvngx.dll_nrshim.dll" || install -m755 "$HERE/bin/nvngx.dll_nrshim.dll" "$RUN/"
    cd "$RUN" || return 1
    env -u DISPLAY -u WAYLAND_DISPLAY -u VK_INSTANCE_LAYERS -u DLSSNR_LAYER_OBJECT \
        WINEPREFIX="$PFX" WINEDEBUG=-all \
        WINEDLLOVERRIDES='mscoree,mshtml,winemenubuilder.exe=;d3d12,d3d12core,dxgi,d3d11,nvapi64,nvofapi64=n' \
        DXVK_ENABLE_NVAPI=1 NVIDIA_WINE_DLL_DIR="$nv" DLSSNR_DISABLE=1 \
        timeout "${NRTIMEOUT:-240}" "$ge/files/bin/wine" 'C:\nr\nrstill.exe' "$@" >"$log" 2>&1
    rc=$?
    WINEPREFIX="$PFX" "$ge/files/bin/wineserver" -k 2>/dev/null
    echo "exit code: $rc" >>"$log"
    return $rc
}

case "${1:-}" in
    status) status ;;
    setup) shift; setup "${1:-}" ;;
    run) shift; run "$@" ;;
    *) sed -n '2,9p' "$0"; exit 64 ;;
esac
