#include "AssetStore.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
constexpr auto kBadMarker = "BAD";
constexpr auto kManifest  = "manifest.json";

bool isNonEmptyFile(const QString& path)
{
    QFileInfo fi(path);
    return fi.isFile() && !fi.isSymLink() && fi.size() > 0;
}
} // namespace

AssetStore::AssetStore(QString userAssetsRoot, QString systemAssetsRoot)
    : m_userRoot(std::move(userAssetsRoot))
    , m_systemRoot(std::move(systemAssetsRoot))
{
}

QList<int> AssetStore::versionKey(const QString& v)
{
    QList<int> out;
    const QStringList parts = v.split(QRegularExpression(QStringLiteral("[^0-9]+")),
                                      Qt::SkipEmptyParts);
    for (const QString& p : parts)
        out << p.toInt();
    return out;
}

int AssetStore::compareVersions(const QString& a, const QString& b)
{
    const QList<int> ka = versionKey(a);
    const QList<int> kb = versionKey(b);
    const int n = qMax(ka.size(), kb.size());
    for (int i = 0; i < n; ++i) {
        const int va = i < ka.size() ? ka[i] : 0;
        const int vb = i < kb.size() ? kb[i] : 0;
        if (va != vb)
            return va < vb ? -1 : 1;
    }
    return 0;
}

AssetStore::AssetSet AssetStore::probeVersionDir(const QString& versionDir) const
{
    AssetSet set;
    set.root = versionDir;

    QFileInfo dirInfo(versionDir);
    if (!dirInfo.isDir() || QFileInfo(versionDir + QLatin1Char('/') + kBadMarker).exists())
        return set;

    // Manifest: tolerate absence only if version can be derived from dirname.
    QString manifestVersion;
    QFile mf(versionDir + QLatin1Char('/') + kManifest);
    if (mf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(mf.readAll()).object();
        manifestVersion = o.value(QStringLiteral("version")).toString();
        set.revision = o.value(QStringLiteral("revision")).toInt();
    }
    if (manifestVersion.isEmpty()) {
        // Fallback: "<version>-r<revision>" directory naming from fetch-assets.sh
        static const QRegularExpression re(QStringLiteral("^(.+)-r(\\d+)$"));
        const auto m = re.match(dirInfo.fileName());
        if (m.hasMatch()) {
            manifestVersion = m.captured(1);
            set.revision = m.captured(2).toInt();
        }
    }
    if (manifestVersion.isEmpty())
        return set;

    const QString build = versionDir + QStringLiteral("/build");
    if (!isNonEmptyFile(build + QStringLiteral("/index.html"))
        || !isNonEmptyFile(build + QStringLiteral("/js/app.bundle.js"))
        || !isNonEmptyFile(build + QStringLiteral("/sw.js")))
        return set;

    set.version = manifestVersion;
    set.valid = true;
    return set;
}

AssetStore::AssetSet AssetStore::probe(const QString& assetsRootOrVersionDir) const
{
    QFileInfo current(assetsRootOrVersionDir + QStringLiteral("/current"));
    if (current.exists()) {
        // Resolve the symlink chain ourselves; canonical may be empty if broken.
        const QString target = QFileInfo(current.filePath()).canonicalFilePath();
        if (!target.isEmpty())
            return probeVersionDir(target);
        return {};
    }
    return probeVersionDir(assetsRootOrVersionDir);
}

AssetStore::AssetSet AssetStore::resolve() const
{
    const AssetSet user = probe(m_userRoot);
    const AssetSet sys  = probe(m_systemRoot);

    if (user.valid && sys.valid)
        return compareVersions(user.version, sys.version) >= 0 ? user : sys;
    if (user.valid)
        return user;
    if (sys.valid)
        return sys;
    return {};
}

AssetStore::AssetSet AssetStore::bestLocal() const
{
    const AssetSet user = probe(m_userRoot);
    const AssetSet sys  = probe(m_systemRoot);
    if (user.valid && sys.valid)
        return compareVersions(user.version, sys.version) >= 0 ? user : sys;
    if (user.valid) return user;
    if (sys.valid)  return sys;
    return {};
}

bool AssetStore::markBad(const QString& versionDir)
{
    QFile f(versionDir + QLatin1Char('/') + kBadMarker);
    return f.open(QIODevice::WriteOnly);
}

void AssetStore::pruneUserVersions(const QStringList& keepDirs) const
{
    QDir root(m_userRoot);
    if (!root.exists())
        return;

    // Collect valid version dirs sorted newest-first.
    QList<AssetSet> sets;
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& e : entries) {
        const AssetSet s = probeVersionDir(root.absoluteFilePath(e));
        if (s.valid)
            sets << s;
    }
    std::sort(sets.begin(), sets.end(), [](const AssetSet& a, const AssetSet& b) {
        return compareVersions(a.version, b.version) > 0;
    });

    // Keep: everything in keepDirs (running + staged) + newest other one.
    bool keptOneExtra = false;
    for (const AssetSet& s : sets) {
        if (keepDirs.contains(s.root))
            continue;
        if (!keptOneExtra) {
            keptOneExtra = true;   // one rollback version retained (FR-9)
            continue;
        }
        QDir(s.root).removeRecursively();
    }
}
