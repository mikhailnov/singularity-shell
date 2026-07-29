#include "NavigationPage.h"
#include "PreloadBridge.h"

#include <QWebEngineCertificateError>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNav, "shell.nav")

NavigationPage::NavigationPage(QWebEngineProfile* profile, bool permissivePopups,
                               PreloadBridge* bridge, QObject* parent)
    : QWebEnginePage(profile, parent)
    , m_permissive(permissivePopups)
    , m_bridge(bridge)
{
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
        return true;
    if (scheme == QStringLiteral("qrc"))
        return true;
    if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http"))
        return false;

    if (m_permissive)
        return true;

    if (isVendorHost(url.host()))
        return true;

    if (type == NavigationTypeLinkClicked && isMainFrame) {
        emit externalUrlRequested(url);
        return false;
    }
    return true;
}

QWebEnginePage* NavigationPage::createWindow(WebWindowType type)
{
    Q_UNUSED(type);
    auto* view = new QWebEngineView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->resize(1024, 768);

    auto* page = new NavigationPage(profile(), /*permissivePopups=*/true,
                                    m_bridge, view);
    view->setPage(page);

    // Install the bridge so window.preloadApi is available in the new window.
    if (m_bridge)
        m_bridge->installOn(page);

    QObject::connect(page, &QWebEnginePage::windowCloseRequested,
                     view, &QWidget::close);
    view->setWindowTitle(tr("Singularity — new window"));
    view->show();
    qCDebug(lcNav) << "popup/new-window opened";
    return page;
}


