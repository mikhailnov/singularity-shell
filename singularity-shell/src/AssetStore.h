#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// AssetStore — resolves which versioned asset set the shell must serve,
// per qt-tz.md FR-9a:
//   1) user dir ($XDG_DATA_HOME/singularity-shell/assets/current)
//      if valid and version >= system version;
//   2) else system dir (/usr/share/singularity-shell/assets/current);
//   3) else invalid (bootstrap mode, FR-10).
//
// Asset directory layout produced by fetch-assets.sh:
//   <assetsRoot>/<version>-r<revision>/{build/**, package.json, manifest.json}
//   <assetsRoot>/current -> symlink to the active version dir
class AssetStore
{
public:
    struct AssetSet {
        QString root;      // resolved absolute path of <version>-r<revision>/
        QString version;   // vendor version, e.g. "12.5.0"
        int revision = 0;
        bool valid = false;
    };

    // systemAssetsRoot: compile-time default is /usr/share/singularity-shell/assets
    explicit AssetStore(QString userAssetsRoot, QString systemAssetsRoot);

    // Resolution per FR-9a. Never throws.
    AssetSet resolve() const;

    // Inspect <assetsRoot>/current (or a concrete version dir). Valid means:
    // manifest parses, build/index.html and build/js/app.bundle.js exist and
    // are non-empty, dir is not marked bad.
    AssetSet probe(const QString& assetsRootOrVersionDir) const;

    // Version comparison on dotted numeric versions; returns <0, 0, >0.
    static int compareVersions(const QString& a, const QString& b);
    static QList<int> versionKey(const QString& v);

    // Mark a broken version dir so it is never selected again (FR-9 step 8).
    static bool markBad(const QString& versionDir);

    // Highest version among valid sets in user and system roots (FR-9 step 3).
    AssetSet bestLocal() const;

    // Prune user versions: keep `keepDirs` (absolute paths) plus the newest
    // other valid version; delete the rest. Never deletes the running set.
    void pruneUserVersions(const QStringList& keepDirs) const;

    QString userRoot() const { return m_userRoot; }
    QString systemRoot() const { return m_systemRoot; }

private:
    AssetSet probeVersionDir(const QString& versionDir) const;

    QString m_userRoot;
    QString m_systemRoot;
};
