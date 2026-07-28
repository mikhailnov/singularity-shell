#pragma once
#include <cstdio>

#include <QDebug>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>
#include <QUrl>

// VendorApiInterceptor — removes the `deadline` gRPC metadata header from
// requests to the vendor API hosts (proxyN.singularity-app.com/.ru).
//
// Why: the vendor's gRPC-web client attaches a custom `deadline` header to
// every API call. The vendor nginx answers CORS preflights with a static
// Access-Control-Allow-Headers list that does NOT include `deadline`, so any
// spec-compliant browser rejects the request before it is sent
// ("Request header field deadline is not allowed by
// Access-Control-Allow-Headers in preflight response"). The official desktop
// client never hits this because it issues the calls from the Electron main
// process (Node axios, no CORS); the web client falls back to the vendor's
// CORS proxy. Our renderer has neither, so we drop this one header at the
// network layer, before CORS evaluation. The header is only an advisory
// client-side timeout hint (DEFAULT_GRPC_TIMEOUT_MS is also applied locally),
// so removing it is semantically safe.
//
// Installed unconditionally (unlike DiagnosticsInterceptor).
class VendorApiInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        const QString host = info.requestUrl().host();
        static const QLatin1String suffixes[] = {
            QLatin1String(".singularity-app.com"),
            QLatin1String(".singularity-app.ru"),
        };
        bool vendor = host == QLatin1String("singularity-app.com")
                      || host == QLatin1String("singularity-app.ru");
        for (const auto& s : suffixes)
            if (host.endsWith(s)) { vendor = true; break; }
        if (!vendor)
            return;

        if (info.httpHeaders().contains(QByteArrayLiteral("deadline"))) {
            // Empty value removes the header (QWebEngineUrlRequestInfo contract).
            info.setHttpHeader(QByteArrayLiteral("deadline"), QByteArray());
            // fprintf, not qInfo: messages from the interceptor context are
            // not reliably routed through Qt logging on worker/SW requests.
            fprintf(stderr, "[net] stripped 'deadline' header: %s\n",
                    qUtf8Printable(info.requestUrl().toString()));
        }
    }
};
