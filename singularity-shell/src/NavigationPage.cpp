#include "NavigationPage.h"

#include <QWebEngineCertificateError>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNav, "shell.nav")

NavigationPage::NavigationPage(QWebEngineProfile* profile, bool permissivePopups,
                               QObject* parent)
    : QWebEnginePage(profile, parent)
    , m_permissive(permissivePopups)
{
    // Qt 6: certificate errors arrive via a signal; the default action is to
    // reject — we just log (never silently accept).
    connect(this, &QWebEnginePage::certificateError, this,
            [](const QWebEngineCertificateError& error) {
        qCWarning(lcNav) << "certificate error for" << error.url()
                         << ":" << error.description() << "(rejected)";
    });
}

bool NavigationPage::isVendorHost(const QString& host)
{
    return host == QStringLiteral("singularity-app.com")
        || host.endsWith(QStringLiteral(".singularity-app.com"))
        || host == QStringLiteral("singularity-app.ru")
        || host.endsWith(QStringLiteral(".singularity-app.ru"));
}

bool NavigationPage::acceptNavigationRequest(const QUrl& url, NavigationType type,
                                             bool isMainFrame)
{
    const QString scheme = url.scheme();

    if (scheme == QStringLiteral("sg"))
        return true;                       // our shell origin
    if (scheme == QStringLiteral("qrc"))
        return true;                       // built-in pages (bootstrap, FR-10)
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http"))
        return false;                      // unknown schemes: ignore (deep links, P-3)

    if (m_permissive)
        return true;                       // auth popups: let OAuth chains run

    if (isVendorHost(url.host()))
        return true;

    // External hosts from the main page: delegate link-clicks to the system
    // browser; allow everything else (redirects, subresources are not routed
    // through here for http(s) anyway).
    if (type == NavigationTypeLinkClicked && isMainFrame) {
        emit externalUrlRequested(url);
        return false;
    }
    return true;
}

QWebEnginePage* NavigationPage::createWindow(WebWindowType type)
{
    Q_UNUSED(type);
    // Auth / dialog popups must share the profile (cookies, window.opener) —
    // FR-6. Create a real top-level view hosting a permissive page so OAuth
    // redirect chains and window.opener work end to end.
    auto* view = new QWebEngineView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->resize(520, 720);

    auto* page = new NavigationPage(profile(), /*permissivePopups=*/true, view);
    view->setPage(page);
    QObject::connect(page, &QWebEnginePage::windowCloseRequested,
                     view, &QWidget::close);
    view->setWindowTitle(tr("Singularity — sign in"));
    view->show();
    qCDebug(lcNav) << "auth/dialog popup opened";
    return page;
}


