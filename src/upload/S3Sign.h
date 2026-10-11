#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// AWS Signature Version 4 for a single S3 request, plus the one piece of
// destination handling that decides WHERE an object goes. Pure functions over
// Qt Core (QCryptographicHash / QMessageAuthenticationCode): no network, no
// clock, no settings, so tests/UploadUrlTest.cpp pins the signer against the
// examples AWS publishes in its SigV4 documentation, byte for byte.
//
// Written here instead of shelling out to `curl --aws-sigv4`: that option's
// handling of a streamed -T upload changed across curl releases (older ones
// signed an empty payload hash and S3 refused every PUT), and the distro curl
// is not ours to pin. Qt's HTTP stack is.
namespace S3Sign {

// The payload hash to sign when the body is streamed from disk. Hashing a
// hundreds-of-MB recording up front would read it twice and block the UI
// thread for the first pass; S3 and every compatible store accept this over
// TLS, where the transport already guarantees the bytes.
inline constexpr char kUnsignedPayload[] = "UNSIGNED-PAYLOAD";

// SigV4 URI encoding: everything but A-Z a-z 0-9 - . _ ~ is %XX (uppercase),
// and '/' too unless `keepSlash` (the object key in a path keeps its slashes).
QByteArray uriEncode(const QString &s, bool keepSlash);

// sha256 hex of `data`, lowercase, as SigV4 spells every hash.
QByteArray sha256Hex(const QByteArray &data);

// ISO 8601 basic form, "20130524T000000Z" - the x-amz-date header.
QByteArray amzDate(const QDateTime &utc);

struct Request {
    QByteArray method;        // "PUT"
    QByteArray canonicalUri;  // already uriEncoded, starting with '/'
    QByteArray canonicalQuery; // empty for a PUT Object
    // Every header that is SIGNED, lowercase name -> trimmed value. Must
    // include "host" and "x-amz-date"; for S3 also "x-amz-content-sha256".
    // Headers sent but not listed here are simply unsigned, which S3 allows.
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray payloadHash;   // sha256Hex(body) or kUnsignedPayload
    QString region;
    QString accessKeyId;
    QString secretAccessKey;
    QDateTime when;           // UTC; the date in the scope and the signature
};

// The complete Authorization header value:
// "AWS4-HMAC-SHA256 Credential=<id>/<date>/<region>/s3/aws4_request,
//  SignedHeaders=<a;b;c>,Signature=<hex>"
QByteArray authorization(const Request &r);

// Where an object for this destination is PUT, and the link handed back.
//   requestUrl  - the S3 endpoint; empty means AWS itself, https://s3.<region>.amazonaws.com
//   bucket, region ("us-east-1" when empty; Cloudflare R2 wants "auto")
//   keyPrefix   - optional folder inside the bucket ("shots/2026")
//   pathStyle   - optional bool. Absent: virtual-hosted (bucket.host) on an
//                 amazonaws.com endpoint, path-style (host/bucket) anywhere
//                 else, which is what MinIO and most self-hosted stores need.
//                 A bucket name with a dot is always path-style over https: as
//                 a host label it would break the *.s3 wildcard certificate.
//   publicUrlBase - optional. The bucket root as the world sees it (a CDN or
//                 custom domain); the object key is appended. Without it the
//                 link is the object URL itself, which only opens for a
//                 public-read bucket.
struct Target {
    QUrl url;                 // the PUT target, explicit default ports removed
    QByteArray host;          // exactly the Host header that is signed and sent
    QByteArray canonicalUri;  // the path as sent, already encoded
    QString key;              // the object key, unencoded
    QString publicUrl;
    QString region;
    // Empty, or a token naming what is missing: "bucket" (none, or one with a
    // '/' in it) or "endpoint" (not an http/https URL with a host). Not a
    // sentence on purpose: the wording lives with UploadManager's other tr()
    // strings, so this unit needs no translations.
    QString error;
};
// `fileName` must already be sanitized (no path separators).
Target target(const QJsonObject &dest, const QString &fileName);

} // namespace S3Sign
