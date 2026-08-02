#pragma once

#include <QMainWindow>

class QCloseEvent;
class QSystemTrayIcon;
class QWebEngineView;
class QWebEngineProfile;
class QLabel;
class NavigationPage;
class PreloadBridge;

// MainWindow — single main view hosting sg://renderer/index.html,
// status indicator for background updates, About dialog, zoom shortcuts,
// system tray with optional WebEngine suspension.
// See qt-tz.md FR-6..FR-13, §6.6.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWebEngineProfile* profile, PreloadBridge* bridge,
               const QString& assetVersion, bool startHidden = false,
               QWidget* parent = nullptr);

    QWebEngineView* view() const { return m_view; }
    NavigationPage* page() const { return m_page; }

    void loadApp();        // sg://renderer/index.html
    void loadBootstrap();  // qrc bootstrap page (FR-10)

public slots:
    void setUpdateStatus(const QString& text);
    void showAbout();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
private:
    void buildMenus();
    void setZoom(double factor);
    void createWebEngine();
    void setupTray();
    void suspendWebEngine();
    void resumeWebEngine();

    QWebEngineView* m_view = nullptr;
    NavigationPage* m_page = nullptr;
    QWebEngineProfile* m_profile = nullptr;
    PreloadBridge* m_bridge = nullptr;
    QLabel* m_updateStatus = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
    QAction* m_quitAction = nullptr;
    bool m_startHidden = false;
    QString m_assetVersion;
};
