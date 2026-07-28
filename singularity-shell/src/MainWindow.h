#pragma once

#include <QMainWindow>

class QCloseEvent;
class QWebEngineView;
class QWebEngineProfile;
class QLabel;
class NavigationPage;
class PreloadBridge;

// MainWindow — single main view hosting sg://renderer/index.html,
// status indicator for background updates, About dialog, zoom shortcuts.
// See qt-tz.md FR-6..FR-13, §6.6.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWebEngineProfile* profile, PreloadBridge* bridge,
               const QString& assetVersion, QWidget* parent = nullptr);

    QWebEngineView* view() const { return m_view; }
    NavigationPage* page() const { return m_page; }

    void loadApp();        // sg://renderer/index.html
    void loadBootstrap();  // qrc bootstrap page (FR-10)

public slots:
    void setUpdateStatus(const QString& text);
    void showAbout();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();

    QWebEngineView* m_view = nullptr;
    NavigationPage* m_page = nullptr;
    QLabel* m_updateStatus = nullptr;
    QString m_assetVersion;
};
