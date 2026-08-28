# RenoDX DLSS Installer

A customized [ReShade](https://github.com/crosire/reshade) setup that installs, in one click:

- ReShade 6.8.0 with full add-on support (official binaries)
- The RenoDX DLSS add-on
- Updated NVIDIA Streamline / DLSS DLLs, copied next to the game executable

Download the latest installer from the [releases page](https://github.com/yumlevi/renodx-dlss-installer/releases/tag/latest).

## How it works

1. Run `RenoDX-DLSS-Setup.exe` and pick the game executable. If you pick a launcher by
   mistake, the setup looks in common binary folders (`bin`, `bin\x64`, `Binaries\Win64`, ...)
   for the real game executable and corrects the selection automatically.
2. The rendering API is detected automatically; the API page only appears if detection fails.
3. The setup installs ReShade, then downloads [`manifest.ini`](manifest.ini) from this
   repository and installs the add-on and DLL package it points to.

Because the manifest is fetched at install time, updating the add-on or the DLLs is done by
uploading new files to the `latest` release (or editing `manifest.ini`) - no installer rebuild
required. Per-game packages can be added as extra manifest sections keyed by executable name.

## Building

Requires VS Build Tools 2022 (MSBuild, MSVC, Windows SDK) and .NET Framework 4.8 reference
assemblies. Place official `ReShade64.dll` / `ReShade32.dll` in `reshade\bin\x64\Release` and
`reshade\bin\Win32\Release`, then build `reshade\setup\ReShade Setup.csproj` in Release.

The installer source is based on crosire/reshade commit `aae2b7ecc18096ccddca2c073b50727541220292`
with modifications limited to `reshade/setup/`. ReShade is licensed BSD-3-Clause, Copyright (C)
Patrick Mours. The Streamline/DLSS DLLs are NVIDIA-proprietary; their license texts are included
in the DLL package.

⚠ This build of ReShade is intended for singleplayer games only and may cause bans in
multiplayer games.
