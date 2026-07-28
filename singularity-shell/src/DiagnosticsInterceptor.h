#pragma once

#include <QDebug>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>

// DiagnosticsInterceptor — logging-only request observer (qt-tz.md FR-11).
// Installed only with --diagnose. MUST NOT modify, redirect, or block anything
// (FR-5: vendor API traffic is untouched).
class DiagnosticsInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        static const char* types[] = {
            "MainFrame", "SubFrame", "Stylesheet", "Script", "Image", "Font",
            "SubResource", "Object", "Media", "Worker", "SharedWorker",
            "Prefetch", "Favicon", "Xhr", "Ping", "ServiceWorker", "CspReport",
            "PluginResource", "NavigationPreloadMainFrame",
            "NavigationPreloadSubFrame", "WebSocket"
        };
        const int t = int(info.resourceType());
        const char* type = (t >= 0 && t < int(sizeof(types) / sizeof(types[0])))
                               ? types[t] : "Unknown";
        qInfo("[request] %-12s %s", type,
              qUtf8Printable(info.requestUrl().toString()));
    }
};
