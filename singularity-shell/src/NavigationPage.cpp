#include "NavigationPage.h"
#include "PreloadBridge.h"

#include <QAction>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QWebEngineCertificateError>
#include <QWebEngineProfile>
#include <QWebEngineView>
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
    // Wrap in QMainWindow for menu bar; no status bar (always hidden).
    auto* win = new QMainWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->resize(1024, 768);
    win->statusBar()->hide();

    QMenu* menu = win->menuBar()->addMenu(tr("&File"));
    QAction* quit = menu->addAction(tr("&Quit"), win, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* diagMenu = win->menuBar()->addMenu(tr("&Diagnostics"));
    diagMenu->addAction(tr("chrome://gpu"), win, [] { openDiagnostics(QStringLiteral("chrome://gpu")); });
    diagMenu->addAction(tr("chrome://net-internals"), win, [] { openDiagnostics(QStringLiteral("chrome://net-internals")); });
    diagMenu->addAction(tr("chrome://serviceworker-internals"), win, [] { openDiagnostics(QStringLiteral("chrome://serviceworker-internals")); });
    diagMenu->addAction(tr("chrome://tracing"), win, [] { openDiagnostics(QStringLiteral("chrome://tracing")); });

    auto* view = new QWebEngineView(win);
    auto* page = new NavigationPage(profile(), /*permissivePopups=*/true,
                                    /*bridge=*/nullptr, view);
    view->setPage(page);
    win->setCentralWidget(view);

    // Each window gets its own PreloadBridge.
    auto* popupBridge = new PreloadBridge(view);
    popupBridge->setVersions(m_bridge ? m_bridge->appVersion() : QString(),
                             m_bridge ? m_bridge->assetVersion() : QString());
    popupBridge->installOn(page);

    QObject::connect(page, &QWebEnginePage::windowCloseRequested,
                     win, &QWidget::close);
    win->setWindowTitle(tr("Singularity — new window"));
    win->show();
    qCDebug(lcNav) << "new window opened";
    return page;
}

void NavigationPage::openDiagnostics(const QString& url)
{
    auto* view = new QWebEngineView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->resize(900, 700);
    view->setWindowTitle(QStringLiteral("Diagnostics — ") + url);
    view->load(QUrl(url));
    view->show();
}


