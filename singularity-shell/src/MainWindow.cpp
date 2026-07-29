#include "MainWindow.h"

#include "NavigationPage.h"
#include "PreloadBridge.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QWebEngineProfile>
#include <QWebEngineView>

MainWindow::MainWindow(QWebEngineProfile* profile, PreloadBridge* bridge,
                       const QString& assetVersion, QWidget* parent)
    : QMainWindow(parent)
    , m_assetVersion(assetVersion)
{
    setWindowTitle(tr("Singularity"));
    resize(1280, 800);

    m_view = new QWebEngineView(this);
    m_page = new NavigationPage(profile, /*permissivePopups=*/false, bridge, m_view);
    m_view->setPage(m_page);
    setCentralWidget(m_view);

    // External links from the main page go to the system browser (§6.6).
    connect(m_page, &NavigationPage::externalUrlRequested, this,
            [](const QUrl& url) { QDesktopServices::openUrl(url); });

    // Bridge wiring (FR-7). Signal connections are now set up per-page in
    // installOn(), so each window (main + popups) controls itself.
    if (bridge)
        bridge->installOn(m_page);

    // Non-intrusive update indicator in the status bar (FR-9: no popups).
    // Hidden by default — only shown when there's text to display.
    m_updateStatus = new QLabel(this);
    statusBar()->addPermanentWidget(m_updateStatus);
    statusBar()->hide();

    // Zoom shortcuts.
    auto addZoomShortcut = [this](const QKeySequence& seq, double delta) {
        auto* sc = new QShortcut(seq, this);
        connect(sc, &QShortcut::activated, this, [this, delta] {
            m_page->setZoomFactor(qBound(0.25, m_page->zoomFactor() + delta, 5.0));
        });
    };
    addZoomShortcut(QKeySequence::ZoomIn, 0.1);
    addZoomShortcut(QKeySequence::ZoomOut, -0.1);
    auto* reset = new QShortcut(QKeySequence(QStringLiteral("Ctrl+0")), this);
    connect(reset, &QShortcut::activated, this, [this] { m_page->setZoomFactor(1.0); });

    buildMenus();

    // Restore geometry.
    QSettings s;
    const QByteArray geo = s.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
}

void MainWindow::buildMenus()
{
    QMenu* appMenu = menuBar()->addMenu(tr("&File"));
    QAction* about = appMenu->addAction(tr("&About"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
    QAction* quit = appMenu->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);

    QMenu* diagMenu = menuBar()->addMenu(tr("&Diagnostics"));
    diagMenu->addAction(tr("chrome://gpu"), this, [] { NavigationPage::openDiagnostics(QStringLiteral("chrome://gpu")); });
    diagMenu->addAction(tr("chrome://net-internals"), this, [] { NavigationPage::openDiagnostics(QStringLiteral("chrome://net-internals")); });
    diagMenu->addAction(tr("chrome://serviceworker-internals"), this, [] { NavigationPage::openDiagnostics(QStringLiteral("chrome://serviceworker-internals")); });
    diagMenu->addAction(tr("chrome://tracing"), this, [] { NavigationPage::openDiagnostics(QStringLiteral("chrome://tracing")); });
}

void MainWindow::loadApp()
{
    m_page->load(QUrl(QStringLiteral("sg://renderer/index.html")));
}

void MainWindow::loadBootstrap()
{
    m_page->load(QUrl(QStringLiteral("qrc:///html/bootstrap.html")));
}

void MainWindow::setUpdateStatus(const QString& text)
{
    m_updateStatus->setText(text);
    statusBar()->setVisible(!text.isEmpty());

    // Bootstrap page progress (FR-10): forward to the page if it is showing.
    // Use JSON.stringify for safe JS string escaping — never concatenate
    // raw user-facing text into a <script> context.
    if (m_page->url().scheme() == QStringLiteral("qrc")) {
        const QByteArray safe = QJsonDocument::fromVariant(
            QVariantList{QVariant(text)}).toJson(QJsonDocument::Compact);
        // safe is now ["..."] with properly escaped content.
        // Extract just the inner string by stripping the array brackets.
        const QByteArray inner = safe.mid(1, safe.size() - 2);
        m_page->runJavaScript(QStringLiteral(
            "var e=document.getElementById('status');"
            "if(e){e.textContent=") + QString::fromUtf8(inner) + QStringLiteral(";}"));
    }
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Singularity shell"),
        tr("<b>Singularity shell</b> %1<br/>"
           "Asset version: %2<br/>"
           "Data dir: %3<br/><br/>"
           "Unofficial QtWebEngine wrapper. SingularityApp itself is "
           "proprietary software by its vendor.")
            .arg(QStringLiteral(APP_VERSION),
                 m_assetVersion.isEmpty() ? tr("none (bootstrap)") : m_assetVersion,
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}
