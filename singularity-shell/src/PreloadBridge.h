#pragma once

#include <QObject>
#include <QString>

class QWebEnginePage;

// PreloadBridge — replacement for the vendor's Electron `window.preloadApi`
// bridge, per qt-tz.md FR-7 / §6.5.
//
// Two layers:
//  1) an injected JS stub (DocumentCreation, main world) that defines
//     window.preloadApi as a Proxy of safe async no-ops, so vendor code paths
//     touching desktop IPC never throw;
//  2) a QWebChannel object ("preloadBridge") backing the small set of
//     operations we deliberately support (window controls, zoom, about info).
//
// The JS stub Object.assign()s the real implementations onto the proxy target
// once the channel is ready; everything else stays a logged no-op.
class PreloadBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString assetVersion READ assetVersion CONSTANT)
public:
    explicit PreloadBridge(QObject* parent = nullptr);

    void setVersions(QString appVer, QString assetVer);

    QString appVersion() const { return m_appVersion; }
    QString assetVersion() const { return m_assetVersion; }

    // Injected by MainWindow on the main page (and popup pages) at startup.
    void installOn(QWebEnginePage* page);

    // JS-callable surface (QWebChannel slots). Keep names stable.
public slots:
    void windowMinimize();
    void windowMaximizeToggle();
    void windowClose();
    void setZoomFactor(double factor);
    double zoomFactor() const;
    void openExternal(const QString& url);

signals:
    // Requested by JS; MainWindow performs the action.
    void minimizeRequested();
    void maximizeToggleRequested();
    void closeRequested();
    void zoomChangeRequested(double factor);
    void externalOpenRequested(const QUrl& url);

private:
    QString m_appVersion;
    QString m_assetVersion;
};
