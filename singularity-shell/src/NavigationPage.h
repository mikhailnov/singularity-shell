#pragma once

#include <QUrl>
#include <QWebEnginePage>

class QWebEngineProfile;

// NavigationPage — navigation/popup policy per qt-tz.md §6.6.
//
// Two flavors:
//  - main page: sg:// and *.singularity-app.com navigations allowed in-app;
//    external link-clicks go to the system browser.
//  - auth popup pages (created via createWindow): permissive — OAuth providers
//    (accounts.google.com, login.microsoftonline.com, ...) must be able to
//    complete their redirect chains inside the popup.
class NavigationPage : public QWebEnginePage
{
    Q_OBJECT
public:
    explicit NavigationPage(QWebEngineProfile* profile, bool permissivePopups,
                            QObject* parent = nullptr);

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
};
