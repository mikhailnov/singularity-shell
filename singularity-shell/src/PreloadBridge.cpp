#include "PreloadBridge.h"

#include <QFile>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>

PreloadBridge::PreloadBridge(QObject* parent) : QObject(parent) {}

void PreloadBridge::setVersions(QString appVer, QString assetVer)
{
    m_appVersion = std::move(appVer);
    m_assetVersion = std::move(assetVer);
}

void PreloadBridge::windowMinimize()        { emit minimizeRequested(); }
void PreloadBridge::windowMaximizeToggle()  { emit maximizeToggleRequested(); }
void PreloadBridge::windowClose()           { emit closeRequested(); }
void PreloadBridge::setZoomFactor(double f) { emit zoomChangeRequested(f); }
void PreloadBridge::openExternal(const QString& url) { emit externalOpenRequested(QUrl(url)); }

double PreloadBridge::zoomFactor() const
{
    if (auto* p = qobject_cast<QWebEnginePage*>(parent()))
        return p->zoomFactor();
    return 1.0;
}

void PreloadBridge::installOn(QWebEnginePage* page)
{
    setParent(page);

    // 1) WebChannel with this object.
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("preloadBridge"), this);
    page->setWebChannel(channel);

    // 2) Inject stub + channel bootstrap at document creation, main world.
    //    qwebchannel.js is inlined into the script source: loading it as a
    //    subresource from qrc:// would be cross-origin for the sg:// page.
    QFile qc(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    QString qwebchannelJs;
    if (qc.open(QIODevice::ReadOnly))
        qwebchannelJs = QString::fromUtf8(qc.readAll());

    const QString stubJs = qwebchannelJs + QStringLiteral(R"JS(
;(function () {
    'use strict';
    var logged = {};
    // Universal recursive stub: any property chain is truthy and any call is
    // an async no-op. The vendor renderer treats "preloadApi exists" as
    // "desktop build" and uses e.g. preloadApi.ipcRenderer.send(...) — with a
    // plain-function stub those chains threw (TypeError: ...send is not a
    // function); with the recursive proxy they resolve to logged no-ops.
    function makeUniversal(path) {
        var fn = function () {};
        return new Proxy(fn, {
            get: function (t, prop) {
                if (prop === '__isStub') return true;
                if (prop === 'then') return undefined;        // never a thenable
                if (prop === Symbol.toPrimitive) return function () { return ''; };
                if (prop in t) return t[prop];                // real impls assigned below
                return makeUniversal(path + '.' + String(prop));
            },
            apply: function (t, thisArg, args) {
                if (!logged[path]) {
                    logged[path] = 1;
                    console.warn('[singularity-shell] preloadApi stub call:', path, args);
                }
                return Promise.resolve(null);
            },
            set: function (t, prop, value) { t[prop] = value; return true; }
        });
    }
    var target = {};                    // real implementations land here
    var stubRoot = makeUniversal('preloadApi');
    window.preloadApi = new Proxy(stubRoot, {
        get: function (s, prop) {
            if (prop in target) return target[prop];
            if (prop === '__isStub') return true;
            if (prop === 'then') return undefined;
            return stubRoot[prop];
        },
        set: function (s, prop, value) { target[prop] = value; return true; }
    });
    window.IS_TEST = false;

    if (typeof QWebChannel !== 'undefined' && typeof qt !== 'undefined'
            && qt.webChannelTransport) {
        new QWebChannel(qt.webChannelTransport, function (channel) {
            var b = channel.objects.preloadBridge;
            if (!b) return;
            // Real implementations backed by the C++ PreloadBridge: they land
            // in `target` and shadow the recursive stub for these names.
            target.windowMinimize       = function () { b.windowMinimize(); };
            target.windowMaximizeToggle = function () { b.windowMaximizeToggle(); };
            target.windowClose          = function () { b.windowClose(); };
            target.setZoomFactor        = function (f) { b.setZoomFactor(f); };
            target.getZoomFactor        = function () { return b.zoomFactor; };
            target.openExternal         = function (u) { b.openExternal(u); };
            target.shellInfo = { appVersion: b.appVersion, assetVersion: b.assetVersion };
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
