#pragma once

#include <QUrl>
#include <QWebEnginePage>

class QWebEngineProfile;
class PreloadBridge;

// NavigationPage — navigation/popup policy per qt-tz.md §6.6.
//
// Two flavors:
//  - main page: sg:// and *.singularity-app.com navigations allowed in-app;
//    external link-clicks go to the system browser.
//  - auth / new-window popups (created via createWindow): permissive — OAuth
//    providers and new-window opens must complete their flows in the popup.
//    If a PreloadBridge is provided, it is installed on every created page
//    so that window.preloadApi is available in new windows too.
class NavigationPage : public QWebEnginePage
{
    Q_OBJECT
public:
    explicit NavigationPage(QWebEngineProfile* profile, bool permissivePopups,
                            PreloadBridge* bridge = nullptr,
                            QObject* parent = nullptr);

    // Opens a bare diagnostic window on the given profile.
    static void openDiagnostics(QWebEngineProfile* profile, const QString& url);

    static bool isVendorHost(const QString& host);

signals:
    // Emitted when an external link should be opened in the system browser.
    void externalUrlRequested(const QUrl& url);

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type,
                                 bool isMainFrame) override;
    QWebEnginePage* createWindow(WebWindowType type) override;

private:
    bool m_permissive;
    PreloadBridge* m_bridge = nullptr;
};
