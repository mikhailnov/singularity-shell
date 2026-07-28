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
    setWindowTitle(QStringLiteral("Singularity"));
    resize(1280, 800);

    m_view = new QWebEngineView(this);
    m_page = new NavigationPage(profile, /*permissivePopups=*/false, m_view);
    m_view->setPage(m_page);
    setCentralWidget(m_view);

    // External links from the main page go to the system browser (§6.6).
    connect(m_page, &NavigationPage::externalUrlRequested, this,
            [](const QUrl& url) { QDesktopServices::openUrl(url); });

    // Bridge wiring (FR-7).
    if (bridge) {
        bridge->installOn(m_page);
        connect(bridge, &PreloadBridge::minimizeRequested, this, &QWidget::showMinimized);
        connect(bridge, &PreloadBridge::maximizeToggleRequested, this, [this] {
            setWindowState(windowState() ^ Qt::WindowMaximized);
        });
        connect(bridge, &PreloadBridge::closeRequested, this, &QWidget::close);
        connect(bridge, &PreloadBridge::zoomChangeRequested, this,
                [this](double f) { m_page->setZoomFactor(qBound(0.25, f, 5.0)); });
        connect(bridge, &PreloadBridge::externalOpenRequested, this,
                [](const QUrl& u) { QDesktopServices::openUrl(u); });
    }

    // Non-intrusive update indicator in the status bar (FR-9: no popups).
    m_updateStatus = new QLabel(this);
    m_updateStatus->setVisible(false);
    statusBar()->addPermanentWidget(m_updateStatus);

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
    m_updateStatus->setVisible(!text.isEmpty());

    // Bootstrap page progress (FR-10): forward to the page if it is showing.
    if (m_page->url().scheme() == QStringLiteral("qrc")) {
        const QString jsText = QString::fromUtf8(
            QJsonDocument::fromVariant(text).toJson(QJsonDocument::Compact));  // quoted+escaped
        m_page->runJavaScript(QStringLiteral(
            "var e=document.getElementById('status');"
            "if(e){e.textContent=") + jsText + QStringLiteral(";}"));
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
