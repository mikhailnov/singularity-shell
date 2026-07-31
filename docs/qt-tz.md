# Technical Specification: `singularity-shell` — an offline-capable QtWebEngine wrapper for SingularityApp

**Document type:** Implementation-ready technical specification for an AI coding agent
**Target stack:** C++17 (or newer), Qt 6 (QtWebEngineWidgets), CMake, Linux
**Language of deliverables:** Code and comments in English
**Status:** Final. Sections marked *Phase 0* are discovery tasks to be executed before/while coding.

---

## 1. Context and problem statement

SingularityApp (https://web.singularity-app.com/) is a proprietary task-management SPA. It is distributed for Linux **only as a snap package**, which requires `snapd` and ships a private Electron/Chromium binary. The user:

- **refuses** to install `snapd`;
- **refuses** to run downloaded third-party binaries (including Electron/Chromium blobs and anything downloaded from npm/GitHub releases);
- **requires offline operation** (cold start without network);
- has a **paid plan with cloud sync** that must keep working;
- targets Linux, **not necessarily x86** (ARM must work — therefore any solution based on x86-only prebuilt Electron is unacceptable);
- already has (or accepts) **Qt / QtWebEngine from the distribution's repositories** — that is the only prebuilt browser engine allowed.

The user's earlier attempt to install the web version as a Chromium PWA failed for offline cold start. Root cause was established by direct analysis (see §3): the vendor's service worker precaches **nothing** (`PRECACHE_URLS = []`, runtime cache disabled) and `index.html` is served with `Cache-Control: no-cache`, so every launch requires network.

The vendor's **own Electron build** solves offline correctly and demonstrates the reference architecture this project must reproduce (§3.4).

## 2. Goal

Build a small C++ application (`singularity-shell`) using Qt 6 `QtWebEngine` that:

1. Serves the vendor's web application **from local files** on a stable, privileged custom URL scheme (`sg://renderer`), replicating the vendor's Electron architecture.
2. Starts and works **fully offline** (cold start), with all user data persisted locally.
3. When online, transparently uses the vendor's cloud API (auth + paid sync) exactly as the official web/Electron clients do — **without intercepting, modifying, or proxying** traffic to vendor API hosts.
4. Ships with a baseline snapshot of the application assets **inside the RPM package** at `/usr/share/singularity-shell/` (extracted from the official snap at package build time — no `snapd`), so the very first launch works offline out of the box.
5. In the background, **silently and automatically** keeps the user's own copy of assets up to date (downloaded directly from the Snap Store API, integrity-verified, stored in the user's home directory). A running instance is never disturbed: a freshly downloaded version becomes active **on the next application start** (staged update).
6. Builds on any architecture supported by the distribution's QtWebEngine (x86_64, aarch64).

### 2.1 Non-goals

- No reimplementation of the application's business logic, UI, or sync protocol. The vendor's JavaScript runs **unmodified**.
- No modification of vendor JS assets (the only exception: an *additive, separate* preload stub injected at runtime, §6.5 — files on disk are never patched).
- No traffic interception, header rewriting, caching, or offline emulation for vendor **API hosts** (`*.singularity-app.com` other than the shell origin). Only the shell origin `sg://renderer` is served locally.
- No in-place/hot update of a running instance: background asset updates are staged and take effect only on the next start. No forced restart, no UI interruption, no modal update dialogs (progress/failure may be surfaced via a non-intrusive status indicator only).
- No packaging into snap/flatpak. Distribution format is **RPM** (spec file included; a plain CMake build plus an optional `.desktop` file for development use).

### 2.2 Hard constraints

| # | Constraint |
|---|---|
| C1 | Implementation language: **C++** (Qt 6). Build system: CMake ≥ 3.20. No Python, no Node.js in the product. Bash is permitted **only** for the standalone asset-fetch helper script. |
| C2 | No `snapd`, no snap confinement tooling. The `.snap` file is treated purely as a SquashFS archive. |
| C3 | No prebuilt browser/Electron binaries downloaded from the internet. Only the distribution's Qt packages. |
| C4 | Vendor JS/WASM/HTML/CSS/fonts/images are the only vendor artifacts executed, and they run inside QtWebEngine's sandboxed renderer. |
| C5 | Must work on aarch64 as well as x86_64 (i.e., nothing x86-specific; SquashFS extraction uses the system `unsquashfs`). |
| C6 | Two asset locations with strict roles: the **system asset directory** `/usr/share/singularity-shell/` (RPM-owned, root-owned, read-only at runtime — the app must never attempt to write there) and the **user asset directory** under `$XDG_DATA_HOME/singularity-shell/assets/` (writable, managed by the background updater). All assets are versioned and immutable once installed. |

## 3. Established facts (research already performed — treat as ground truth)

These were verified against the live web app and the official snap `singularityapp` **12.5.0** (revision 145, amd64).

### 3.1 Why the plain web version cannot cold-start offline

- `https://web.singularity-app.com/` responds with `Cache-Control: no-cache` (must revalidate every launch).
- The JS bundles are immutable (`Cache-Control: max-age=315360000`, hash in query string).
- The vendor service worker (`/sw.js`, ~350 kB) contains: `PRECACHE_URLS = []` (empty) and `isRuntimeCacheEnable = false`. Its `fetch` handler only consults the (empty) precache and a local "file manager" store, then falls through to the network.
- Conclusion: the vendor's "automatic offline mode" = *an already-open tab keeps working and queues sync*; a cold start always needs network.

### 3.2 Sync and data plane are pure web tech

- Sync/database logic lives in the page context and web workers (`workers/cloud.js`, `workers/db.js`, `workers/files.js`, `db.bundle.js`, `sw.js`), using IndexedDB + `localStorage` and HTTPS calls to `cloud.singularity-app.com` and related hosts. No Electron/Node APIs are involved in sync.
- **CORS is permissive**: `OPTIONS https://cloud.singularity-app.com/api/user-tariffs` with `Origin: sg://renderer` returns `access-control-allow-origin: *`, `access-control-allow-methods: GET,HEAD,PUT,PATCH,POST,DELETE`, and echoes requested headers (`authorization`, `content-type`). Authentication is token-based (Authorization header), not cookie-based, so `*` works. → Cross-origin API calls from any origin (including our custom scheme) succeed.

### 3.3 Snap contents (relevant subset)

```
/resources/app.asar            # ~337 MB, the whole application
/resources/app.asar.unpacked/  # unpacked node_modules (jszip etc.), no native .node modules required at runtime
```

Inside `app.asar`:

- `package.json`: `name: singularityapp`, `version: 12.5.0`, `main: build/main/index.js`. All runtime dependencies are pure JS/WASM (`sql.js`, `quill`, `@aws-sdk/client-s3`, `flexsearch`, …). **No keytar/sqlite3/sharp or other native modules.**
- `build/` — a complete static web build: `index.html`, `app.css`, `js/app.bundle.js` (+ lazy chunks), `sw.js`, `workers/*.js`, `resources/**` (fonts, images, emoji sprites, help docs), `proxy.html`, `manifest.json`.
- `build/main/index.js`, `build/main/preload.js` — the Electron main/preload processes (Electron-only, **not used** by this project, except as a source of behavioral facts).

The bundled Electron is Chromium 134 (≈ Electron 35) — informational only; we do not run it.

### 3.4 The vendor's Electron architecture (reference design to replicate)

From `build/main/index.js`:

```js
// constants
SINGULARITY_HTTP_PROTOCOL = "sg"
SINGULARITY_HTTP_DOMAIN   = "renderer"
SINGULARITY_HTTP_URL_BASE = "sg://renderer/"

// at startup
protocol.registerSchemesAsPrivileged([{
  scheme: "sg",
  privileges: { standard: true, secure: true, allowServiceWorkers: true, supportFetchAPI: true }
}])

// at runtime: every request to sg://... is served from the local app directory
protocol.handle("sg", (req) => net.fetch(`file://${convertSgUrlToFilePath(req.url)}`))
```

Consequences observed in their build:

- The **entire application** (main window, hidden DB-proxy window `proxy.html`, service worker, IndexedDB, Cache Storage) lives on the single stable origin `sg://renderer`, served from local files. Offline cold start is trivially satisfied: there is no network dependency for the shell at all.
- API calls go cross-origin from `sg://renderer` to `https://*.singularity-app.com` and succeed thanks to permissive CORS (§3.2).
- `preload.js` exposes `window.preloadApi` (via `contextBridge.exposeInMainWorld`) — a set of desktop-integration IPC bridges. IPC channel constants found in the bundle: `IPC_APP_CHANNEL, IPC_BACKUP_CHANNEL, IPC_BADGE_CHANNEL, IPC_CLIPBOARD_CHANNEL, IPC_DB_LOG_CHANNEL, IPC_DEEP_LINK_CHANNEL, IPC_DISK_SPACE_CHECKER_CHANNEL, IPC_DND_CHANNEL, IPC_FETCH_CONTROLLER_CHANNEL, IPC_FILE_CHANNEL, IPC_IMPORT_CHANNEL, IPC_LOG_CHANNEL, IPC_MENU_CHANNEL, IPC_POPUP_CHANNEL, IPC_QUICK_ENTRY_CHANNEL, IPC_SAVE_FILE_DIALOG_CHANNEL, IPC_TARIFFS_CHANNEL, IPC_UPDATE_CHANNEL, IPC_URL_CHANNEL, IPC_WINDOW_CHANNEL, IPC_ZOOM_CHANNEL`.

### 3.5 Asset acquisition without snapd (verified)

```
GET https://api.snapcraft.io/v2/snaps/info/singularityapp
Header: Snap-Device-Series: 16
→ JSON: ."channel-map"[].download.url  (direct .snap download URL)
        ."channel-map"[].download.sha3-384  (integrity hash — MUST be verified)
        ."channel-map"[].version            (e.g. "12.5.0")
```

A `.snap` file is a SquashFS image (`hsqs` magic): `unsquashfs -d <dir> <file>.snap` extracts it without `snapd`.

## 4. High-level architecture

```
┌──────────────────────────── singularity-shell (C++/Qt6) ───────────────────────────┐
│                                                                                    │
│  SgSchemeHandler            AssetStore                UpdateController             │
│  (QWebEngineUrlSchemeHandler│ (resolves active set:    (background, silent: checks │
│   serves sg://renderer/**   │  user dir > system dir    Snap Store API, downloads, │
│   from resolved AssetStore) │  /usr/share, read-only)   verifies, stages for next  │
│         ▲                        ▲                          start via fetch-assets.sh)│
│         │                        │                                                 │
│  Persistent QWebEngineProfile ("singularity", on-disk storage + HTTP disk cache)   │
│         │                                                                          │
│  ┌──────┴───────────────────────────────────────────┐                              │
│  │ MainWindow (QWebEngineView)                       │                             │
│  │  URL: sg://renderer/index.html                    │                             │
│  │  + QWebEngineScript (DocumentCreation, MainWorld):│                             │
│  │    window.preloadApi stub → QWebChannel bridge    │                             │
│  └───────────────────────────────────────────────────┘                             │
│                                                                                    │
│  External links / auth popups → PopupController (in-app popup or system browser)   │
│  Downloads (backup export) → QWebEngineProfile::downloadRequested → QFileDialog    │
└────────────────────────────────────────────────────────────────────────────────────┘
        │                                   │
   sg://renderer (local)          https://*.singularity-app.com (network, untouched)
```

**Key idea (mirrors the vendor's Electron design):** the shell origin `sg://renderer` is *always* available because it is served from disk. The vendor's own service worker and sync workers then behave exactly as in the official desktop client: shell requests resolve locally; data requests go to the real cloud when online and are queued by the vendor's own code when offline.

## 5. Functional requirements

ID convention: FR-n (functional), NFR-n (non-functional). Acceptance tests reference these IDs (§10).

- **FR-1 Shell serving.** On startup the application loads `sg://renderer/index.html`. Every request to `sg://renderer/<path>` is served from the local asset directory (`<assets>/build/<path>`), with correct MIME type, `200` on success, `404` when absent. Path traversal outside the asset root is impossible (normalized, no `..`, no symlinks followed).
- **FR-2 Privileged scheme.** The `sg` scheme is registered **before** `QApplication` construction with flags equivalent to Electron's privileges: standard, secure, service-workers allowed, Fetch API allowed, CORS enabled (see §6.3 for exact Qt flags).
- **FR-3 Persistent state.** Cookies, IndexedDB, Cache Storage, service worker registrations and localStorage persist across restarts (dedicated on-disk `QWebEngineProfile`, §6.4). Closing and reopening the app (online or offline) preserves the session and all user data.
- **FR-4 Offline cold start.** With the network interface down, the application starts, shows the full UI, and allows viewing/editing tasks. (This is inherent to FR-1+FR-2+FR-3, but must be tested explicitly.)
- **FR-5 Online sync.** When online and authenticated, data synchronizes with the vendor cloud as in the official client. The shell must not intercept, cache, block, or modify requests to any host other than the `sg` scheme. No global proxy, no `QWebEngineUrlRequestInterceptor` is installed by default (see FR-11).
- **FR-6 Authentication flow.** Login (including popup/redirect flows to `me.singularity-app.com`, Google/Microsoft OAuth, magic links) works. Popups required by the flow open in an in-app popup window on the same profile; genuinely external links (help, marketing, payment pages) open in the system browser via `QDesktopServices`. The auth result must reach the main page (cookies/tokens share the profile; `window.opener` relationships are preserved by using real popup views, not new processes).
- **FR-7 Desktop-integration stub (`preloadApi`).** A JS stub is injected at document creation (main world) defining `window.preloadApi` so that vendor code paths touching desktop IPC never throw on undefined. Behavior: safe no-ops with `console.warn` logging, plus real implementations where trivial and valuable (window minimize/maximize/close, zoom, badge counter on supported desktops) bridged to C++ via `QWebChannel`. The exact surface to stub is a *Phase 0* discovery item (§6.5).
- **FR-8 Version awareness.** The app shows (e.g., in an About dialog / status bar) the asset version it is running (from `package.json` inside the asset snapshot) and the storage location.
- **FR-9 Background auto-update (silent, staged).** Shortly after every startup (after the UI is shown; small randomized delay, skipped when offline), the app queries the Snap Store API (§3.5). If the store offers a newer `version`/`revision` than the **best locally available** asset set (see FR-9a), it downloads the snap in the background, verifies `sha3-384`, extracts it into a new versioned directory under the **user asset dir** (`$XDG_DATA_HOME/singularity-shell/assets/<version>/`), writes a manifest, validates it (`build/index.html` present, manifest parseable), and atomically switches the `current` symlink. **The running instance keeps using the assets it started with; the new version becomes active on the next launch.** No dialogs; failure is logged and silently retried on a later start (exponential backoff, max once per day). At most **two** user-downloaded versions are kept (new + previous for rollback); older ones are pruned after the new version has started successfully once. A hidden CLI flag `--no-auto-update` disables the whole mechanism; a non-intrusive status-bar/About indicator shows update state (idle / downloading / staged for next start / failed) — no popups.
- **FR-9a Asset resolution order at startup.** The active asset set is chosen as: (1) user asset dir `current` **if valid and its version ≥ system version**; (2) otherwise the system dir `/usr/share/singularity-shell/assets/current`; (3) if neither exists (broken install), the bootstrap page (FR-10). A user copy **older** than the RPM-bundled copy is ignored and scheduled for pruning — a fresh RPM install must never be masked by a stale user download.
- **FR-10 Bootstrap fallback.** FR-9a(3): if no usable assets exist anywhere, show a minimal built-in HTML page (served from `qrc:/`) explaining the situation; the background updater (FR-9) starts immediately in the foreground-blocking sense only for this case — the page shows live download progress and loads the app automatically once assets are staged. The app must never crash or show a blank window due to missing assets. (In normal operation this path is unreachable because the RPM always bundles a baseline asset set.)
- **FR-11 Diagnostics mode.** `--diagnose` CLI flag installs a *logging-only* `QWebEngineUrlRequestInterceptor` that records (not modifies) every request: URL, resource type, scheme/host. Output to stderr/rotating log file. Used for debugging sync/offline behavior.
- **FR-12 External file downloads.** Backup exports / file attachments downloads are handled via `QWebEngineProfile::downloadRequested`, asking the user for a destination with a sensible default filename.
- **FR-13 Graceful network transitions.** The app must not reload or lose state when the network appears/disappears (the vendor code handles offline mode itself). The shell only displays an optional online/offline indicator (via `QNetworkInformation`, if available in the Qt version).

### 5.1 Non-functional requirements

- **NFR-1** Cold start to interactive UI ≤ 3 s on a mid-range machine with warm OS caches.
- **NFR-2** Idle RAM overhead of the shell itself (excluding web content) ≤ ~150 MB.
- **NFR-3** All file I/O in the scheme handler must be off the GUI thread where Qt requires it (see §6.3 note on handler thread affinity) and must not block navigations noticeably.
- **NFR-4** No telemetry, no extra network calls except: vendor app traffic itself and Snap Store API version checks + asset downloads (FR-9, automatic, background, rate-limited to one check per day). Background downloads must be throttled/cancel-safe so they never starve the vendor sync traffic on a slow link.
- **NFR-5** Code must compile with `-Wall -Wextra` without new warnings, C++17, no exceptions crossing Qt boundaries; use `QString`/Qt types at boundaries.

## 6. Detailed design

### 6.1 Project layout

```
singularity-shell/
├── CMakeLists.txt
├── README.md                     # build & usage, incl. distro QtWebEngine package names
├── src/
│   ├── main.cpp                  # scheme registration MUST happen before QApplication
│   ├── MainWindow.{h,cpp}
│   ├── SgSchemeHandler.{h,cpp}   # QWebEngineUrlSchemeHandler
│   ├── ShellProfile.{h,cpp}      # profile setup, settings, download handler
│   ├── PreloadBridge.{h,cpp}     # QWebChannel object + JS stub generation
│   ├── PopupController.{h,cpp}   # createWindow policy, external-link routing
│   ├── UpdateController.{h,cpp}  # Snap Store API client, version compare, sha3-384 verify
│   ├── AssetStore.{h,cpp}        # versioned asset dirs, manifest, atomic switch
│   └── DiagnosticsInterceptor.{h,cpp}
├── resources/
│   ├── shell.qrc                 # bootstrap page (FR-10), qwebchannel.js
│   └── bootstrap.html
├── tools/
│   └── asar-extract.cpp          # dependency-free asar unpacker (build/** + package.json)
├── scripts/
│   └── fetch-assets.sh           # bash: download + verify + extract snap (no snapd, uses asar-extract)
└── packaging/
    ├── singularity-shell.desktop
    └── singularity-shell.spec        # RPM: builds the app + bundles baseline assets
```

### 6.2 Runtime data layout (XDG)

| Path | Owner | Content |
|---|---|---|
| `/usr/share/singularity-shell/assets/<version>/` | RPM (root, read-only) | Baseline extracted `build/` tree + `manifest.json`, shipped in the package |
| `/usr/share/singularity-shell/assets/current` | RPM | Symlink to the bundled baseline version |
| `$XDG_DATA_HOME/singularity-shell/profile/` | user | QtWebEngine profile (cookies, IndexedDB, SW, cache) |
| `$XDG_DATA_HOME/singularity-shell/assets/<version>/` | user | Background-downloaded `build/` tree + `manifest.json` (version, snap revision, sha3-384, fetch date) |
| `$XDG_DATA_HOME/singularity-shell/assets/current` | user | Symlink to the staged user version (atomic switch via rename); takes effect on next start |
| `$XDG_CONFIG_HOME/singularity-shell/settings.ini` | user | QSettings: window geometry, zoom, per-version health markers (started-ok counters), flags |

### 6.3 Custom scheme and handler

- Register in `main()` **before** creating `QApplication`:

```cpp
QWebEngineUrlScheme sgScheme("sg");
sgScheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
sgScheme.setFlags(QWebEngineUrlScheme::SecureScheme
                | QWebEngineUrlScheme::ServiceWorkersAllowed
                | QWebEngineUrlScheme::FetchApiAllowed
                | QWebEngineUrlScheme::CorsEnabled);
QWebEngineUrlScheme::registerScheme(sgScheme);
```

> **VALIDATED PITFALL (do not "fix" this back):** an earlier revision of this
> spec listed `LocalScheme | LocalAccessAllowed`. With `LocalScheme` set,
> Chromium treats `sg://` like `file://`: pages on it are forbidden to issue
> **any** request to remote `http(s)` origins — every `fetch()`/XHR fails
> instantly with `TypeError: Failed to fetch` *before* CORS and before any
> packet is sent (no request appears even in the DevTools Network domain).
> This killed all vendor cloud traffic (health checks, auth, sync) and is
> extremely hard to diagnose from JS side. The vendor's own Electron client
> registers this scheme as `standard + secure + allowServiceWorkers +
> supportFetchAPI` only (see `build/main/index.js`,
> `registerSchemesAsPrivileged`) — no "local" semantics. Same-origin
> `sg://renderer` subresource loads, service workers and IndexedDB all work
> without the local flags.

(Flag availability varies by Qt 6 minor version — guard with `#if QT_VERSION` and document the minimum Qt version in README; target Qt ≥ 6.5.)

- Install `SgSchemeHandler` on the profile (`profile->installUrlSchemeHandler("sg", handler)`).
- Handler maps `requestUrl().path()` → `<assets>/build/<path>`; empty or `/` → `index.html`. Respond with the file bytes and a MIME type from `QMimeDatabase`; add explicit overrides for `.js/.mjs → text/javascript`, `.css → text/css`, `.woff2 → font/woff2`, `.wasm → application/wasm`, `.map → application/json` (do not rely on system MIME DB alone).
- Security: reject paths containing `..` after normalization; resolve against the asset root with `QDir::cleanPath` and verify the result still starts with the root; never follow symlinks out of the root (`QFileInfo::isSymLink` check or open with `O_NOFOLLOW` equivalent).
- Note (implementation detail): `requestStarted()` is invoked on the IO thread; keep the handler stateless/reentrant; do not touch GUI objects from it.
- Do **not** send cache headers that could confuse the vendor SW logic; `200` with correct body is sufficient. (The vendor app appends its own content-hash query strings.)

### 6.4 Profile configuration

- Named persistent profile: `QWebEngineProfile(QStringLiteral("singularity"), parent)` — then explicitly set `setPersistentStoragePath()` to §6.2 path, `setHttpCacheType(DiskHttpCache)`, `setPersistentCookiesPolicy(ForcePersistentCookies)`.
- Settings to enable explicitly: `JavascriptEnabled`, `LocalStorageEnabled`, `ServiceWorkersEnabled` (if exposed in this Qt version), `LocalContentCanAccessRemoteUrls = false` (default — keep it; cross-origin fetches go through CORS as in the official client), `ErrorPageEnabled`.
- User agent: keep the stock QtWebEngine UA. Do **not** spoof Chrome; if a vendor feature gates on UA, that is a *Phase 0* finding to report, not to hack around silently.
- `downloadRequested` → save dialog (FR-12).

### 6.4a Vendor gRPC-web `deadline` header (validated CORS fix)

The vendor cloud API is gRPC-web against `proxy{1..5}.singularity-app.com/.ru`.
Every call carries custom metadata headers: `agent`, `version`, `language`,
`timestamp`, **`deadline`**. The vendor nginx answers CORS preflights with a
static `Access-Control-Allow-Headers` list that **does not include `deadline`**
(verified: identical response for `Origin: sg://renderer` and for the official
`Origin: https://web.singularity-app.com`). Any spec-compliant browser
therefore rejects these requests before sending
("Request header field deadline is not allowed by
Access-Control-Allow-Headers in preflight response"). The official desktop
client never hits this because it issues API calls from the Electron **main
process** (Node axios, no CORS — see `FetchController`/`AxiosFetch` in
`build/main/index.js`); the web client falls back to the vendor's own CORS
proxy (`AntiCORS.isCorsError` → `PROXY_NO_CORS_URL` in `workers/cloud.js`).

Our shell does neither; instead install a `QWebEngineUrlRequestInterceptor`
(`VendorApiInterceptor`) that **removes the `deadline` header** (via
`info.setHttpHeader("deadline", QByteArray())`) for hosts under
`singularity-app.com/.ru` before CORS evaluation. The header is only an
advisory client-side timeout hint (the same timeout is enforced locally via
`DEFAULT_GRPC_TIMEOUT_MS`), so dropping it is semantically safe. Validated
end-to-end: `POST /singularity.api.Service/CanRegister` returns `200` and the
login form advances to the password step. Do **not** use
`--disable-web-security` as an alternative — it weakens the whole profile for
no benefit once this interceptor exists.

### 6.5 `preloadApi` stub and bridge (*Phase 0* dependency)

*Phase 0 task P-1:* extract `build/main/preload.js` from the snap (commands in §11) and enumerate the exact `window.preloadApi` surface (method names, arguments, return-value expectations — sync value vs `Promise`). Also grep `build/js/app.bundle.js` for `preloadApi.` call sites to rank which methods are actually invoked during startup and normal use.

Implementation:

- Generate a JS stub (from a C++-side template + a JSON surface description) injected via `QWebEngineScript` at `DocumentCreation`, `MainWorld`, on all frames (`runsOnSubFrames = true`).
- Every stubbed method: logs a warning once per method name, returns a benign value (`Promise.resolve(null)` / empty array) consistent with what P-1 found.
- Where P-1 shows a method is needed for a feature we deliberately support (window controls, zoom, quit), back it with a `QWebChannel` object (`PreloadBridge`, registered on the page's channel; `qwebchannel.js` loaded from `qrc:///qtwebchannel/qwebchannel.js` in the same injected script).
- Explicitly out of scope (stub to no-op and log): auto-updater (`IPC_UPDATE_CHANNEL` — replaced by our FR-9), tray, OS keychain, file logging to disk.
- If P-1 shows the renderer **refuses to boot** without a specific `preloadApi` behavior that cannot be stubbed safely, stop and report — do not patch vendor files.

### 6.6 Popups and navigation policy

- Reimplement `QWebEnginePage::createWindow` / `certificateError` policy:
  - `NewView`/`Dialog` for hosts on `*.singularity-app.com` or known OAuth providers → in-app popup `QWebEngineView` sharing the profile (needed for `window.opener`-based auth completion).
  - Everything else → `QDesktopServices::openUrl` and return `nullptr`.
- `acceptNavigationRequest`: allow all `sg://` and `https://*.singularity-app.com` navigations; delegate other hosts to the system browser for `NavigationTypeLinkClicked`.

### 6.7 UpdateController (background, silent, staged)

Lifecycle (implements FR-9/FR-9a/FR-10):

1. **Startup (before window show):** resolve the active asset set per FR-9a and freeze it for the whole session — the scheme handler is bound to that resolved path; nothing may switch it under a running instance.
2. **Background check:** once the UI is idle (timer with randomized 10–60 s delay, skipped if offline per `QNetworkInformation` or a failed probe), `QNetworkAccessManager` GET on the API from §3.5 (header `Snap-Device-Series: 16`); parse `channel-map` → pick `channel.track == "latest"`, `risk == "stable"`, architecture map for the build arch (`amd64`→x86_64, `arm64`→aarch64; if absent, log "no build for this architecture" and stop).
3. **Compare:** candidate `version` (+ snap `revision` tiebreaker) vs the **best local version** (max of system dir and user dir). If not newer → done; persist "last check" timestamp to enforce the once-per-day rate limit (NFR-4).
4. **Download** to `$XDG_DATA_HOME/singularity-shell/tmp/<version>.snap.part` (rename to `.snap` on completion; resume optional, cancel-safe; abort and cleanup on app quit — a partial download must never be treated as complete).
5. **Verify** SHA3-384 against `download.sha3-384` (`QCryptographicHash::Sha3_384`, streaming). Mismatch → delete, log, back off.
6. **Extract** via `scripts/fetch-assets.sh extract <snap> <destdir>` (QProcess; the script needs `unsquashfs` to pull `resources/app.asar` out of the SquashFS and the project's own `asar-extract` binary to unpack `build/**` + `package.json` from the asar) into `$XDG_DATA_HOME/singularity-shell/assets/<version>-r<revision>/`; write `manifest.json`; **validate** (`build/index.html` exists and non-empty, `build/js/app.bundle.js` exists, manifest parses).
7. **Stage:** atomically switch the user `current` symlink (create `current.new`, `rename()` over `current`). Log "version X staged for next start"; show the non-intrusive indicator state (FR-9). **Do not** reload pages, do not prompt restart.
8. **Health marking & pruning:** on every successful app start, increment `started-ok` for the active version in QSettings. Prune user asset dirs keeping: the staged `current` + one previous version. If a version fails validation at startup resolution time (FR-9a(1) "valid"), skip it, fall back per FR-9a, and mark it bad so it is never selected again.

All steps are asynchronous (no blocking of the GUI thread); every failure path only logs and schedules a later retry — the update mechanism must never be able to break application startup.

### 6.8 `scripts/fetch-assets.sh` (bash, the only non-C++ component)

Subcommands:

- `fetch-assets.sh latest <destdir>` — resolves URL+hash via Snap Store API (uses `curl` + `jq`), downloads, verifies, extracts to `<destdir>/assets/<version>/`, writes manifest, updates `<destdir>/assets/current` symlink. Used in three places: (a) manually for development bootstrapping, (b) by UpdateController at runtime (destdir = user data dir), (c) **at RPM build time** (destdir = buildroot `/usr/share/singularity-shell`, see §6.10).
- `fetch-assets.sh extract <file.snap> <destdir>` — extraction-only path (used by UpdateController after its own verified download).
- Fails loudly with distinct exit codes (1=network, 2=hash mismatch, 3=unsquashfs missing, 4=bad API response). No `set -x` by default; `set -euo pipefail` mandatory.

### 6.10 RPM packaging

- `packaging/singularity-shell.spec` builds the C++ app and **bundles the baseline asset set**: the `%prep`/`%build` stage runs `scripts/fetch-assets.sh latest %{buildroot}/usr/share/singularity-shell` (or consumes a pre-fetched asset tarball from the build system — the spec must support both, since build hosts may be offline).
- The package owns `/usr/share/singularity-shell/**` (root:root, `0644`/`0755`, no `%config` — replaced wholesale on package upgrade). Package upgrade replaces the baseline; user-downloaded versions in `$HOME` are untouched and continue to win per FR-9a only if newer (see the rollback guard).
- Requires: `qt6-qtwebengine` (distro package name per target distro — document in README), `qt6-qttranslations` (UI localization: `qt_<locale>.qm` for Qt's own dialogs **and** `qtwebengine_<locale>.qm` for the Chromium right-click context menu — Undo/Redo/Cut/Copy/Paste/Paste-and-match-style/Select-all — and the JS alert/confirm/prompt dialogs; note Chromium's own UI (error pages, find bar) comes from the `.pak` files in `qtwebengine_locales/`, not this catalog; `qt6-qtwebengine` does **not** pull `qt6-qttranslations` in), `squashfs-tools` (for runtime extraction), `curl`, `jq` (for the helper script), plus the `.desktop` file and icon (icon extracted from the snap assets: `resources/icons/icon.png` / `meta/gui/icon.png`).
- The RPM never runs the GUI app in `%post`; no network access at install time.

### 6.9 Diagnostics

- `--diagnose`: logging-only interceptor (FR-11) + `QTWEBENGINE_CHROMIUM_FLAGS="--enable-logging=stderr --v=0"` passthrough option `--chromium-flags="<flags>"`.
- Document `QTWEBENGINE_REMOTE_DEBUGGING=<port>` for DevTools (inspect service workers, IndexedDB, network) in README.

## 7. Phase 0 — discovery tasks (do these first; feed results into §6.5/§6.6)

| ID | Task | How |
|---|---|---|
| P-1 | Enumerate `window.preloadApi` surface and its usage | Extract `app.asar` (§11.2), pretty-print `build/main/preload.js`, list all exposed methods; grep `app.bundle.js` for `preloadApi\.(\w+)` with counts. Output: JSON surface description checked into `resources/`. |
| P-2 | Confirm startup doesn't hard-require Electron APIs beyond `preloadApi` | Grep renderer for `require(`, `process.`, `ipcRenderer`, `nodeIntegration` leftovers. Expected: only via `preloadApi`. Report any violation. |
| P-3 | Map the auth flow end-to-end | In DevTools against the live web app: where does the JWT/session come from (cookie on which domain? `postMessage` from popup? redirect with token in URL fragment — note `getPWASchemeURL`/deep-link code exists)? Confirm popup hosts list for §6.6. Output: sequence diagram in `docs/auth-flow.md`. |
| P-4 | Confirm `proxy.html` / worker topology is page-internal | Verify whether `proxy.html` is opened by the renderer itself (iframe/worker/`window.open`) or by the Electron main process. If by main: replicate as a hidden `QWebEnginePage` on `sg://renderer/proxy.html` sharing the profile; if renderer-internal: no action needed. Evidence: grep for `proxy.html` in `app.bundle.js` vs only in `main/index.js`. |
| P-5 | Verify SW registration URL & scope on `sg://renderer` | After first run, via remote debugging: SW active, scope `sg://renderer/`, and offline reload works. |

Each P-task ends with findings appended to `docs/phase0.md`. **Do not write production code for a feature whose P-task contradicts its assumptions.**

## 8. Build instructions (to include in README)

```bash
# Debian/Ubuntu:  apt install qt6-webengine-dev qt6-base-dev cmake build-essential squashfs-tools jq curl
# Fedora/RHEL:    dnf install qt6-qtwebengine-devel cmake squashfs-tools jq curl rpm-build
# Arch:           pacman -S qt6-webengine cmake squashfs-tools jq curl

# Development build (assets into the user data dir):
./scripts/fetch-assets.sh latest ~/.local/share/singularity-shell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/singularity-shell

# RPM build (bundles baseline assets into /usr/share/singularity-shell/):
rpmbuild -ba packaging/singularity-shell.spec
```

## 9. Testing and debugging plan

- Unit tests (Qt Test) for: path mapping/traversal protection in `SgSchemeHandler`; version comparison; sha3-384 verification; manifest read/write; symlink atomic switch.
- Integration checklist: §10 acceptance tests.
- Debugging recipes to document: remote debugging port; `--diagnose` log inspection; forcing offline via `nmcli networking off` or network namespace (`unshare -n`); inspecting IndexedDB/SW via DevTools; vendor log surface (their in-app logs) left untouched.

## 10. Acceptance criteria (all must pass)

| # | Test | Expected |
|---|---|---|
| A-1 (FR-4) | Fresh profile, assets present, **network off** → start app | Full UI loads ≤ 3 s; can browse/create tasks; no error pages |
| A-2 (FR-3) | Create task offline, quit, start again offline | Task present |
| A-3 (FR-5) | Start offline, create task, enable network | Task syncs to cloud (verify via official web client on another device) |
| A-4 (FR-6) | Log in online (password + Google OAuth), restart offline | Session persists; offline start shows user data |
| A-5 (FR-9) | Run with system assets v1; simulate store offering v2 (mock API or real new release) | Background download starts without any dialog; running instance still serves v1 until quit; **next start** serves v2 (About/status shows v2); v1 (system) untouched |
| A-6 (FR-9) | Corrupt background download (truncated file) | Hash mismatch logged silently; no indicator error popup; user `current` symlink unchanged; current session and next start unaffected |
| A-6a (FR-9a) | User assets v1 present, RPM upgraded to baseline v2 | Next start uses the **system v2**; stale user v1 ignored and later pruned |
| A-6b (FR-9) | Staged v3 is corrupted on disk between runs (delete a chunk file) | Startup validation fails → falls back per FR-9a (previous user version or system baseline); bad v3 marked, never selected again |
| A-7 (FR-1) | Request `sg://renderer/../main/index.js` | 404/blocked; no file outside root served |
| A-8 (FR-7) | Startup with `--diagnose` | No `TypeError`/uncaught exceptions mentioning `preloadApi` in renderer console |
| A-9 (FR-10) | Remove **both** system and user assets → start online | Bootstrap page with live progress; background fetch completes; app loads automatically; no blank window, no crash |
| A-10 (NFR-4) | Monitor traffic (`--diagnose`) during normal use | Requests only to `sg://` and `*.singularity-app.com` (+ OAuth providers during login) + Snap Store API on update check |
| A-11 (C5) | Build & run A-1 on aarch64 | Same result |

## 11. Appendix: reference commands and data

### 11.1 Snap Store API (asset acquisition)

```bash
curl -s -H 'Snap-Device-Series: 16' \
  'https://api.snapcraft.io/v2/snaps/info/singularityapp' \
  | jq '."channel-map"[0] | {version, revision, download: .download.url, sha3: .download["sha3-384"], size: .download.size}'
```

### 11.2 Extracting and reading `app.asar` (for Phase 0)

The project ships its own dependency-free extractor (`tools/asar-extract.cpp`,
C++17 STL only; **validated against the real snap: byte-identical output**).
The asar layout it implements:

```
offset 0 : uint32 LE = 4
offset 4 : uint32 LE = header_pickle_size
offset 8 : uint32 LE = header_string_pickle_size
offset 12: uint32 LE = json_length
offset 16: JSON file table (json_length bytes), padded to 4
data     : payloads; entry "offset" is relative to data_base = 16 + align4(json_length)
```

```bash
unsquashfs -f -d sq singularityapp_*.snap 'resources/app.asar'
asar-extract sq/resources/app.asar assets-out   # extracts build/** + package.json
```

`fetch-assets.sh extract` wraps exactly this pipeline.

### 11.3 Known vendor constants

| Constant | Value |
|---|---|
| Shell scheme / host | `sg://renderer` |
| Web origin | `https://web.singularity-app.com` |
| API hosts | `cloud.singularity-app.com` (sync, CORS `*`), `me.singularity-app.com` (account/auth), `proxy`, `storage`, `avatars`, `gpt`, `helpdesk` subdomains |
| Snap (as of spec date) | version `12.5.0`, revision `145`, ~149 MB |
| Vendor Electron | Chromium 134 (≈ Electron 35), snap flags `--disable-gpu --no-sandbox` (snap-confinement workarounds — not needed here) |
| Desktop data dir (their client) | `~/snap/singularityapp/current/.config/SingularityApp` (informational; we use §6.2 layout) |

### 11.4 Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Renderer depends on a `preloadApi` behavior not safely stubbable | Low–Medium | P-1/P-2 first; if hit, report before coding; worst case = run with reduced desktop features |
| Vendor changes asset layout/entry point in a future snap | Medium | Staged update + startup validation (FR-9 step 6/8) + fallback to previous/system version; a broken new version can never be activated |
| Background auto-update silently degrades UX (bandwidth, battery) | Low | Rate limit once/day, random delay, cancel-safe downloads, `--no-auto-update` kill switch |
| System (RPM) baseline grows stale over time | Low | Expected by design: background updater supersedes it per FR-9a; RPM releases should refresh the baseline periodically |
| QtWebEngine build of a given distro lags Chromium features used by the app | Low | Minimum Qt 6.5; document distro/version matrix in README after A-1/A-11 |
| Auth flow relies on `singularityapp://` deep links (desktop-specific) | Medium | P-3; if needed, register an XDG URL handler that forwards into the running app via local socket |
| Vendor ToS concerns about redistributing assets | Low | Assets are fetched per-user from the official store; nothing is redistributed |

---

*End of specification.*
