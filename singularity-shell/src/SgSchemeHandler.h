#pragma once

#include <QWebEngineUrlSchemeHandler>

class QString;

// SgSchemeHandler — serves sg://renderer/<path> from the resolved asset
// directory (<assetRoot>/build/<path>), per qt-tz.md FR-1/§6.3.
//
// Threading: requestStarted() runs on QtWebEngine's IO thread. The handler is
// therefore stateless after construction; it never touches GUI objects.
class SgSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    // assetRoot: absolute path of the active "<version>-r<revision>/" dir.
    // Bound once at startup and never switched under a running instance
    // (staged updates take effect on the next start only — FR-9).
    explicit SgSchemeHandler(QString assetRoot, QObject* parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob* job) override;

    QString assetRoot() const { return m_root; }

private:
    QString resolveFile(const QString& urlPath) const;  // empty => reject
    static QByteArray mimeFor(const QString& path);

    QString m_root;  // canonical, no trailing slash
};
