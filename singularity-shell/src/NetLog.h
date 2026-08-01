#pragma once

#include <QByteArray>
#include <cstdio>

// Verbose network logging, gated by the environment variable
// SINGULARITY_SHELL_LOG_REQUESTS (any non-empty value enables it). Output goes
// to stderr — that is reliable in the request-interception / service-worker
// request contexts, where Qt's own logging is not always routed through.
//
// The request interceptor (VendorApiInterceptor) can only see requests before
// they are sent, so it logs requests WITHOUT an HTTP status; the sg:// proxy
// (SgSchemeHandler::proxyRequest) performs the fetch itself and therefore also
// logs the resulting status.
namespace NetLog {

inline bool enabled()
{
    static const bool e = !qgetenv("SINGULARITY_SHELL_LOG_REQUESTS").isEmpty();
    return e;
}

// Human-readable name for QWebEngineUrlRequestInfo::ResourceType (cast to int).
inline const char* resourceTypeName(int t)
{
    static const char* names[] = {
        "MainFrame", "SubFrame", "Stylesheet", "Script", "Image", "Font",
        "SubResource", "Object", "Media", "Worker", "SharedWorker", "Prefetch",
        "Favicon", "Xhr", "Ping", "ServiceWorker", "CspReport", "PluginResource",
        "NavigationPreloadMainFrame", "NavigationPreloadSubFrame", "WebSocket"
    };
    return (t >= 0 && t < int(sizeof(names) / sizeof(names[0]))) ? names[t] : "Unknown";
}

}  // namespace NetLog
