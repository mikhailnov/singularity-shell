# singularity-shell

Offline-capable **Qt 6 / QtWebEngine** wrapper for [SingularityApp](https://web.singularity-app.com/)
— no snapd, no third-party browser binaries. Design document: `qt-tz.md`.

> ✅ **Build status:** compiled and smoke-tested on **ROSA Fresh 13.2,
> Qt 6.11.1** (systemd-nspawn container, offscreen). Validated there:
>
> - offline **cold start**: with DNS blackholed the full UI renders from local
>   assets (acceptance test A-1, minus real account data — no login was done);
> - the app **self-registers its service worker** on `sg://renderer` and
>   populates IndexedDB (`AppDatabase`, `SingularityLogs3`); registrations
>   persist across restarts;
> - **cloud API transport works end-to-end**: the login flow's first server
>   call (`POST /singularity.api.Service/CanRegister` on
>   `proxy1.singularity-app.com`) returns `200` and the UI advances to the
>   password step (tested with a fake email; real credentials not tried);
> - the **bootstrap page** renders during first-run asset download
>   (`qrc:///html/bootstrap.html`);
> - the **background updater** runs: Snap Store check → "up to date 12.5.0
>   r145" (download/verify/stage path is exercised by `fetch-assets.sh`,
>   which is tested end-to-end incl. SHA3-384);
> - asset pipeline (`tools/asar-extract.cpp`, `scripts/fetch-assets.sh`)
>   byte-identical extraction against the real snap.
>
> **Not yet tested** (needs a display and/or a real account): real login &
> actual cloud sync (FR-5/A-3), Google/Apple OAuth popups (FR-6), downloads
> (FR-12), RPM build (§6.10), aarch64 (A-11).
>
> Hard-won runtime notes (all validated in the container):
>
> - **`sg://` must NOT be registered with `LocalScheme`/`LocalAccessAllowed`.**
>   A "local" scheme is treated like `file://`: every fetch/XHR to remote
>   http(s) fails instantly with `TypeError: Failed to fetch` — before CORS,
>   before any packet (nothing shows in the DevTools Network domain). This
>   silently killed all cloud traffic. Flags are exactly: `SecureScheme |
>   ServiceWorkersAllowed | CorsEnabled | FetchApiAllowed`.
> - The vendor gRPC-web API sends a custom `deadline` header that the vendor's
>   own CORS preflight response does not allow (true for *any* origin — even
>   the official web client can't send it; the desktop client avoids CORS via
>   Electron's main process). `VendorApiInterceptor` strips that one header
>   for `*.singularity-app.com/.ru` before CORS evaluation — surgical fix, no
>   `--disable-web-security`.
> - in environments where a **journald socket** is present, Qt logs do not
>   reach stderr — run with `QT_FORCE_STDERR_LOGGING=1` to see `shell.*` logs;
> - the injected `preloadApi` stub is a *recursive universal proxy* — a plain
>   no-op-function proxy crashed the renderer boot (`this.ipc.send is not a
>   function`) before service worker registration. If you tune the stub
>   (P-1 in docs/phase0.md), keep the recursive shape.

## How it works

The vendor's own Electron client runs the whole app on a privileged custom
origin `sg://renderer` served from local files. This shell replicates exactly
that with `QWebEngineUrlScheme` + `QWebEngineUrlSchemeHandler`: the shell
origin is always "online" because it comes from disk, so **cold start works
offline**; sync and auth go cross-origin to the vendor's cloud (their API
answers `Access-Control-Allow-Origin: *`) exactly as in the official client.

- Baseline assets may be shipped at `/usr/share/singularity-shell/` →
  first launch works offline out of the box.
- A **silent background updater** checks the Snap Store once a day, downloads
  newer assets into `$XDG_DATA_HOME/singularity-shell/assets/` (SHA3-384
  verified), and stages them via an atomic symlink switch — active **on the
  next start**. Disable with `--no-auto-update`.
- All app state (cookies, IndexedDB, service workers) lives in a persistent
  `QWebEngineProfile` under `$XDG_DATA_HOME/singularity-shell/profile/`.

## Build

```bash
# Fedora/RHEL:    dnf install qt6-qtwebengine-devel qt6-qtwebchannel-devel \
#                 cmake gcc-c++ squashfs-tools jq curl openssl rpm-build
# Debian/Ubuntu:  apt install qt6-webengine-dev qt6-webchannel-dev \
#                 cmake g++ squashfs-tools jq curl openssl

# Assets for the dev run (into the user data dir):
./scripts/fetch-assets.sh latest ~/.local/share/singularity-shell

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/singularity-shell
```

## CLI

| Flag | Effect |
|---|---|
| `--diagnose` | Log every request (read-only; FR-11) |
| `--no-auto-update` | Disable background asset updates |
| `--chromium-flags "<flags>"` | Extra flags for the Chromium engine |

## Debugging

```bash
QTWEBENGINE_REMOTE_DEBUGGING=9223 ./build/singularity-shell
# then open http://127.0.0.1:9223 in any Chromium browser:
# DevTools for the page, service workers, IndexedDB, network
./build/singularity-shell --diagnose 2>&1 | grep '\[request\]'
# verbose Qt logs:
QT_LOGGING_RULES="shell.*.debug=true" ./build/singularity-shell
```

Offline cold-start test: `nmcli networking off` (or `unshare -n
./build/singularity-shell`), launch, work, quit, launch again.

## Layout

```
src/            C++ application (see qt-tz.md §6.1)
tools/          asar-extract.cpp — dependency-free asar unpacker (tested)
scripts/        fetch-assets.sh — snap → verified assets, no snapd (tested)
resources/      bootstrap page (first-run fallback, FR-10)
packaging/      .desktop file, RPM spec
docs/           phase0.md (fill in per qt-tz.md §7)
```

## Data locations

| Path | Content |
|---|---|
| `/usr/share/singularity-shell/assets/` | Baseline assets from the RPM (read-only) |
| `~/.local/share/singularity-shell/assets/` | Background-updated assets (versioned, `current` symlink) |
| `~/.local/share/singularity-shell/profile/` | Cookies, IndexedDB, SW, cache |
| `~/.config/singularity-shell/settings.ini` | UI state, updater timestamps |
