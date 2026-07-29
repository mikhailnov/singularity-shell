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
#include <QWebEnginePage>
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

    // Everything else: open in the system browser, regardless of frame type.
    emit externalUrlRequested(url);
    return false;
}

QWebEnginePage* NavigationPage::createWindow(WebWindowType type)
{
    Q_UNUSED(type);
    auto* win = new QMainWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->resize(1024, 768);
    win->statusBar()->hide();

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

    // Menus (after page is created so lambdas can capture it).
    QMenu* menu = win->menuBar()->addMenu(tr("&File"));
    QAction* quit = menu->addAction(tr("&Quit"), win, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* diagMenu = win->menuBar()->addMenu(tr("&Diagnostics"));
    diagMenu->addAction(tr("Force reload"), win, [page] {
        page->triggerAction(QWebEnginePage::ReloadAndBypassCache);
    });
    diagMenu->addSeparator();
    auto* p = profile();
    diagMenu->addAction(QStringLiteral("chrome://gpu"), win, [p] { openDiagnostics(p, QStringLiteral("chrome://gpu")); });
    diagMenu->addAction(QStringLiteral("chrome://indexeddb-internals"), win, [p] { openDiagnostics(p, QStringLiteral("chrome://indexeddb-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://net-internals"), win, [p] { openDiagnostics(p, QStringLiteral("chrome://net-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://serviceworker-internals"), win, [p] { openDiagnostics(p, QStringLiteral("chrome://serviceworker-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://tracing"), win, [p] { openDiagnostics(p, QStringLiteral("chrome://tracing")); });

    QObject::connect(page, &QWebEnginePage::windowCloseRequested,
                     win, &QWidget::close);
    win->setWindowTitle(tr("Singularity — new window"));
    win->show();
    qCDebug(lcNav) << "new window opened";
    return page;
}

void NavigationPage::openDiagnostics(QWebEngineProfile* profile, const QString& url)
{
    auto* page = new QWebEnginePage(profile);
    auto* view = new QWebEngineView;
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->setPage(page);
    view->resize(900, 700);
    view->setWindowTitle(QStringLiteral("Diagnostics — ") + url);
    view->load(QUrl(url));
    view->show();
}


