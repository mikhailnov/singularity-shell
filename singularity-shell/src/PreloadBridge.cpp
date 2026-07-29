#include "PreloadBridge.h"

#include <QDesktopServices>
#include <QFile>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

PreloadBridge::PreloadBridge(QObject* parent) : QObject(parent) {}

void PreloadBridge::setVersions(QString appVer, QString assetVer)
{
    m_appVersion = std::move(appVer);
    m_assetVersion = std::move(assetVer);
}

bool PreloadBridge::isMaximized() const
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent())) {
        if (auto* view = qobject_cast<QWebEngineView*>(p->parent()))
            return view->isMaximized();
    }
    return false;
}

void PreloadBridge::windowMinimize()   { emit minimizeRequested(); }
void PreloadBridge::windowMaximize()   { emit maximizeToggleRequested(); }
void PreloadBridge::windowUnmaximize() { emit maximizeToggleRequested(); }
void PreloadBridge::windowClose()      { emit closeRequested(); }

void PreloadBridge::zoomIn()
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent()))
        p->setZoomFactor(qBound(0.25, p->zoomFactor() + 0.1, 5.0));
}
void PreloadBridge::zoomOut()
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent()))
        p->setZoomFactor(qBound(0.25, p->zoomFactor() - 0.1, 5.0));
}
void PreloadBridge::zoomReset()
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent()))
        p->setZoomFactor(1.0);
}
double PreloadBridge::zoomFactor() const
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent()))
        return p->zoomFactor();
    return 1.0;
}

void PreloadBridge::openExternal(const QString& url)
{
    if (!url.isEmpty())
        emit externalOpenRequested(QUrl(url));
}
void PreloadBridge::installOn(QWebEnginePage* page)
{
    // Connect this page's view to bridge signals. Each bridge instance is
    // created per-page (main window gets one, each popup gets its own), so
    // signals never cross-contaminate between windows.
    if (auto* view = qobject_cast<QWebEngineView*>(page->parent())) {
        connect(this, &PreloadBridge::minimizeRequested, view, &QWidget::showMinimized);
        connect(this, &PreloadBridge::maximizeToggleRequested, view, [view] {
            view->setWindowState(view->windowState() ^ Qt::WindowMaximized);
        });
        connect(this, &PreloadBridge::closeRequested, view, &QWidget::close);
    }
    connect(this, &PreloadBridge::externalOpenRequested, this,
            [](const QUrl& u) { QDesktopServices::openUrl(u); });

    // 1) WebChannel with this object.
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("preloadBridge"), this);
    page->setWebChannel(channel);

    // 2) Inject qwebchannel.js + stub + CSS at document creation, main world.
    QFile qc(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    QString qwebchannelJs;
    if (qc.open(QIODevice::ReadOnly))
        qwebchannelJs = QString::fromUtf8(qc.readAll());

    const QString stubJs = qwebchannelJs + QStringLiteral(R"JS(
;(function () {
    'use strict';

    // --- CSS: hide window-control buttons (Qt provides native decorations) ---
    var style = document.createElement('style');
    style.textContent = '.win-top-panel { display: none !important; }';
    document.addEventListener('DOMContentLoaded', function () {
        (document.head || document.documentElement).appendChild(style);
    });

    // --- Helper: async wrapper that returns a Promise ---
    function wrapAsync(fn) {
        return function () {
            return new Promise(function (resolve) {
                fn.apply(this, arguments);
                resolve(null);
            });
        };
    }

    // --- Helper: sync getter via QWebChannel property ---
    function wrapSync(getter) {
        return function () { return getter(); };
    }

    // --- Default stubs for every controller ---
    // Each method is an async no-op returning Promise<null> unless overridden.
    function makeStubController(methods) {
        var c = {};
        methods.forEach(function (m) {
            c[m] = function () { return Promise.resolve(null); };
        });
        return c;
    }

    // --- ipcRenderer stub (fully sufficient for vendor's needs) ---
    var ipcStub = {
        send: function () {},
        on:   function () { return function () {}; },
        off:  function () {}
    };

    // --- Remaining controller stubs ---
    var stubs = {
        appController:            makeStubController(['QUIT_APP','RESTART_APP','WORKERS_LANGUAGE','SPELLCHECK_ENABLED','SPELLCHECK_IS_ENABLED']),
        fileController:           makeStubController(['open','clear']),
        deepLinkController:       makeStubController(['APP_OPEN_URL']),
        purchaseController:       makeStubController(['fetchTariffs','initStoreListeners','purchase','purchaseRestore','disposeStoreListeners']),
        updateController:         makeStubController(['applyUpdateAndRestart','checkUpdates']),
        badgeController:          makeStubController(['BADGE_COUNT_CHANGE']),
        diskSpaceCheckerController: makeStubController(['isLowSpace']),
        quickEntryController:     makeStubController(['QUICK_ENTRY_REGISTER']),
        popupController:          makeStubController(['isVisible','open','setBounds','sendResult','close','onPopupControllerReady','updatePosition','focus']),
        saveFileDialogController: makeStubController(['saveFile']),
        clipboardController:      makeStubController(['clear','readText','readHTML','write','broadcastCopyCut','setContextMenuIsQuill']),
        dndController:            makeStubController(['REMOVE_CHECKLIST_FROM_EDITOR','DRAG_START','DRAG_END']),
        fetchController:          { fetch: function (url, options) { return window.fetch(url, options).then(function (r) { return r.arrayBuffer().then(function (body) { return { body: body, status: r.status, statusText: r.statusText, headers: Array.from(r.headers.entries()) }; }); }); } },
        backupController:         makeStubController(['GET_LOG_FILE_WITH_OBFUSCATED_BACKUP','GET_RAW_BACKUP']),
        importController:         makeStubController(['IMPORT_THINGS','IMPORT_OMNIFOCUS','IMPORT_TICKTICK','IMPORT_CSV','IMPORT_TODOIST','IMPORT_EVERNOTE','IMPORT_NOTION']),
        menuController:           makeStubController(['TOOLBAR_UPDATED','SHOW_MAIN_MENU_AS_POPUP_MENU','POPUP_MENU'])
    };

    // --- windowController (defined first — bridge depends on it) ---
    var wcStub = {
        getId:         function () { return 1; },
        isVisible:     function () { return true; },
        isFocused:     function () { return true; },
        isMaximized:   function () { return false; },
        isFullScreen:  function () { return false; },
        minimize:          function () { return Promise.resolve(null); },
        maximize:          function () { return Promise.resolve(null); },
        unmaximize:        function () { return Promise.resolve(null); },
        close:             function () { return Promise.resolve(null); },
        hide:              function () { return Promise.resolve(null); },
        show:              function () { return Promise.resolve(null); },
        focus:             function () { return Promise.resolve(null); },
        blur:              function () { return Promise.resolve(null); },
        setAlwaysOnTop:    function () { return Promise.resolve(null); },
        moveTop:           function () { return Promise.resolve(null); },
        setFullScreen:     function () { return Promise.resolve(null); },
        OPEN_NEW_WINDOW:       function () { window.open('sg://renderer/index.html'); return Promise.resolve(null); },
        SET_ERROR_IN_RENDER:   function () { return Promise.resolve(null); },
        SHOW_POMODORO_SETTINGS: function () { return Promise.resolve(null); }
    };

    // Spreads wcStub methods so provider.window.focus() etc. work,
    // plus getPosition/getBounds/addListener from ipcRenderer.
    var winBridge = {
        getId:         wcStub.getId,
        isVisible:     wcStub.isVisible,
        isFocused:     wcStub.isFocused,
        isMaximized:   wcStub.isMaximized,
        isFullScreen:  wcStub.isFullScreen,
        minimize:      wcStub.minimize,
        maximize:      wcStub.maximize,
        unmaximize:    wcStub.unmaximize,
        close:         wcStub.close,
        hide:          wcStub.hide,
        show:          wcStub.show,
        focus:         wcStub.focus,
        blur:          wcStub.blur,
        setAlwaysOnTop: wcStub.setAlwaysOnTop,
        moveTop:       wcStub.moveTop,
        setFullScreen: wcStub.setFullScreen,
        OPEN_NEW_WINDOW:       wcStub.OPEN_NEW_WINDOW,
        SET_ERROR_IN_RENDER:   wcStub.SET_ERROR_IN_RENDER,
        SHOW_POMODORO_SETTINGS: wcStub.SHOW_POMODORO_SETTINGS,
        getPosition: function () { return [window.screenLeft, window.screenTop]; },
        getBounds:   function () { return { x: window.screenLeft, y: window.screenTop, width: window.innerWidth, height: window.innerHeight }; },
        addListener: function (ch, cb) { return ipcStub.on(ch, cb); },
        id: 1
    };

    // --- Assemble preloadApi ---
    window.preloadApi = {
        getPathForFile: function () { return undefined; },
        ipcRenderer:    ipcStub,
        isPopup:        false,
        renderer:       ipcStub,
        windowRenderToMainBridge: winBridge,
        windowController: wcStub,
        zoomController: {
            ZOOM_IN:   function () { return Promise.resolve(null); },
            ZOOM_OUT:  function () { return Promise.resolve(null); },
            ZOOM_RESET: function () { return Promise.resolve(null); }
        },
        urlController: {
            openExternal:     function (url) { window.open(url, '_blank'); return Promise.resolve(null); },
            openPath:         function () { return Promise.resolve(null); },
            supportsOpenPath: function () { return Promise.resolve(false); }
        }
    };

    // Merge remaining stubs
    Object.keys(stubs).forEach(function (k) { window.preloadApi[k] = stubs[k]; });

    // --- Wire QWebChannel → real implementations override the stubs ---
    if (typeof QWebChannel !== 'undefined' && typeof qt !== 'undefined'
            && qt.webChannelTransport) {
        new QWebChannel(qt.webChannelTransport, function (channel) {
            var b = channel.objects.preloadBridge;
            if (!b) return;

            // Window controller: async actions
            var wc = window.preloadApi.windowController;
            wc.minimize   = wrapAsync(function () { b.windowMinimize(); });
            wc.maximize   = wrapAsync(function () { b.windowMaximize(); });
            wc.unmaximize = wrapAsync(function () { b.windowUnmaximize(); });
            wc.close      = wrapAsync(function () { b.windowClose(); });
            wc.isMaximized = function () { return b.isMaximized; };

            // Zoom controller
            var zc = window.preloadApi.zoomController;
            zc.ZOOM_IN   = wrapAsync(function () { b.zoomIn(); });
            zc.ZOOM_OUT  = wrapAsync(function () { b.zoomOut(); });
            zc.ZOOM_RESET = wrapAsync(function () { b.zoomReset(); });

            // URL controller: real openExternal
            var uc = window.preloadApi.urlController;
            uc.openExternal = function (url) {
                // QWebChannel requires an explicit String() cast for argument
                // serialization; passing the raw value may produce an empty call.
                b.openExternal(String(url));
                return Promise.resolve(null);
            };
            // App controller: QUIT_APP → window.close()
            var ac = window.preloadApi.appController;
            ac.QUIT_APP = wrapAsync(function () { b.windowClose(); });

            // Shell info
            window.preloadApi.shellInfo = {
                appVersion:   b.appVersion,
                assetVersion: b.assetVersion
            };

        });
    }
})();
)JS");

    QWebEngineScript script;
    script.setName(QStringLiteral("preloadApi-stub"));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setRunsOnSubFrames(true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    script.setWorldId(QWebEngineScript::MainWorld);
#endif
    script.setSourceCode(stubJs);
    page->scripts().insert(script);
}
