// singularity-shell — offline-capable QtWebEngine wrapper for SingularityApp.
// See qt-tz.md. Entry point: scheme registration, profile setup, asset
// resolution (FR-9a), main window, background updater (FR-9).

#include "SettingsPath.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <QVariant>
#include <QWebEngineDownloadRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlScheme>

#include "AssetStore.h"
#include "DiagnosticsInterceptor.h"
#include "VendorApiInterceptor.h"
#include "MainWindow.h"
#include "NavigationPage.h"
#include "PreloadBridge.h"
#include "SgSchemeHandler.h"
#include "UpdateController.h"

Q_LOGGING_CATEGORY(lcMain, "shell.main")

// Must run BEFORE QApplication exists (FR-2, §6.3).
static void registerSgScheme()
{
    QWebEngineUrlScheme scheme(QByteArrayLiteral("sg"));
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setDefaultPort(443);
    // IMPORTANT: do NOT set LocalScheme/LocalAccessAllowed here.
    // A "local" scheme is treated like file:// by Chromium: pages on it are
    // forbidden to issue ANY request to remote http(s) origins (fetch/XHR
    // fail instantly with TypeError: Failed to fetch, before CORS). That
    // broke every cloud call (health checks, auth, sync). The vendor's
    // Electron client registers the same scheme as standard+secure only
    // (see build/main/index.js: registerSchemesAsPrivileged).
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::ServiceWorkersAllowed
                    | QWebEngineUrlScheme::CorsEnabled
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
                    | QWebEngineUrlScheme::FetchApiAllowed
#endif
    );
    QWebEngineUrlScheme::registerScheme(scheme);
}

static QString helperScriptPath()
{
    // Installed location first (RPM), then dev-tree fallbacks.
    const QStringList candidates = {
        QStringLiteral(SINGULARITY_SHELL_DATADIR) + QStringLiteral("/fetch-assets.sh"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../scripts/fetch-assets.sh"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/fetch-assets.sh"),
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c))
            return QDir(c).canonicalPath();
    return candidates.first();  // UpdateController will log a clear failure
}

int main(int argc, char* argv[])
{
    registerSgScheme();

    QCoreApplication::setApplicationName(QStringLiteral("singularity-shell"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));


    // Pre-parse --chromium-flags (rest goes to Qt).
    for (int i = 1; i < argc - 1; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--chromium-flags")) {
            const QByteArray extra = QByteArray(argv[i + 1]);
            const QByteArray cur = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                    cur.isEmpty() ? extra : cur + " " + extra);
        }
    }

    QApplication app(argc, argv);

    // --- Translations (qt-tz.md §6.9) -----------------------------------------
    // Qt's own dialogs first, then our strings.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qt"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);

    QTranslator appTranslator;
    const QStringList tsPaths = {
        QCoreApplication::applicationDirPath(),                              // build dir
        QStringLiteral(SINGULARITY_SHELL_DATADIR) + QStringLiteral("/translations"),  // installed
        QCoreApplication::applicationDirPath() + QStringLiteral("/../translations"),   // dev tree
        QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),      // bundled
    };
    for (const QString& p : tsPaths) {
        if (appTranslator.load(QLocale(), QStringLiteral("singularity-shell"),
                               QStringLiteral("_"), p)) {
            app.installTranslator(&appTranslator);
            break;
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Offline-capable QtWebEngine wrapper for SingularityApp"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption optDiagnose(
        QStringLiteral("diagnose"),
        QStringLiteral("Log every network request (read-only interception, FR-11)."));
    const QCommandLineOption optNoUpdate(
        QStringLiteral("no-auto-update"),
        QStringLiteral("Disable the background asset updater (FR-9)."));
    const QCommandLineOption optChromium(
        QStringLiteral("chromium-flags"),
        QStringLiteral("Extra flags passed to the Chromium engine."), QStringLiteral("flags"));
    const QCommandLineOption optNoStub(
        QStringLiteral("no-preload-stub"),
        QStringLiteral("Do not inject the window.preloadApi stub: run the app in "
                       "pure web mode (as if no desktop bridge existed)."));
    const QCommandLineOption optProbe(
        QStringLiteral("probe"),
        QStringLiteral("Dump page runtime diagnostics (service workers, IndexedDB) "
                       "to stdout 20 s after start, then quit. Debug aid."));
    parser.addOption(optDiagnose);
    parser.addOption(optNoUpdate);
    parser.addOption(optChromium);
    parser.addOption(optNoStub);
    parser.addOption(optProbe);
    parser.process(app);

    // --- Data locations (§6.2) ------------------------------------------------
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);

    AssetStore store(dataDir + QStringLiteral("/assets"),
                     QStringLiteral(SINGULARITY_SHELL_SYSTEM_ASSETS));
    const AssetStore::AssetSet active = store.resolve();
    const bool bootstrap = !active.valid;
    if (bootstrap)
        qCWarning(lcMain) << "no usable assets (system/user) — bootstrap mode (FR-10)";
    else
        qCInfo(lcMain) << "serving assets:" << active.version << "r" << active.revision
                       << "from" << active.root;


    // --- Persistent profile (FR-3, §6.4) --------------------------------------
    auto* profile = new QWebEngineProfile(QStringLiteral("singularity"), &app);
    profile->setPersistentStoragePath(dataDir + QStringLiteral("/profile"));
    profile->setCachePath(dataDir + QStringLiteral("/profile-cache"));
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("runtime/activeAssetDir"), active.root);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    QWebEngineSettings* ws = profile->settings();
    ws->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    ws->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    ws->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    ws->setAttribute(QWebEngineSettings::ErrorPageEnabled, true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    ws->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
#endif

    // Scheme handler: bound ONCE to the resolved asset set (FR-9 step 1).
    SgSchemeHandler* handler = nullptr;
    if (!bootstrap) {
        handler = new SgSchemeHandler(active.root, profile);
        profile->installUrlSchemeHandler("sg", handler);
    }

    if (parser.isSet(optDiagnose))
        profile->setUrlRequestInterceptor(new DiagnosticsInterceptor(profile));
    else
        // Always-on surgical CORS fix for the vendor gRPC-web API.
        // (Qt allows only one interceptor per profile; --diagnose trades the
        // fix for pure observation, so cloud calls will fail in that mode.)
        profile->setUrlRequestInterceptor(new VendorApiInterceptor(profile));

    // Downloads: backup exports / attachments (FR-12).
    QObject::connect(profile, &QWebEngineProfile::downloadRequested,
                     &app, [](QWebEngineDownloadRequest* dl) {
        const QString suggested = dl->suggestedFileName();
        const QString dest = QFileDialog::getSaveFileName(
            nullptr, QStringLiteral("Save file"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                + QLatin1Char('/') + suggested);
        if (dest.isEmpty())
            return;  // cancelled: request not accepted
        dl->setDownloadFileName(QFileInfo(dest).fileName());
        dl->setDownloadDirectory(QFileInfo(dest).absolutePath());
        dl->accept();
    });

    // --- Main window + bridge --------------------------------------------------
    // Heap-allocated with parent &app: installOn() may be called for multiple
    // pages (main window + popups), so the bridge must not be reparented.
    auto* bridge = new PreloadBridge(&app);
    bridge->setVersions(QStringLiteral(APP_VERSION),
                        bootstrap ? QString() : active.version);

    MainWindow window(profile, parser.isSet(optNoStub) ? nullptr : bridge,
                      bootstrap ? QString() : active.version);
    if (bootstrap)
        window.loadBootstrap();
    else
        window.loadApp();
    window.show();

    // --- Background updater (FR-9; silent, staged) ------------------------------
    UpdateController updater(&store, dataDir, helperScriptPath(), &app);
    QObject::connect(&updater, &UpdateController::stateChanged, &window,
                     [&window](UpdateController::State s, const QString& detail) {
        using S = UpdateController::State;
        QString text;
        switch (s) {
        case S::Downloading: text = QStringLiteral("Downloading update %1…").arg(detail); break;
        case S::Verifying:   text = QStringLiteral("Verifying update %1…").arg(detail); break;
        case S::Staged:      text = QStringLiteral("Update %1 ready — active on next start").arg(detail); break;
        case S::Failed:      text = {}; break;  // silent per FR-9 (log only)
        default: break;
        }
        window.setUpdateStatus(text);
    });
    QObject::connect(&updater, &UpdateController::progressChanged, &window,
                     [&window](int pct) {
        if (pct >= 0)
            window.setUpdateStatus(QStringLiteral("Downloading update… %1%").arg(pct));
    });
    // Bootstrap only: once the first asset set is staged, load the app (FR-10).
    if (bootstrap) {
        QObject::connect(&updater, &UpdateController::versionStaged, &window,
                         [&window, &store, profile](const QString& version, const QString& dir) {
            auto* h = new SgSchemeHandler(dir, profile);
            profile->installUrlSchemeHandler("sg", h);
            window.setUpdateStatus(QStringLiteral("Version %1 installed — starting…").arg(version));
            QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("runtime/activeAssetDir"), dir);
            window.loadApp();
        });
    }

    if (parser.isSet(optNoUpdate))
        qCInfo(lcMain) << "--no-auto-update: updater disabled";
    else if (bootstrap)
        updater.startImmediately();
    else
        updater.start();

    // --probe: runtime diagnostics of the loaded page (debug aid).
    if (parser.isSet(optProbe)) {
        QTimer::singleShot(20'000, &window, [&window] {
            const QString js = QStringLiteral(R"JS(
(async () => {
    const out = {};
    out.url = location.href;
    out.title = document.title;
    try { out.swRegs = (await navigator.serviceWorker.getRegistrations())
        .map(r => ({scope: r.scope, active: !!r.active, state: r.active && r.active.state})); }
    catch (e) { out.swError = String(e); }
    try { out.hasSwController = !!navigator.serviceWorker.controller; }
    catch (e) { out.swControllerError = String(e); }
    try { out.dbs = (await indexedDB.databases()).map(d => d.name); }
    catch (e) { out.idbError = String(e); }
    out.hasCaches = (typeof caches !== 'undefined');
    out.preloadApiType = typeof window.preloadApi;
    out.bodyChildren = document.body ? document.body.children.length : -1;
    return JSON.stringify(out);
})()
)JS");
            window.page()->runJavaScript(js, [](const QVariant& v) {
                qInfo().noquote() << "PROBE:" << v.toString();
                QCoreApplication::quit();
            });
        });
    }

    return app.exec();
}
