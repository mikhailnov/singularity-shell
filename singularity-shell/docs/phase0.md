# Phase 0 — discovery log

Fill this in per qt-tz.md §7 **before relying on the corresponding features**.
Each task lists what to do and where the answer lands in the code.

## Validation results already obtained (ROSA 13.2 container, Qt 6.11.1)

- **P-2 (no fatal Electron deps):** renderer boots fully without Electron.
  Desktop-IPC access goes exclusively through `window.preloadApi`.
- **P-1 (preloadApi stub):** a *non-recursive* Proxy of no-op functions is
  **fatal**: `RendererLogger(this.ipcRenderer)` calls `this.ipc.send(...)`,
  which throws on a function-typed stub and aborts boot *before service worker
  registration*. The recursive universal proxy in `src/PreloadBridge.cpp`
  makes every chain (`preloadApi.ipcRenderer.send(...)`, etc.) a logged no-op;
  with it, boot completes cleanly: SW self-registers, `initFileManager`
  MessagePorts open, collections load into IndexedDB. Remaining P-1 work:
  map real methods for window controls / badge / import if desired.
- **P-5 (service worker):** the app registers `sw.js` itself during boot;
  registration persists; manual `navigator.serviceWorker.register('/sw.js')`
  also succeeds on `sg://renderer`.
- **A-1 (offline cold start):** with `--host-resolver-rules="MAP * ~NOTFOUND"`
  (and `--no-auto-update` so Qt's own network stack is also quiet) the full
  UI renders: sidebar, login modal, promo banner. SW stays active, IndexedDB
  intact. On the container, `unshare -n` cannot raise loopback
  (CAP_NET_ADMIN missing), so DNS blackhole was used instead.
- **Console noise that is normal:** Firebase/Google-Analytics fetch failures
  from `sw.js` when those hosts are unreachable; `QRhiGles2 Failed to create
  context` under `QT_QPA_PLATFORM=offscreen` (software rendering still works).
- **journald:** with a journald socket present, Qt logs leave stderr; use
  `QT_FORCE_STDERR_LOGGING=1`.
- **P-3 (auth/cloud transport) — ROOT-CAUSED AND FIXED, two stacked bugs:**
  1. `LocalScheme` on the `sg://` scheme made Chromium refuse *all* remote
     http(s) fetches from the app origin (instant `TypeError: Failed to
     fetch`, zero entries in the Network domain — the worker's
     `UrlHealthChecker` failed for every `proxyN` host). Removed; flags are
     now `SecureScheme | ServiceWorkersAllowed | CorsEnabled |
     FetchApiAllowed` — mirrors the vendor's own
     `registerSchemesAsPrivileged({standard, secure, allowServiceWorkers,
     supportFetchAPI})` in `build/main/index.js`.
  2. After (1), the gRPC-web calls still failed preflight: the client attaches
     a custom `deadline` header that the vendor nginx never lists in
     `Access-Control-Allow-Headers` (verified identical for
     `Origin: sg://renderer` and `Origin: https://web.singularity-app.com`,
     and reproduced in stock headless Chromium from the official web origin —
     no browser can send this header cross-origin; the official desktop client
     routes API calls through Electron main-process axios, the web client
     falls back to the vendor `PROXY_NO_CORS_URL` proxy). Fixed by
     `VendorApiInterceptor`, which strips `deadline` for
     `*.singularity-app.com/.ru` before CORS evaluation.
  Validated: `POST /singularity.api.Service/CanRegister` → `200`, login form
  advances to the password step; A/B (`--diagnose` swaps in the logging-only
  interceptor) reproduces the failure. Real-credentials login + sync still
  pending a live account.
- **Electron desktop fetch architecture (for the record):** workers call
  `WorkerToMainFetch.fetch` → broadcast `FETCH_FROM_MAIN` → Electron main
  `FetchController` (`IPC_FETCH_CONTROLLER_CHANNEL`) → Node `AxiosFetch`
  (axios, `validateStatus: () => true`, `webcal://`→`https://`) → result
  relayed back via `FETCH_FROM_MAIN_RESULT`. Renderer exposes
  `preloadApi.fetchController.fetch(input, init, timeout)` →
  `{status, statusText, headers: [[k,v]...], body: [bytes...]}`. If we ever
  need true desktop-mode emulation (UA `Electron`), this is the contract to
  implement over QWebChannel + QNetworkAccessManager.
- **Bootstrap page:** `qrc:///html/bootstrap.html` renders during first-run
  asset download (screenshot validated); early bug was `acceptNavigationRequest`
  rejecting the `qrc` scheme.

## P-1 — `window.preloadApi` surface

- [ ] Extract `app.asar` (`scripts/fetch-assets.sh extract` + a full asar dump
      on an analysis machine, or `npx asar extract` — workstation only).
- [ ] Pretty-print `build/main/preload.js`; list every method exposed via
      `contextBridge.exposeInMainWorld("preloadApi", {...})`.
- [ ] Grep `build/js/app.bundle.js` for `preloadApi\.(\w+)` with usage counts.
- [ ] Compare against the generic Proxy stub in `src/PreloadBridge.cpp`.
      If any frequently-used method needs a real return value shape (not
      `Promise.resolve(null)`), extend the `Object.assign(target, {...})` map
      or the noop's return value accordingly.

Findings: _TBD_

## P-2 — hard Electron dependencies beyond `preloadApi`

- [ ] Grep renderer bundles for `require(`, `process.versions`,
      `ipcRenderer`, `node:` — expected: only inside guarded desktop paths.

Findings: _TBD_

## P-3 — auth flow end-to-end

- [ ] Trace login in DevTools on the live web app: where does the JWT/session
      come from (cookie domain / postMessage / URL fragment)?
- [ ] List popup hosts seen during password + Google + Microsoft login.
- [ ] If the flow ends in a `singularityapp://` deep link (desktop-only),
      decide: register an XDG scheme handler feeding back into the running
      instance, or treat as unsupported (web-style login only).

Findings: _TBD_

## P-4 — `proxy.html` / worker topology

- [ ] Grep `app.bundle.js` for `proxy.html` (iframe/worker/window.open?) vs
      usage only in `main/index.js`. If the renderer never opens it itself,
      host a hidden `QWebEnginePage` on `sg://renderer/proxy.html` sharing the
      profile (mirror of the Electron `proxyWindow`).

Findings: _TBD_

## P-5 — service worker on `sg://renderer`

- [ ] After first run with `QTWEBENGINE_REMOTE_DEBUGGING=9223`: SW registered,
      scope `sg://renderer/`, offline reload works (A-1).

Findings: _TBD_
