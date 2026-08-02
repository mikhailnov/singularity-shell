#include "MainWindow.h"

#include "NavigationPage.h"
#include "PreloadBridge.h"
#include "SettingsPath.h"


#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QProcess>

#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QUrl>
#include <QWebEngineProfile>
#include <QWebEngineView>
MainWindow::MainWindow(QWebEngineProfile* profile, PreloadBridge* bridge,
                       const QString& assetVersion, QWidget* parent)
    : QMainWindow(parent)
    , m_profile(profile)
    , m_bridge(bridge)
    , m_assetVersion(assetVersion)
{
    setWindowTitle(tr("Singularity"));
    resize(1280, 800);

    createWebEngine();

    // Non-intrusive update indicator in the status bar (FR-9: no popups).
    m_updateStatus = new QLabel(this);
    statusBar()->addPermanentWidget(m_updateStatus);
    statusBar()->hide();

    buildMenus();
    setupTray();

    // Restore geometry.
    QSettings s(settingsPath(), QSettings::IniFormat);
    const QByteArray geo = s.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
}

void MainWindow::createWebEngine()
{
    const auto pageBg = []() -> QColor {
        const bool dark = QGuiApplication::styleHints()->colorScheme()
                          == Qt::ColorScheme::Dark;
        return dark ? QColor(0x1a, 0x1a, 0x2e) : QColor(0xf0, 0xf0, 0xf5);
    };

    m_view = new QWebEngineView(this);
    m_page = new NavigationPage(m_profile, /*permissivePopups=*/false, m_bridge, m_view);
    m_page->setBackgroundColor(pageBg());
    m_view->setPage(m_page);
    setCentralWidget(m_view);

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this] {
        const bool dark = QGuiApplication::styleHints()->colorScheme()
                          == Qt::ColorScheme::Dark;
        m_page->setBackgroundColor(dark ? QColor(0x1a, 0x1a, 0x2e)
                                        : QColor(0xf0, 0xf0, 0xf5));
    });

    // External links from the main page go to the system browser.
    connect(m_page, &NavigationPage::externalUrlRequested, this,
            [](const QUrl& url) { QDesktopServices::openUrl(url); });

    // Bridge wiring (FR-7).
    if (m_bridge)
        m_bridge->installOn(m_page);

    // Intercept zoom keys before QWebEngineView (Chromium) consumes them.
    m_view->installEventFilter(this);
}

void MainWindow::buildMenus()
{
    QMenu* appMenu = menuBar()->addMenu(tr("&File"));
    QAction* about = appMenu->addAction(tr("&About"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
    QAction* hideAction = appMenu->addAction(tr("&Hide to tray"), this, [this] {
        hide();
        suspendWebEngine();
    });
    hideAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    m_quitAction = appMenu->addAction(tr("&Quit"), this, [this] {
        if (m_tray)
            m_tray->hide();
        QApplication::quit();
    });
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setMenuRole(QAction::QuitRole);
    QMenu* diagMenu = menuBar()->addMenu(tr("&Diagnostics"));
    diagMenu->addAction(tr("Force reload"), this, [this] {
        m_page->triggerAction(QWebEnginePage::ReloadAndBypassCache);
    });
    QAction* devTools = diagMenu->addAction(tr("&Developer tools"), this, [this] {
        NavigationPage::openDevTools(m_page);
    });
    devTools->setShortcut(QKeySequence(QStringLiteral("F12")));
    diagMenu->addSeparator();
    diagMenu->addAction(QStringLiteral("chrome://gpu"), this, [this] { NavigationPage::openDiagnostics(m_profile, QStringLiteral("chrome://gpu")); });
    diagMenu->addAction(QStringLiteral("chrome://indexeddb-internals"), this, [this] { NavigationPage::openDiagnostics(m_profile, QStringLiteral("chrome://indexeddb-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://net-internals"), this, [this] { NavigationPage::openDiagnostics(m_profile, QStringLiteral("chrome://net-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://serviceworker-internals"), this, [this] { NavigationPage::openDiagnostics(m_profile, QStringLiteral("chrome://serviceworker-internals")); });
    diagMenu->addAction(QStringLiteral("chrome://tracing"), this, [this] { NavigationPage::openDiagnostics(m_profile, QStringLiteral("chrome://tracing")); });
    diagMenu->addSeparator();
    diagMenu->addAction(tr("Clear profile data…"), this, [this] {
        if (QMessageBox::question(this, tr("Clear profile"),
                tr("This will delete all local data: cookies, session, "
                   "IndexedDB, service workers, settings.\n"
                   "Downloaded assets will also be removed and re-downloaded "
                   "from the Snap Store on next launch.\n\n"
                   "The application will restart with a fresh profile."))
            != QMessageBox::Yes)
            return;

        // Hide tray before quit — otherwise MainWindow::closeEvent ignores
        // the close and QApplication::quit() is cancelled.
        if (m_tray)
            m_tray->hide();
        QProcess::startDetached(QApplication::applicationFilePath(),
                                {QStringLiteral("--clear-profile")});
        QApplication::quit();
    });
    QMenu* viewMenu = menuBar()->addMenu(tr("&Zoom"));
    QAction* zoomLabel = viewMenu->addAction(tr("100 %"));
    zoomLabel->setEnabled(false);
    QObject::connect(viewMenu, &QMenu::aboutToShow, this, [zoomLabel, this] {
        zoomLabel->setText(QStringLiteral("%1 %").arg(qRound(m_page->zoomFactor() * 100)));
    });
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { setZoom(m_page->zoomFactor() + 0.1); });
    viewMenu->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { setZoom(m_page->zoomFactor() - 0.1); });
    viewMenu->addAction(tr("&Reset"), QKeySequence(QStringLiteral("Ctrl+0")), this, [this] { setZoom(1.0); });
}

void MainWindow::setZoom(double factor)
{
    factor = qBound(0.25, factor, 5.0);
    m_page->setZoomFactor(factor);
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("ui/zoomFactor"), factor);
}

void MainWindow::loadApp()
{
    m_page->load(QUrl(QStringLiteral("sg://renderer/index.html")));
    // Restore persisted zoom after the page loads.
    connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (!ok) return;
        const double z = QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("ui/zoomFactor"), 1.0).toDouble();
        if (z != 1.0)
            m_page->setZoomFactor(qBound(0.25, z, 5.0));
    }, Qt::SingleShotConnection);
}


void MainWindow::loadBootstrap()
{
    // Inject translated strings after the page loads.
    connect(m_page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (!ok || m_page->url().scheme() != QStringLiteral("qrc"))
            return;
        const auto jsStr = [](const QString& s) {
            const QByteArray json = QJsonDocument::fromVariant(
                QVariantList{QVariant(s)}).toJson(QJsonDocument::Compact);
            return QString::fromUtf8(json.mid(1, json.size() - 2));
        };
        m_page->runJavaScript(QStringLiteral(
            "var e=document.getElementById('bs-title');"
            "if(e)e.textContent=") + jsStr(tr("Downloading Singularity…")) + QStringLiteral(";"
            "e=document.getElementById('bs-body');"
            "if(e)e.textContent=") + jsStr(tr("The application assets were not found on this system."
            " They are being downloaded from the official Snap Store right now"
            " (one-time, ~43 MB extracted)."
            " Next launches will work fully offline.")) + QStringLiteral(";"
            "e=document.getElementById('status');"
            "if(e)e.textContent=") + jsStr(tr("Connecting…")) + QStringLiteral(";"));
    }, Qt::SingleShotConnection);
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
           "Source code: <a href='https://github.com/mikhailnov/singularity-shell'>"
           "github.com/mikhailnov/singularity-shell</a><br/>"
           "Contact: GitHub issues or <a href='mailto:m@mikhailnov.ru'>m@mikhailnov.ru</a>"
           "<br/><br/>"
           "Unofficial QtWebEngine wrapper. SingularityApp itself is "
           "proprietary software by its vendor.")
            .arg(QStringLiteral(APP_VERSION),
                 m_assetVersion.isEmpty() ? tr("none (bootstrap)") : m_assetVersion,
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && obj == m_view) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const int key = ke->key();
        const auto mods = ke->modifiers();
        if ((mods & Qt::ControlModifier) == Qt::ControlModifier) {
            if (key == Qt::Key_Equal || key == Qt::Key_Plus) {
                setZoom(m_page->zoomFactor() + 0.1);
                return true;
            }
            if (key == Qt::Key_Minus) {
                setZoom(m_page->zoomFactor() - 0.1);
                return true;
            }
            if (key == Qt::Key_0) {
                setZoom(1.0);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("ui/geometry"), saveGeometry());

    if (m_tray && m_tray->isVisible()) {
        hide();               // hide window first — required before freeze
        suspendWebEngine();   // then freeze the page
        event->ignore();
    } else {
        QMainWindow::closeEvent(event);
    }
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(QIcon(QStringLiteral(":/tray-icon.svg")));
    m_tray->setToolTip(tr("Singularity"));

    auto* menu = new QMenu(this);
    QAction* showAction = menu->addAction(QString(), this, [this] {
        if (isVisible()) {
            hide();
            suspendWebEngine();
        } else {
            resumeWebEngine();
            show();
            raise();
            activateWindow();
        }
    });
    connect(menu, &QMenu::aboutToShow, this, [this, showAction] {
        showAction->setText(isVisible() ? tr("&Hide") : tr("&Show"));
    });
    menu->addAction(tr("&About"), this, &MainWindow::showAbout);
    menu->addSeparator();
    menu->addAction(tr("&Quit"), qApp, &QApplication::quit);
    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger
            || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible()) {
                hide();
                suspendWebEngine();
            } else {
                resumeWebEngine();
                show();
                raise();
                activateWindow();
            }
        }
    });

    m_tray->show();
}

void MainWindow::suspendWebEngine()
{
    if (!m_page)
        return;

    // Save zoom before freezing.
    QSettings(settingsPath(), QSettings::IniFormat)
        .setValue(QStringLiteral("ui/zoomFactor"), m_page->zoomFactor());

    qInfo() << "tray: freezing page";
    // Freeze the page: stops JS execution (timers, requestAnimationFrame, etc.)
    // while keeping the page in memory for instant resume.
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    m_page->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
#endif
}

void MainWindow::resumeWebEngine()
{
    if (!m_page)
        return;

    qInfo() << "tray: resuming page";
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    m_page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
#endif
}
