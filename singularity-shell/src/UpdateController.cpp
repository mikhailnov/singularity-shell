#include "UpdateController.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>

Q_LOGGING_CATEGORY(lcUpd, "shell.update")

namespace {
constexpr auto kApiUrl = "https://api.snapcraft.io/v2/snaps/info/singularityapp";
constexpr qint64 kRateLimitMs = 24ll * 60 * 60 * 1000;  // NFR-4: once per day
}

UpdateController::UpdateController(AssetStore* store, QString dataDir,
                                   QString helperScript, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_dataDir(std::move(dataDir))
    , m_helperScript(std::move(helperScript))
{
    m_nam = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &UpdateController::check);
}

void UpdateController::start()
{
    // Randomized 10-60 s delay after startup (FR-9): let the UI settle first.
    const int delayMs = 10'000
        + int(QRandomGenerator::global()->bounded(50'000));
    m_timer->start(delayMs);
    qCInfo(lcUpd) << "background check scheduled in" << delayMs / 1000 << "s";
}

void UpdateController::startImmediately()
{
    m_bootstrap = true;
    m_timer->start(0);
}

bool UpdateController::rateLimited() const
{
    QSettings s;
    const QDateTime last = s.value(QStringLiteral("update/lastCheck")).toDateTime();
    return last.isValid()
        && last.msecsTo(QDateTime::currentDateTimeUtc()) < kRateLimitMs;
}

void UpdateController::setState(State s, const QString& detail)
{
    m_state = s;
    emit stateChanged(s, detail);
}

void UpdateController::fail(const QString& why)
{
    qCWarning(lcUpd) << "update failed (silent retry later):" << why;
    cleanupTemp();
    setState(State::Failed, why);
}

void UpdateController::cleanupTemp()
{
    if (m_dlReply) { m_dlReply->abort(); m_dlReply->deleteLater(); m_dlReply = nullptr; }
    if (m_dlFile) {
        const QString name = m_dlFile->fileName();
        m_dlFile->close();
        m_dlFile->deleteLater();
        m_dlFile = nullptr;
        QFile::remove(name);  // .part must never be mistaken for a complete file
    }
}

void UpdateController::check()
{
    if (m_apiReply || m_dlReply)
        return;  // already in flight

    if (!m_bootstrap && rateLimited()) {  // FR-10: bootstrap ignores the limit
        qCInfo(lcUpd) << "skipped: checked less than 24 h ago";
        return;
    }
    QSettings().setValue(QStringLiteral("update/lastCheck"),
                         QDateTime::currentDateTimeUtc());

    setState(State::Checking);
    QNetworkRequest req{QUrl(QString::fromLatin1(kApiUrl))};
    req.setRawHeader("Snap-Device-Series", "16");
    req.setTransferTimeout(30'000);
    m_apiReply = m_nam->get(req);
    connect(m_apiReply, &QNetworkReply::finished,
            this, &UpdateController::onApiFinished);
}

void UpdateController::onApiFinished()
{
    auto* reply = m_apiReply;
    m_apiReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("API request: %1").arg(reply->errorString()));
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonArray map = root.value(QStringLiteral("channel-map")).toArray();

    // Host arch -> snap arch label (§6.7 step 2).
    const QString host = QSysInfo::buildCpuArchitecture();
    const QString snapArch = host == QStringLiteral("x86_64")  ? QStringLiteral("amd64")
                           : host == QStringLiteral("aarch64") ? QStringLiteral("arm64")
                           : QString();

    QString bestVersion, bestUrl, bestSha;
    int bestRevision = 0;
    for (const auto& v : map) {
        const QJsonObject e = v.toObject();
        const QJsonObject ch = e.value(QStringLiteral("channel")).toObject();
        if (ch.value(QStringLiteral("track")).toString() != QStringLiteral("latest")
            || ch.value(QStringLiteral("risk")).toString() != QStringLiteral("stable")
            || (!snapArch.isEmpty()
                && ch.value(QStringLiteral("architecture")).toString() != snapArch))
            continue;
        const int rev = e.value(QStringLiteral("revision")).toInt();
        const QString ver = e.value(QStringLiteral("version")).toString();
        if (bestVersion.isEmpty()
            || AssetStore::compareVersions(ver, bestVersion) > 0
            || (AssetStore::compareVersions(ver, bestVersion) == 0 && rev > bestRevision)) {
            bestVersion = ver;
            bestRevision = rev;
            const QJsonObject dl = e.value(QStringLiteral("download")).toObject();
            bestUrl = dl.value(QStringLiteral("url")).toString();
            bestSha = dl.value(QStringLiteral("sha3-384")).toString();
        }
    }

    if (bestVersion.isEmpty() || bestUrl.isEmpty() || bestSha.isEmpty()) {
        fail(QStringLiteral("no suitable channel-map entry (arch %1)").arg(host));
        return;
    }

    // Compare against the BEST LOCAL set, not the running one (FR-9 step 3).
    const AssetStore::AssetSet local = m_store->bestLocal();
    const bool newer = !local.valid
        || AssetStore::compareVersions(bestVersion, local.version) > 0
        || (AssetStore::compareVersions(bestVersion, local.version) == 0
            && bestRevision > local.revision);
    if (!newer) {
        qCInfo(lcUpd) << "up to date:" << local.version << "r" << local.revision;
        setState(State::UpToDate, local.version);
        return;
    }

    qCInfo(lcUpd) << "new version available:" << bestVersion << "r" << bestRevision
                  << "(local:" << local.version << "r" << local.revision << ")";
    beginDownload(bestUrl, bestSha, bestVersion, bestRevision);
}

void UpdateController::beginDownload(const QString& url, const QString& expectedSha,
                                     const QString& version, int revision)
{
    m_candUrl = url;
    m_candSha = expectedSha;
    m_candVersion = version;
    m_candRevision = revision;

    QDir().mkpath(m_dataDir + QStringLiteral("/tmp"));
    m_dlFile = new QFile(m_dataDir + QStringLiteral("/tmp/%1-r%2.snap.part")
                                     .arg(version).arg(revision));
    if (!m_dlFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("cannot open temp file"));
        return;
    }

    setState(State::Downloading, version);
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(60'000);
    m_dlReply = m_nam->get(req);
    connect(m_dlReply, &QNetworkReply::readyRead,
            this, &UpdateController::onDownloadReadyRead);
    connect(m_dlReply, &QNetworkReply::finished,
            this, &UpdateController::onDownloadFinished);
    connect(m_dlReply, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64 total) {
                emit progressChanged(total > 0 ? int(got * 100 / total) : -1);
            });
}

void UpdateController::onDownloadReadyRead()
{
    if (m_dlReply && m_dlFile)
        m_dlFile->write(m_dlReply->readAll());
}

void UpdateController::onDownloadFinished()
{
    auto* reply = m_dlReply;
    m_dlReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("download: %1").arg(reply->errorString()));
        return;
    }
    // Flush any bytes not yet consumed by readyRead, then close the file.
    if (m_dlFile)
        m_dlFile->write(reply->readAll());
    m_dlFile->flush();
    m_dlFile->close();

    const QString partPath = m_dlFile->fileName();
    m_dlFile->deleteLater();
    m_dlFile = nullptr;

    const QString snapPath = partPath.mid(0, partPath.size() - 5);  // strip ".part"
    if (!QFile::rename(partPath, snapPath)) {
        fail(QStringLiteral("rename of completed download failed"));
        return;
    }
    m_snapPath = snapPath;

    // Verify SHA3-384 (FR-9 step 5).
    setState(State::Verifying, m_candVersion);
    if (!verifySha(m_snapPath, m_candSha)) {
        QFile::remove(m_snapPath);
        fail(QStringLiteral("sha3-384 mismatch"));
        return;
    }

    // Extract via the helper script (FR-9 step 6). Destination: versioned dir.
    const QString dirName = QStringLiteral("%1-r%2").arg(m_candVersion).arg(m_candRevision);
    m_destDir = m_store->userRoot() + QLatin1Char('/') + dirName;
    QDir().mkpath(m_destDir);

    auto* proc = new QProcess(this);
    proc->setProgram(QStringLiteral("bash"));
    proc->setArguments({m_helperScript, QStringLiteral("extract"),
                        m_snapPath, m_destDir});
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdateController::onExtractFinished);
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->start();
    qCInfo(lcUpd) << "extracting to" << m_destDir;
}

bool UpdateController::verifySha(const QString& file, const QString& expectedShaHex) const
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash h(QCryptographicHash::Sha3_384);
    if (!h.addData(&f))
        return false;
    return h.result().toHex().compare(expectedShaHex.toLatin1(),
                                      Qt::CaseInsensitive) == 0;
}

void UpdateController::onExtractFinished(int exitCode, int exitStatus)
{
    QFile::remove(m_snapPath);  // snap no longer needed either way

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QDir(m_destDir).removeRecursively();
        fail(QStringLiteral("fetch-assets.sh extract exited with %1").arg(exitCode));
        return;
    }

    // Manifest: written by the helper for `latest`; for `extract` we write it
    // here so the dir is self-describing (FR-9 step 6).
    const QString manifestPath = m_destDir + QStringLiteral("/manifest.json");
    if (!QFile::exists(manifestPath)) {
        QFile mf(manifestPath);
        if (mf.open(QIODevice::WriteOnly)) {
            const QJsonObject o{
                {QStringLiteral("name"), QStringLiteral("singularityapp")},
                {QStringLiteral("version"), m_candVersion},
                {QStringLiteral("revision"), m_candRevision},
                {QStringLiteral("sha3_384"), m_candSha},
                {QStringLiteral("source"), m_candUrl},
                {QStringLiteral("fetchedAt"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            };
            mf.write(QJsonDocument(o).toJson());
        }
    }

    // Validate before staging (FR-9 step 6): never activate a broken set.
    const AssetStore::AssetSet staged = m_store->probe(m_destDir);
    if (!staged.valid) {
        AssetStore::markBad(m_destDir);
        fail(QStringLiteral("staged assets failed validation"));
        return;
    }

    // Atomic switch: current.new -> rename over current (FR-9 step 7).
    const QString linkNew = m_store->userRoot() + QStringLiteral("/current.new");
    const QString linkCur = m_store->userRoot() + QStringLiteral("/current");
    QFile::remove(linkNew);
    if (!QFile::link(m_destDir, linkNew) || !QFile::rename(linkNew, linkCur)) {
        fail(QStringLiteral("symlink switch failed"));
        return;
    }

    m_stagedVersion = m_candVersion;
    setState(State::Staged, m_candVersion);
    emit versionStaged(m_candVersion, m_destDir);
    qCInfo(lcUpd) << m_candVersion << "staged for next start";

    // Prune older user versions, keeping the staged one + one rollback (FR-9).
    // Never touch the currently running set (handled by AssetStore::pruneUserVersions
    // via keepDirs; the running root is passed by MainWindow through settings).
    QSettings s;
    const QString running = s.value(QStringLiteral("runtime/activeAssetDir")).toString();
    m_store->pruneUserVersions({m_destDir, running});
}
