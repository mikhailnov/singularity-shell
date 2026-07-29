#pragma once

#include <QObject>
#include <QString>

class QWebEnginePage;

// PreloadBridge — replacement for the vendor's Electron `window.preloadApi`
// bridge, per qt-tz.md FR-7 / §6.5.
//
// The C++ side exposes slots that the JS stub wires into controller objects
// matching the vendor's actual Electron preload API surface (reverse-engineered
// from build/main/preload.js in the official snap). Each controller mirrors the
// names the vendor app expects, so vendor code paths never throw or hang.
//
// Sync methods (e.g. isMaximized, zoomFactor) use Q_PROPERTY so QWebChannel
// serves them synchronously; async actions use slots.
class PreloadBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString assetVersion READ assetVersion CONSTANT)
    Q_PROPERTY(bool isMaximized READ isMaximized NOTIFY maximizedChanged)
public:
    explicit PreloadBridge(QObject* parent = nullptr);

    void setVersions(QString appVer, QString assetVer);

    QString appVersion() const { return m_appVersion; }
    QString assetVersion() const { return m_assetVersion; }

    // Sync getter exposed as Q_PROPERTY for the vendor's windowController.isMaximized.
    bool isMaximized() const;

    // Injected by MainWindow on the main page (and popup pages) at startup.
    void installOn(QWebEnginePage* page);

    // --- Async slots (QWebChannel → JS Promise) ---
    // These are wired into controller objects matching the vendor's API:
    //   windowController: minimize, maximize, unmaximize, close
    //   zoomController:   zoomIn, zoomOut, zoomReset
    //   urlController:    openExternal
public slots:
    void windowMinimize();
    void windowMaximize();
    void windowUnmaximize();
    void windowClose();
    void zoomIn();
    void zoomOut();
    void zoomReset();
    double zoomFactor() const;
    void openExternal(const QString& url);

signals:
    void minimizeRequested();
    void maximizeToggleRequested();
    void closeRequested();
    void externalOpenRequested(const QUrl& url);
    void maximizedChanged();

private:
    QString m_appVersion;
    QString m_assetVersion;
};
