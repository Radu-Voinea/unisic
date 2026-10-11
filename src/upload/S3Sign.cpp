#include "S3Sign.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QStringList>
#include <algorithm>

namespace S3Sign {

QByteArray uriEncode(const QString &s, bool keepSlash)
{
    // QUrl::toPercentEncoding leaves exactly the RFC 3986 unreserved set alone
    // and writes uppercase hex, which is SigV4's rule; only the slash differs.
    return QUrl::toPercentEncoding(s, keepSlash ? QByteArray("/") : QByteArray());
}

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray amzDate(const QDateTime &utc)
{
    return utc.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

static QByteArray hmac(const QByteArray &key, const QByteArray &msg)
{
    return QMessageAuthenticationCode::hash(msg, key, QCryptographicHash::Sha256);
}

QByteArray authorization(const Request &r)
{
    auto headers = r.headers;
    std::sort(headers.begin(), headers.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QByteArray canonicalHeaders, signedHeaders;
    for (const auto &h : std::as_const(headers)) {
        canonicalHeaders += h.first + ':' + h.second.trimmed() + '\n';
        if (!signedHeaders.isEmpty())
            signedHeaders += ';';
        signedHeaders += h.first;
    }
    const QByteArray canonicalRequest = r.method + '\n' + r.canonicalUri + '\n'
                                        + r.canonicalQuery + '\n' + canonicalHeaders + '\n'
                                        + signedHeaders + '\n' + r.payloadHash;

    const QByteArray date = r.when.toUTC().toString(QStringLiteral("yyyyMMdd")).toLatin1();
    const QByteArray scope = date + '/' + r.region.toUtf8() + "/s3/aws4_request";
    const QByteArray stringToSign = "AWS4-HMAC-SHA256\n" + amzDate(r.when) + '\n' + scope + '\n'
                                    + sha256Hex(canonicalRequest);

    const QByteArray kDate = hmac("AWS4" + r.secretAccessKey.toUtf8(), date);
    const QByteArray kRegion = hmac(kDate, r.region.toUtf8());
    const QByteArray kService = hmac(kRegion, "s3");
    const QByteArray kSigning = hmac(kService, "aws4_request");
    const QByteArray signature = hmac(kSigning, stringToSign).toHex();

    return "AWS4-HMAC-SHA256 Credential=" + r.accessKeyId.toUtf8() + '/' + scope
           + ",SignedHeaders=" + signedHeaders + ",Signature=" + signature;
}

Target target(const QJsonObject &dest, const QString &fileName)
{
    Target t;
    t.region = dest.value(QStringLiteral("region")).toString().trimmed();
    if (t.region.isEmpty())
        t.region = QStringLiteral("us-east-1");

    const QString bucket = dest.value(QStringLiteral("bucket")).toString().trimmed();
    if (bucket.isEmpty() || bucket.contains(QLatin1Char('/'))) {
        t.error = QStringLiteral("bucket");
        return t;
    }

    QString endpoint = dest.value(QStringLiteral("requestUrl")).toString().trimmed();
    if (endpoint.isEmpty())
        endpoint = QStringLiteral("https://s3.%1.amazonaws.com").arg(t.region);
    else if (!endpoint.contains(QLatin1String("://")))
        endpoint.prepend(QStringLiteral("https://")); // "minio.lan:9000" as typed
    const QUrl ep(endpoint);
    const QString scheme = ep.scheme().toLower();
    const QByteArray epHost = ep.host(QUrl::FullyEncoded).toLatin1();
    if (!ep.isValid() || epHost.isEmpty()
        || (scheme != QLatin1String("https") && scheme != QLatin1String("http"))) {
        t.error = QStringLiteral("endpoint");
        return t;
    }
    // The Host header is signed, so it has to be exactly what goes on the wire.
    // An explicit default port is dropped here rather than trusted to match
    // whatever the HTTP stack would have written for it.
    int port = ep.port();
    if ((scheme == QLatin1String("https") && port == 443)
        || (scheme == QLatin1String("http") && port == 80))
        port = -1;

    // Prefix segments: an empty, "." or ".." segment is dropped, so a typed
    // "/shots//2026/" and a hostile "../x" both land inside the bucket.
    QStringList keyParts;
    const QStringList prefix = dest.value(QStringLiteral("keyPrefix")).toString()
                                   .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &seg : prefix) {
        const QString s = seg.trimmed();
        if (!s.isEmpty() && s != QLatin1String(".") && s != QLatin1String(".."))
            keyParts << s;
    }
    keyParts << fileName;
    t.key = keyParts.join(QLatin1Char('/'));
    const QByteArray encodedKey = uriEncode(t.key, true);

    bool pathStyle = !epHost.endsWith("amazonaws.com");
    if (dest.contains(QStringLiteral("pathStyle")))
        pathStyle = dest.value(QStringLiteral("pathStyle")).toBool();
    if (bucket.contains(QLatin1Char('.')) && scheme == QLatin1String("https"))
        pathStyle = true;

    QByteArray basePath = ep.path(QUrl::FullyEncoded).toLatin1();
    while (basePath.endsWith('/'))
        basePath.chop(1);
    QByteArray host;
    if (pathStyle) {
        host = epHost;
        t.canonicalUri = basePath + '/' + uriEncode(bucket, false) + '/' + encodedKey;
    } else {
        host = uriEncode(bucket.toLower(), false) + '.' + epHost;
        t.canonicalUri = basePath + '/' + encodedKey;
    }
    t.host = port == -1 ? host : host + ':' + QByteArray::number(port);
    t.url = QUrl::fromEncoded(scheme.toLatin1() + "://" + t.host + t.canonicalUri,
                              QUrl::StrictMode);

    QString base = dest.value(QStringLiteral("publicUrlBase")).toString().trimmed();
    if (!base.isEmpty()) {
        if (!base.endsWith(QLatin1Char('/')))
            base += QLatin1Char('/');
        t.publicUrl = base + QString::fromLatin1(encodedKey);
    } else {
        t.publicUrl = t.url.toString(QUrl::FullyEncoded);
    }
    return t;
}

} // namespace S3Sign
