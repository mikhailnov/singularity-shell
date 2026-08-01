#include "SgSchemeHandler.h"

#include <cstdio>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QWebEngineUrlRequestJob>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrlQuery>
#include "NetLog.h"
// Prepended to the vendor service worker (build/sw.js). The no-CORS iCal
// endpoint singularity-app.com/ical/ is CORS-preflighted, so it cannot be
// satisfied by a redirect (CORS forbids redirecting a preflight) and we cannot
// inject response headers. Instead, rewrite those URLs to our same-origin
// sg:// relay here, so the SW issues a preflight-free fetch; proxyRequest then
// fetches the original URL natively. skipWaiting()+clients.claim() activate the
// patched SW without a manual close-all-clients dance.
static const char kSwPatch[] = R"JS(
;(function(){
'use strict';
var _f = self.fetch.bind(self);
self.fetch = function(input, init){
  try {
    var u = (typeof input === 'string') ? input : (input && input.url) || '';
    if (u.indexOf('singularity-app.com/ical/') !== -1 ||
        u.indexOf('singularity-app.ru/ical/') !== -1) {
      var nu = 'sg://renderer/__proxy__?u=' + encodeURIComponent(u);
      input = (typeof input === 'string' || !input) ? nu : new Request(nu, input);
    }
  } catch (e) {}
  return _f(input, init);
};
self.addEventListener('install', function(){ self.skipWaiting(); });
self.addEventListener('activate', function(e){ e.waitUntil(self.clients.claim()); });
})();
)JS";


SgSchemeHandler::SgSchemeHandler(QString assetRoot, QObject* parent)
    : QWebEngineUrlSchemeHandler(parent)
    , m_root(QDir(assetRoot).canonicalPath())
{
    m_nam = new QNetworkAccessManager(this);
}

QString SgSchemeHandler::resolveFile(const QString& urlPath) const
{
    QString rel = urlPath;
    if (rel.isEmpty() || rel == QStringLiteral("/"))
        rel = QStringLiteral("index.html");
    else if (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);

    const QString cleaned = QDir::cleanPath(rel);
    if (cleaned.startsWith(QStringLiteral("..")) || cleaned.contains(QStringLiteral("/../")))
        return {};

    const QString candidate = m_root + QStringLiteral("/build/") + cleaned;

    QFileInfo fi(candidate);
    if (!fi.isFile())
        return {};
    if (fi.isSymLink())
        return {};
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.startsWith(m_root + QStringLiteral("/build/")))
        return {};
    return canonical;
}

QByteArray SgSchemeHandler::mimeFor(const QString& path)
{
    static const QHash<QString, QByteArray> overrides = {
        {QStringLiteral("html"), "text/html; charset=utf-8"},
        {QStringLiteral("js"),   "text/javascript"},
        {QStringLiteral("mjs"),  "text/javascript"},
        {QStringLiteral("css"),  "text/css"},
        {QStringLiteral("json"), "application/json"},
        {QStringLiteral("map"),  "application/json"},
        {QStringLiteral("wasm"), "application/wasm"},
        {QStringLiteral("svg"),  "image/svg+xml"},
        {QStringLiteral("woff"), "font/woff"},
        {QStringLiteral("woff2"),"font/woff2"},
        {QStringLiteral("ttf"),  "font/ttf"},
        {QStringLiteral("png"),  "image/png"},
        {QStringLiteral("gif"),  "image/gif"},
        {QStringLiteral("ico"),  "image/x-icon"},
        {QStringLiteral("txt"),  "text/plain; charset=utf-8"},
        {QStringLiteral("crt"),  "application/x-x509-ca-cert"},
    };
    const QString ext = QFileInfo(path).suffix().toLower();
    const auto it = overrides.constFind(ext);
    if (it != overrides.constEnd())
        return it.value();
    static QMimeDatabase db;
    return db.mimeTypeForFile(path).name().toUtf8();
}

void SgSchemeHandler::requestStarted(QWebEngineUrlRequestJob* job)
{
    if (job->requestUrl().host() != QStringLiteral("renderer")) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // sg://renderer/__proxy__?u=<url> — native CORS-free relay; see proxyRequest.
    if (job->requestUrl().path() == QStringLiteral("/__proxy__")) {
        proxyRequest(job);
        return;
    }

    const QString filePath = resolveFile(job->requestUrl().path());
    if (filePath.isEmpty()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    auto* file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    auto* buffer = new QBuffer;
    QByteArray data = file->readAll();
    // Inject a small prepend-patch into the vendor service worker. The patch
    // only reroutes its no-CORS iCal fetches to the sg:// relay; every other
    // fetch in sw.js is passed through unchanged.
    if (filePath.endsWith(QLatin1String("/sw.js")))
        data.prepend(kSwPatch);
    buffer->setData(data);
    file->close();
    delete file;
    buffer->open(QIODevice::ReadOnly);
    QObject::connect(job, &QObject::destroyed, buffer, &QObject::deleteLater);

    job->reply(mimeFor(filePath), buffer);
}

void SgSchemeHandler::applyCors(QWebEngineUrlRequestJob* job)
{
    QMultiMap<QByteArray, QByteArray> h;
    h.insert(QByteArrayLiteral("Access-Control-Allow-Origin"), QByteArrayLiteral("*"));
    h.insert(QByteArrayLiteral("Access-Control-Allow-Methods"), QByteArrayLiteral("GET, POST, OPTIONS"));
    h.insert(QByteArrayLiteral("Access-Control-Allow-Headers"), QByteArrayLiteral("*"));
    job->setAdditionalResponseHeaders(h);
}

void SgSchemeHandler::proxyRequest(QWebEngineUrlRequestJob* job)
{
    const QUrl target = QUrlQuery(job->requestUrl())
                            .queryItemValue(QStringLiteral("u"), QUrl::FullyDecoded);
    const QByteArray method = job->requestMethod();

    // CORS preflight → approve.
    if (method == QByteArrayLiteral("OPTIONS")) {
        applyCors(job);
        auto* buf = new QBuffer;
        buf->open(QIODevice::ReadOnly);
        QObject::connect(job, &QObject::destroyed, buf, &QObject::deleteLater);
        job->reply(QByteArrayLiteral("text/plain"), buf);
        return;
    }
    if (!target.isValid()
            || (target.scheme() != QStringLiteral("http")
                && target.scheme() != QStringLiteral("https"))) {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }

    // CRITICAL security boundary: this relay must NEVER become an open proxy.
    // Only vendor hosts are allowed, so forwarded Cookie/Authorization can only
    // ever reach their legitimate owner (singularity-app.com/.ru) and arbitrary
    // / internal / localhost URLs (SSRF) are refused.
    const QString host = target.host();
    const bool vendorHost =
        host == QLatin1String("singularity-app.com")
        || host.endsWith(QLatin1String(".singularity-app.com"))
        || host == QLatin1String("singularity-app.ru")
        || host.endsWith(QLatin1String(".singularity-app.ru"));
    if (!vendorHost) {
        // Surface rejections on the console so a misconfigured or abused proxy
        // URL is immediately visible when debugging (cf. VendorApiInterceptor).
        fprintf(stderr, "[sg-proxy] refused non-vendor host in relay URL: %s\n",
                qUtf8Printable(target.toString()));
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }

    if (NetLog::enabled())
        fprintf(stderr, "[sg-proxy] %s %s\n",
                method.constData(), qUtf8Printable(target.toString()));

    // Fetch the target natively (Qt network stack — never re-enters Chromium's
    // interceptor, so no redirect loop) and relay the body to the renderer as a
    // same-origin sg:// response, which is exempt from CORS.
    QNetworkRequest req(target);
    // Forward a safe subset of the original request headers. With the vendor-host
    // restriction above, these can only ever reach singularity-app.com/.ru, so
    // forwarding them is safe — but the list is kept deliberately minimal.
    //
    // NOTE (unverified): "Authorization" is the user's JWT ("Bearer <token>",
    // produced by JwtAuthProvider) and "Cookie" the session cookie. They are
    // forwarded defensively, but their real need for the iCal /ical/ endpoint is
    // NOT confirmed: that endpoint is a plain GET CORS proxy, whereas user auth
    // normally happens via a separate authenticated gRPC-web call
    // (INTEGRATION_ICAL_ADD). They are probably absent from this request anyway.
    // If a Network-tab check shows they are unused here, drop them for strict
    // least-privilege.
    static const QSet<QByteArray> allow = {
        "Authorization", "Cookie", "Accept", "Accept-Language", "Accept-Encoding",
        "Content-Type", "User-Agent", "If-None-Match", "If-Modified-Since"
    };
    for (auto it = job->requestHeaders().constBegin();
         it != job->requestHeaders().constEnd(); ++it) {
        if (allow.contains(it.key()))
            req.setRawHeader(it.key(), it.value());
    }

    QNetworkReply* reply = (method == QByteArrayLiteral("GET") || method.isEmpty())
        ? m_nam->get(req)
        : m_nam->sendCustomRequest(req, method);  // body not forwarded (iCal is GET)
    QObject::connect(job, &QObject::destroyed, reply, [reply] {
        reply->abort();
        reply->deleteLater();
    });
    QObject::connect(reply, &QNetworkReply::finished, job, [job, reply, target] {
        reply->deleteLater();
        if (NetLog::enabled()) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            fprintf(stderr, "[sg-proxy] HTTP %d  (%s)  %s\n", code,
                    reply->error() == QNetworkReply::NoError ? "ok"
                        : qPrintable(reply->errorString()),
                    qUtf8Printable(target.toString()));
        }
        if (reply->error() != QNetworkReply::NoError) {
            job->fail(QWebEngineUrlRequestJob::RequestFailed);
            return;
        }
        auto* buf = new QBuffer;
        buf->setData(reply->readAll());
        buf->open(QIODevice::ReadOnly);
        QByteArray ct = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (ct.isEmpty())
            ct = QByteArrayLiteral("application/octet-stream");
        applyCors(job);
        QObject::connect(job, &QObject::destroyed, buf, &QObject::deleteLater);
        job->reply(ct, buf);
    });
}
