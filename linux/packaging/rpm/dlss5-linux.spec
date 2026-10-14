# dlss5-linux.spec - RPM for the Linux port (linux/ in renodx-dlss-installer).
#
# Built by ../build-rpm.sh, which makes Source0 from the working tree and writes
# VERSION from dlss5-linux.sh into the Version: line of its copy of this spec,
# so that stays the one place the version is set and the SRPM carries a literal
# version that rebuilds (rpmbuild --rebuild, mock, COPR) on its own. Run
# rpmbuild on this file directly and the placeholder below stops it at once.
# Everything here is scripts and data: no compiler, noarch.

Name:           dlss5-linux
Version:        %{!?pkg_version:%{error:build this with ../build-rpm.sh, which writes the Version}}%{?pkg_version}
Release:        1%{?dist}
Summary:        Probe Steam games and install DLSS 5 neural rendering under Proton

# linux/ (everything this package ships) is GPL-3.0-only; see linux/LICENSE.
# reshade/LICENSE.md is NOT this package's licence: it is ReShade's
# (BSD-3-Clause, Patrick Mours) and covers only the vendored reshade/setup code,
# none of which is packaged. Keep <project_license> in the metainfo in step.
# The DLLs fetched at run time carry their own terms and are not shipped.
License:        GPL-3.0-only
URL:            https://github.com/MiguelCarino/renodx-dlss-installer
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  desktop-file-utils
# %%check syntax-checks the Python scripts.
BuildRequires:  python3

# What dlss5-linux.sh's preflight checks for, plus xdg-open for the GUI's
# "open folder" buttons. bash and python3 are not listed: rpmbuild already
# requires /usr/bin/bash and /usr/bin/python3 from the scripts' shebangs.
Requires:       curl
Requires:       unzip
Requires:       binutils
# 7z for the reshade and feeder engines. Only p7zip-plugins, not p7zip: on
# Fedora <= 41 p7zip-plugins ships /usr/bin/7z and pulls in p7zip itself; from
# Fedora 42 the 7zip package provides p7zip-plugins and /usr/bin/7z, while
# "p7zip" resolves to 7zip-standalone, which has no 7z binary at all.
Requires:       p7zip-plugins
Requires:       xdg-utils
Requires:       hicolor-icon-theme

# The native window. Without these dlss5-app.py says why and opens the same
# page in the system browser instead, so they are optional.
Recommends:     python3-gobject
Recommends:     gtk4
Recommends:     libadwaita
Recommends:     webkitgtk6.0

%description
A Linux port of renodx-dlss-installer. It walks every Steam library, probes
each installed game to say whether DLSS 5 can work there, and installs,
uninstalls or launches it under Proton. Three engines: optiscaler (games that
ship DLSS), reshade (ReShade + RenoDX add-on + Streamline) and feeder
(DLSS5-Feeder, for games without DLSS).

Components are downloaded at run time from their projects' GitHub releases;
none are shipped in this package. Single-player games only, never with
anti-cheat.

%global app_id io.github.MiguelCarino.DLSS5Linux

%prep
%autosetup -n %{name}-%{version}

%build
# Nothing to build.

%install
# The scripts locate each other through their own directory (HERE / SCRIPT in
# dlss5-gui.py), so all three live together and nothing writes there at run
# time: user state goes to ~/.cache and ~/.config.
install -Dpm 0755 linux/dlss5-linux.sh %{buildroot}%{_datadir}/%{name}/dlss5-linux.sh
install -Dpm 0755 linux/dlss5-gui.py   %{buildroot}%{_datadir}/%{name}/dlss5-gui.py
install -Dpm 0755 linux/dlss5-app.py   %{buildroot}%{_datadir}/%{name}/dlss5-app.py
# The "Still image" view's runtime: a Wine-side helper built from still/native
# (shipped beside it as its corresponding source) and the script that runs it
# in a private prefix under ~/.local/share.
install -Dpm 0755 linux/still/nr-runtime.sh            %{buildroot}%{_datadir}/%{name}/still/nr-runtime.sh
install -Dpm 0644 linux/still/bin/nrstill.exe          %{buildroot}%{_datadir}/%{name}/still/bin/nrstill.exe
install -Dpm 0644 linux/still/bin/nvngx.dll_nrshim.dll %{buildroot}%{_datadir}/%{name}/still/bin/nvngx.dll_nrshim.dll
install -Dpm 0644 linux/still/native/nrstill.cpp       %{buildroot}%{_datadir}/%{name}/still/native/nrstill.cpp
install -Dpm 0644 linux/still/native/nrshim.cpp        %{buildroot}%{_datadir}/%{name}/still/native/nrshim.cpp
install -Dpm 0755 linux/still/native/build.sh          %{buildroot}%{_datadir}/%{name}/still/native/build.sh

install -Dpm 0755 linux/packaging/bin/dlss5-linux     %{buildroot}%{_bindir}/dlss5-linux
install -Dpm 0755 linux/packaging/bin/dlss5-linux-gui %{buildroot}%{_bindir}/dlss5-linux-gui

install -Dpm 0644 linux/packaging/%{app_id}.desktop      %{buildroot}%{_datadir}/applications/%{app_id}.desktop
install -Dpm 0644 linux/packaging/%{app_id}.svg          %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
install -Dpm 0644 linux/packaging/%{app_id}.png          %{buildroot}%{_datadir}/icons/hicolor/256x256/apps/%{app_id}.png
install -Dpm 0644 linux/packaging/%{app_id}.metainfo.xml %{buildroot}%{_metainfodir}/%{app_id}.metainfo.xml

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
# Catches a script that would fail before printing anything, e.g. a syntax error.
bash -n %{buildroot}%{_datadir}/%{name}/dlss5-linux.sh
bash -n %{buildroot}%{_datadir}/%{name}/still/nr-runtime.sh
sh -n %{buildroot}%{_bindir}/dlss5-linux
sh -n %{buildroot}%{_bindir}/dlss5-linux-gui
# compile() rather than py_compile, so no __pycache__ lands in the buildroot.
python3 -c 'import sys; [compile(open(f).read(), f, "exec") for f in sys.argv[1:]]' \
    %{buildroot}%{_datadir}/%{name}/*.py

%files
%license linux/LICENSE
# README.md is the project's full readme; its build-from-source paths do not
# apply to an installed system, but its usage sections do.
%doc linux/README.md
%{_bindir}/dlss5-linux
%{_bindir}/dlss5-linux-gui
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/dlss5-linux.sh
%{_datadir}/%{name}/dlss5-gui.py
%{_datadir}/%{name}/dlss5-app.py
%{_datadir}/%{name}/still/
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_datadir}/icons/hicolor/256x256/apps/%{app_id}.png
%{_metainfodir}/%{app_id}.metainfo.xml

%changelog
* Wed Oct 14 2026 MiguelCarino <miguel.carino1994@outlook.com> - 0.0.3-1
- Experimental "Still image" view: run the DLSS 5 NR model on a picture
  through a headless Wine helper (still/), with a before/after comparison.

* Wed Oct 14 2026 MiguelCarino <miguel.carino1994@outlook.com> - 0.0.2-1
- Detect the GPU vendor from sysfs; clear messages on AMD and Intel.
- Experimental AMD engine (--engine=amd) using RedDukeDev/dlss5-nr-amd-custom.

* Wed Oct 14 2026 MiguelCarino <miguel.carino1994@outlook.com> - 0.0.1-1
- First package: CLI (dlss5-linux), native desktop app (dlss5-linux-gui),
  desktop entry, icon and AppStream metadata.
