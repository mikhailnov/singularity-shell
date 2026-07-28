#include "SgSchemeHandler.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QWebEngineUrlRequestJob>

SgSchemeHandler::SgSchemeHandler(QString assetRoot, QObject* parent)
    : QWebEngineUrlSchemeHandler(parent)
    , m_root(QDir(assetRoot).canonicalPath())
{
}

QString SgSchemeHandler::resolveFile(const QString& urlPath) const
{
    // Map "/" -> index.html; strip leading slash; normalize; reject traversal.
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
    // Do not follow symlinks out of the asset root (defense in depth; assets
    // are extracted by our own tooling and contain no symlinks at all).
    if (fi.isSymLink())
        return {};
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.startsWith(m_root + QStringLiteral("/build/")))
        return {};
    return canonical;
}

QByteArray SgSchemeHandler::mimeFor(const QString& path)
{
    // Explicit overrides first: system MIME databases vary and the SPA is
    // unforgiving about wrong script/style MIME types (FR-1, §6.3).
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
    // Host check: we only serve the shell origin; anything else on sg:// is a bug.
    if (job->requestUrl().host() != QStringLiteral("renderer")) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
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

    // Read fully into a buffer: reply() requires the device to outlive the job;
    // buffer also gives QtWebEngine a known size. Assets are at most ~10 MB.
    auto* buffer = new QBuffer;
    buffer->setData(file->readAll());
    file->close();
    delete file;
    buffer->open(QIODevice::ReadOnly);
    QObject::connect(job, &QObject::destroyed, buffer, &QObject::deleteLater);

    job->reply(mimeFor(filePath), buffer);
}
