#pragma once

#include <QObject>
#include <QString>

#include "AssetStore.h"

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QTimer;

// UpdateController — background, silent, staged asset updater (qt-tz.md FR-9,
// §6.7). Never interrupts a running instance; never breaks startup.
//
// Lifecycle:
//   start()                    → schedule a one-shot background check
//   check()                    → Snap Store API query (rate-limited 1/day)
//   download(url, sha)         → to <dataDir>/tmp/<ver>-r<rev>.snap.part
//   verifyAndStage()           → sha3-384 check, extract via fetch-assets.sh,
//                                manifest validation, atomic symlink switch
// Signals drive the (non-intrusive) status indicator and bootstrap page.
class UpdateController : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Checking, Downloading, Verifying, Staged, UpToDate, Failed };
    Q_ENUM(State)

    UpdateController(AssetStore* store, QString dataDir, QString helperScript,
                     QObject* parent = nullptr);

    // Normal mode: randomized delay + 24 h rate limit (FR-9).
    void start();
    // Bootstrap mode (FR-10): run immediately, ignore the rate limit.
    void startImmediately();

private:
    bool m_bootstrap = false;

    State state() const { return m_state; }
    QString stagedVersion() const { return m_stagedVersion; }

signals:
    void stateChanged(UpdateController::State state, const QString& detail);
    // 0..100 during Downloading; -1 = indeterminate.
    void progressChanged(int percent);
    // Emitted after a version was fully staged (active on next start).
    void versionStaged(const QString& version, const QString& versionDir);

private slots:
    void check();
    void onApiFinished();
    void onDownloadReadyRead();
    void onDownloadFinished();
    void onExtractFinished(int exitCode, int exitStatus);

private:
    void setState(State s, const QString& detail = {});
    void fail(const QString& why);       // log + backoff; never throws
    bool rateLimited() const;
    void beginDownload(const QString& url, const QString& expectedSha,
                       const QString& version, int revision);
    bool verifySha(const QString& file, const QString& expectedShaHex) const;
    void cleanupTemp();

    AssetStore* m_store;
    QString m_dataDir;         // $XDG_DATA_HOME/singularity-shell
    QString m_helperScript;    // fetch-assets.sh path
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_apiReply = nullptr;
    QNetworkReply* m_dlReply = nullptr;
    QFile* m_dlFile = nullptr;
    QTimer* m_timer = nullptr;

    State m_state = State::Idle;
    QString m_stagedVersion;

    // In-flight candidate metadata.
    QString m_candVersion;
    int m_candRevision = 0;
    QString m_candSha;
    QString m_candUrl;
    QString m_snapPath;        // completed .snap awaiting extraction
    QString m_destDir;         // assets/<ver>-r<rev>/
};
