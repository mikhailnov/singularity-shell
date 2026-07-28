# singularity-shell.spec — RPM packaging per qt-tz.md §6.10.
# The package bundles a baseline asset set into /usr/share/singularity-shell/
# so the first launch works offline; runtime background updates go to the
# user's home directory.
#
# Build: rpmbuild -ba packaging/singularity-shell.spec
# (or point %_sourcedir at a tarball of this tree; the spec expects the
#  project sources as singularity-shell-%{version}.tar.gz)

Name:           singularity-shell
Version:        0.1.0
Release:        1%{?dist}
Summary:        Offline-capable QtWebEngine wrapper for SingularityApp
License:        MIT
URL:            https://example.invalid/singularity-shell
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtwebengine-devel
BuildRequires:  qt6-qtwebchannel-devel
# Asset acquisition at build time (see %build). For offline build hosts,
# pre-place the snap in %_sourcedir and set:  --with offline_assets
BuildRequires:  curl, jq, openssl, squashfs-tools

Requires:       qt6-qtwebengine
Requires:       squashfs-tools
Requires:       curl
Requires:       jq
Requires:       openssl

%description
A small C++/Qt6 application that serves the SingularityApp web client
from local files on a privileged sg:// origin (mirroring the vendor's
own Electron design), giving true offline cold start while keeping the
official cloud sync. Assets are fetched from the official Snap Store
without snapd; no third-party browser binaries are used.


%prep
%autosetup -n %{name}-%{version}


%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

# --- Baseline assets (qt-tz.md §6.10) ---------------------------------------
# Into a staging dir that %install will copy into the buildroot.
%{__rm} -rf %{_builddir}/assets-staging
%{__mkdir_p} %{_builddir}/assets-staging
%if %{with offline_assets}
    # Offline build host: use the pre-placed snap.
    bash scripts/fetch-assets.sh extract \
        %{_sourcedir}/singularityapp.snap %{_builddir}/assets-staging/assets/manual
%else
    bash scripts/fetch-assets.sh latest %{_builddir}/assets-staging
%endif


%install
%cmake_install

# Baseline assets: versioned dir + "current" symlink (read-only at runtime).
%{__mkdir_p} %{buildroot}%{_datadir}/singularity-shell/assets
%{__cp} -a %{_builddir}/assets-staging/assets/* \
    %{buildroot}%{_datadir}/singularity-shell/assets/
# Symlinks were preserved by cp -a ("current" included).

# Icon from the vendor assets for the desktop file.
%{__mkdir_p} %{buildroot}%{_datadir}/icons/hicolor/512x512/apps
%{__cp} %{buildroot}%{_datadir}/singularity-shell/assets/current/build/resources/favicon-512.png \
    %{buildroot}%{_datadir}/icons/hicolor/512x512/apps/singularity-shell.png


%files
%{_bindir}/singularity-shell
%{_bindir}/asar-extract
%{_datadir}/singularity-shell/fetch-assets.sh
%dir %{_datadir}/singularity-shell/assets
%{_datadir}/singularity-shell/assets/
%{_datadir}/applications/singularity-shell.desktop
%{_datadir}/icons/hicolor/512x512/apps/singularity-shell.png


%changelog
* Mon Jul 27 2026 singularity-shell authors - 0.1.0-1
- Initial package: offline shell, staged background updates, baseline assets.
