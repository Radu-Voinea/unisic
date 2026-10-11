#include <QtTest>
#include <QTemporaryDir>
#include <QTimeZone>
#include "Settings.h"
#include "upload/UploadManager.h"
#include "upload/S3Sign.h"

class UploadUrlTest : public QObject
{
    Q_OBJECT
private slots:
    void curlTarget_data();
    void curlTarget();
    void responseTemplate();
    void offeredVariablesResolve();
    void vgyMeSupport();
    void s3SignatureMatchesAwsExamples();
    void s3Target_data();
    void s3Target();
    void s3SetupGuard();
    void s3ErrorBody();
    void s3UriEncode_data();
    void s3UriEncode();
    void s3TargetErrors();
    void s3SignedHeadersAreSorted();
    void s3OffersNoTemplateVars();
    void liveBuzzheavierUpload();
    void liveVgyMeUpload();
    void liveS3Upload();
};

void UploadUrlTest::curlTarget_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("want");
    QTest::newRow("append no slash") << "sftp://host/dir" << "a b.png" << "sftp://host/dir/a%20b.png";
    QTest::newRow("append slash") << "sftp://host/dir/" << "a b.png" << "sftp://host/dir/a%20b.png";
    QTest::newRow("token keeps query") << "https://host/up/%file%?to=inbox" << "a b.png"
                                       << "https://host/up/a%20b.png?to=inbox";
    QTest::newRow("token no escape") << "https://host/up/%file%" << "../../etc/x.png"
                                     << "https://host/up/.._.._etc_x.png";
    QTest::newRow("append no escape") << "sftp://host/dir/" << "../../etc/x.png"
                                      << "sftp://host/dir/.._.._etc_x.png";
}

void UploadUrlTest::curlTarget()
{
    QFETCH(QString, url);
    QFETCH(QString, name);
    QFETCH(QString, want);
    QCOMPARE(UploadManager::curlTargetUrl(url, name), want);
}

void UploadUrlTest::responseTemplate()
{
    const QJsonObject dest{{"urlPath", "https://host/$json:data.id$"}};
    QCOMPARE(UploadManager::extractUrl(dest, "urlPath", QByteArray(R"({"data":{"id":"abc123"}})")),
             QStringLiteral("https://host/abc123"));
}

// The destination editor offers each of these as a clickable chip, so each one
// is a promise that the sender substitutes it. This is the check that the two
// lists cannot drift: a chip for a token nothing resolves uploads the literal
// "%file%" and looks fine right up until the link is dead.
void UploadUrlTest::offeredVariablesResolve()
{
    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    Settings settings;
    UploadManager uploads(&settings);

    // The pills are painted from the same pattern the help carries, so a token
    // the chip types and the pattern misses would never be drawn as a variable.
    // Filled in first: a chip with caretBack > 0 inserts a token the user still
    // has to finish ($json:$ needs its path), and an unfinished one is meant to
    // stay unpilled, because it is not yet something extractUrl resolves.
    // Collected rather than asserted in place: QVERIFY returns, and a lambda
    // cannot both return early and yield a list.
    QStringList misses;
    auto tokensOf = [&](const QString &field, const QString &type) {
        QStringList out;
        const QVariantMap help = uploads.templateHelp(field, type);
        const QRegularExpression re(help.value("pattern").toString());
        const QVariantList vars = help.value("vars").toList();
        for (const QVariant &v : vars) {
            const QString token = v.toMap().value("token").toString();
            const int caretBack = v.toMap().value("caretBack").toInt();
            QString filled = token;
            if (caretBack > 0)
                filled.insert(filled.size() - caretBack, 'x');
            const QRegularExpressionMatch m = re.match(filled);
            if (!m.hasMatch() || m.captured(0) != filled)
                misses << field + "'s pattern misses " + filled;
            out << token;
        }
        out.sort();
        return out;
    };

    QCOMPARE(tokensOf("data", "http"), QStringList({"$base64$", "$filename$", "$mime$"}));
    QCOMPARE(tokensOf("requestUrl", "curl"), QStringList({"%file%"}));
    // Both senders substitute it, so both offer it - the descriptions differ
    // (only curl appends the name when the token is absent), the token does not.
    QCOMPARE(tokensOf("requestUrl", "http"), QStringList({"%file%"}));
    QCOMPARE(tokensOf("urlPath", "http"),
             QStringList({"$json:$", "$regex:$", "$text$"}));
    QVERIFY2(misses.isEmpty(), qPrintable(misses.join("; ")));

    // And through the code that has to consume them. $json:$ and $regex:$ are
    // offered half-written, with the caret parked where the path goes, so they
    // are finished here the way the user would.
    QCOMPARE(UploadManager::curlTargetUrl("https://host/up/%file%", "shot.png"),
             QStringLiteral("https://host/up/shot.png"));
    QCOMPARE(UploadManager::requestUrlWithFileName("https://host/up/%file%?to=inbox", "a b.png"),
             QStringLiteral("https://host/up/a%20b.png?to=inbox"));
    // The http URL takes NO append fallback: curl adds the name to a folder,
    // an API endpoint has to be posted to exactly as it stands.
    QCOMPARE(UploadManager::requestUrlWithFileName("https://api.host/v1/upload", "shot.png"),
             QStringLiteral("https://api.host/v1/upload"));
    const QByteArray answer(R"({"data":{"link":"https://host/x.png"}})");
    // Through named locals rather than brace-init or raw-string arguments: moc's
    // macro parser counts brackets without understanding either, and reads them
    // as the end of QCOMPARE's argument list.
    auto extract = [&](const QString &urlPath) {
        const QJsonObject dest{{QStringLiteral("urlPath"), urlPath}};
        return UploadManager::extractUrl(dest, "urlPath", answer);
    };
    const QString want = QStringLiteral("https://host/x.png");
    // Custom delimiter: the pattern itself contains )" .
    const QString byRegex = R"RX($regex:"link":"([^"]+)"$)RX";
    QCOMPARE(extract("$text$"), QString::fromUtf8(answer));
    QCOMPARE(extract("$json:data.link$"), want);
    QCOMPARE(extract(byRegex), want);
}

void UploadUrlTest::vgyMeSupport()
{
    // Host detection
    QVERIFY(UploadManager::isVgyMe(QJsonObject{{QStringLiteral("requestUrl"), QStringLiteral("https://vgy.me/upload")}}));
    QVERIFY(UploadManager::isVgyMe(QJsonObject{{QStringLiteral("requestUrl"), QStringLiteral("https://i.vgy.me/upload")}}));
    QVERIFY(!UploadManager::isVgyMe(QJsonObject{{QStringLiteral("requestUrl"), QStringLiteral("https://catbox.moe/user/api.php")}}));
    QVERIFY(!UploadManager::isVgyMe(QJsonObject{{QStringLiteral("requestUrl"), QStringLiteral("https://notvgy.me/upload")}}));

    // User key extraction
    const QJsonObject withKey{
        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("userkey"), QStringLiteral("secret123")}}}};
    QCOMPARE(UploadManager::vgyMeUserKey(withKey), QStringLiteral("secret123"));

    const QJsonObject withKeyCased{
        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("UserKey"), QStringLiteral("secret456")}}}};
    QCOMPARE(UploadManager::vgyMeUserKey(withKeyCased), QStringLiteral("secret456"));

    const QJsonObject withoutKey{
        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("other"), QStringLiteral("val")}}}};
    QVERIFY(UploadManager::vgyMeUserKey(withoutKey).isEmpty());

    // Response URL extraction
    const QJsonObject vgyDest{
        {QStringLiteral("urlPath"), QStringLiteral("$json:image$")},
        {QStringLiteral("deletionUrlPath"), QStringLiteral("$json:delete$")},
    };
    const QByteArray response(R"({"error":false,"url":"https://vgy.me/u/test","image":"https://i.vgy.me/test.png","delete":"https://vgy.me/delete/del123"})");
    QCOMPARE(UploadManager::extractUrl(vgyDest, QStringLiteral("urlPath"), response),
             QStringLiteral("https://i.vgy.me/test.png"));
    QCOMPARE(UploadManager::extractUrl(vgyDest, QStringLiteral("deletionUrlPath"), response),
             QStringLiteral("https://vgy.me/delete/del123"));

    // Guard test: vgy.me destination without a user key must be refused early
    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());

    Settings settings;
    UploadManager uploads(&settings);

    bool testDone = false;
    uploads.testDestination(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("vgy.me")},
        {QStringLiteral("requestUrl"), QStringLiteral("https://vgy.me/upload")}
    }, [&](bool ok, const QString &url, const QString &err) {
        testDone = true;
        QVERIFY(!ok);
        QVERIFY(url.isEmpty());
        QVERIFY(err.contains(QStringLiteral("user key")));
    });
    QVERIFY(testDone);
}

// The two worked examples in AWS's SigV4 documentation ("Signature Calculations
// for the Authorization Header: Transferring Payload in a Single Chunk"), with
// their published signatures. If these move, every real S3 upload is refused
// with SignatureDoesNotMatch, so they are the contract and not a snapshot.
void UploadUrlTest::s3SignatureMatchesAwsExamples()
{
    const QDateTime when(QDate(2013, 5, 24), QTime(0, 0), QTimeZone::utc());
    QCOMPARE(S3Sign::amzDate(when), QByteArray("20130524T000000Z"));

    S3Sign::Request get;
    get.method = "GET";
    get.canonicalUri = "/test.txt";
    get.payloadHash = S3Sign::sha256Hex({});
    get.region = QStringLiteral("us-east-1");
    get.accessKeyId = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
    get.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    get.when = when;
    // Deliberately out of order: the signer sorts.
    get.headers = {{"x-amz-date", "20130524T000000Z"},
                   {"range", "bytes=0-9"},
                   {"host", "examplebucket.s3.amazonaws.com"},
                   {"x-amz-content-sha256", get.payloadHash}};
    QCOMPARE(S3Sign::authorization(get),
             QByteArray("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,"
                        "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"
                        "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));

    S3Sign::Request put = get;
    put.method = "PUT";
    put.canonicalUri = S3Sign::uriEncode(QStringLiteral("/test$file.text"), true);
    QCOMPARE(put.canonicalUri, QByteArray("/test%24file.text"));
    put.payloadHash = S3Sign::sha256Hex("Welcome to Amazon S3.");
    QCOMPARE(put.payloadHash,
             QByteArray("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));
    put.headers = {{"host", "examplebucket.s3.amazonaws.com"},
                   {"date", "Fri, 24 May 2013 00:00:00 GMT"},
                   {"x-amz-date", "20130524T000000Z"},
                   {"x-amz-storage-class", "REDUCED_REDUNDANCY"},
                   {"x-amz-content-sha256", put.payloadHash}};
    QVERIFY(S3Sign::authorization(put).endsWith(
        "Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
}

void UploadUrlTest::s3Target_data()
{
    QTest::addColumn<QJsonObject>("dest");
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("publicUrl");

    const QJsonObject aws{{"bucket", "shots"}, {"region", "eu-central-1"}};
    QTest::newRow("aws default endpoint, virtual-hosted")
        << aws << "https://shots.s3.eu-central-1.amazonaws.com/a%20b.png"
        << "shots.s3.eu-central-1.amazonaws.com"
        << "https://shots.s3.eu-central-1.amazonaws.com/a%20b.png";
    QJsonObject dotted = aws;
    dotted.insert("bucket", "my.shots");
    QTest::newRow("dotted bucket goes path-style over https")
        << dotted << "https://s3.eu-central-1.amazonaws.com/my.shots/a%20b.png"
        << "s3.eu-central-1.amazonaws.com"
        << "https://s3.eu-central-1.amazonaws.com/my.shots/a%20b.png";
    QTest::newRow("minio: path-style, port kept, prefix cleaned")
        << QJsonObject{{"requestUrl", "http://localhost:9000/"}, {"bucket", "b"},
                       {"keyPrefix", "/../shots//2026/./"}}
        << "http://localhost:9000/b/shots/2026/a%20b.png" << "localhost:9000"
        << "http://localhost:9000/b/shots/2026/a%20b.png";
    QTest::newRow("r2 with a public base, default port dropped")
        << QJsonObject{{"requestUrl", "https://acct.r2.cloudflarestorage.com:443"},
                       {"bucket", "b"}, {"region", "auto"}, {"keyPrefix", "u"},
                       {"publicUrlBase", "https://cdn.example.com"}}
        << "https://acct.r2.cloudflarestorage.com/b/u/a%20b.png"
        << "acct.r2.cloudflarestorage.com" << "https://cdn.example.com/u/a%20b.png";
    QTest::newRow("scheme-less endpoint, forced virtual-hosted")
        << QJsonObject{{"requestUrl", "s3.example.org"}, {"bucket", "b"}, {"pathStyle", false}}
        << "https://b.s3.example.org/a%20b.png" << "b.s3.example.org"
        << "https://b.s3.example.org/a%20b.png";
}

void UploadUrlTest::s3Target()
{
    QFETCH(QJsonObject, dest);
    QFETCH(QString, url);
    QFETCH(QString, host);
    QFETCH(QString, publicUrl);
    const S3Sign::Target t = S3Sign::target(dest, QStringLiteral("a b.png"));
    QVERIFY2(t.error.isEmpty(), qPrintable(t.error));
    QCOMPARE(t.url.toString(QUrl::FullyEncoded), url);
    QCOMPARE(QString::fromLatin1(t.host), host);
    QCOMPARE(t.publicUrl, publicUrl);
    // What is signed has to be what QUrl will put on the wire.
    QCOMPARE(t.url.path(QUrl::FullyEncoded).toLatin1(), t.canonicalUri);
}

void UploadUrlTest::s3SetupGuard()
{
    QVERIFY(UploadManager::s3SetupError(QJsonObject{{"type", "s3"}}).contains("bucket"));
    QVERIFY(UploadManager::s3SetupError(QJsonObject{{"type", "s3"}, {"bucket", "a/b"}})
                .contains("slash"));
    QVERIFY(UploadManager::s3SetupError(QJsonObject{{"type", "s3"}, {"bucket", "b"}})
                .contains("access key"));
    const QJsonObject keyed{{"type", "s3"}, {"bucket", "b"},
                            {"accessKeyId", "id"}, {"secretAccessKey", "secret"}};
    QVERIFY(UploadManager::s3SetupError(keyed).isEmpty());
    QJsonObject badEndpoint = keyed;
    badEndpoint.insert("requestUrl", "ftp://host/");
    QVERIFY(UploadManager::s3SetupError(badEndpoint).contains("endpoint"));

    // Refused before any request goes out, on both entry points.
    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    Settings settings;
    UploadManager uploads(&settings);
    bool called = false;
    uploads.testDestination(QVariantMap{{"name", "s3"}, {"type", "s3"}, {"bucket", "b"}},
                            [&](bool ok, const QString &, const QString &err) {
        called = true;
        QVERIFY(!ok);
        QVERIFY(err.contains("access key"));
    });
    QVERIFY(called);
    QVERIFY(!uploads.busy());
}

void UploadUrlTest::s3ErrorBody()
{
    QCOMPARE(UploadManager::s3ErrorMessage(
                 "<?xml version=\"1.0\"?><Error><Code>AccessDenied</Code>"
                 "<Message>Access Denied</Message><RequestId>x</RequestId></Error>"),
             QStringLiteral("AccessDenied: Access Denied"));
    QVERIFY(UploadManager::s3ErrorMessage("not xml").isEmpty());
}

// SigV4's encoding is RFC 3986 unreserved-only with uppercase hex, and an
// object key keeps its slashes. Getting any of these wrong signs a different
// path from the one sent, which S3 answers with SignatureDoesNotMatch only for
// file names that happen to contain the character.
void UploadUrlTest::s3UriEncode_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<bool>("keepSlash");
    QTest::addColumn<QByteArray>("want");
    QTest::newRow("unreserved untouched") << "AZaz09-._~" << false << QByteArray("AZaz09-._~");
    QTest::newRow("space is %20, never +") << "a b" << false << QByteArray("a%20b");
    QTest::newRow("sub-delims encoded") << "a+b=c&d$(e)!*'" << false
                                        << QByteArray("a%2Bb%3Dc%26d%24%28e%29%21%2A%27");
    QTest::newRow("utf-8, uppercase hex") << QString::fromUtf8("zrzut ż.png") << false
                                          << QByteArray("zrzut%20%C5%BC.png");
    QTest::newRow("slash kept in a key") << "shots/2026/a.png" << true << QByteArray("shots/2026/a.png");
    QTest::newRow("slash encoded otherwise") << "a/b" << false << QByteArray("a%2Fb");
}

void UploadUrlTest::s3UriEncode()
{
    QFETCH(QString, in);
    QFETCH(bool, keepSlash);
    QFETCH(QByteArray, want);
    QCOMPARE(S3Sign::uriEncode(in, keepSlash), want);
}

void UploadUrlTest::s3TargetErrors()
{
    QCOMPARE(S3Sign::target(QJsonObject{}, "a.png").error, QStringLiteral("bucket"));
    QCOMPARE(S3Sign::target(QJsonObject{{"bucket", "a/b"}}, "a.png").error, QStringLiteral("bucket"));
    QCOMPARE(S3Sign::target(QJsonObject{{"bucket", "b"}, {"requestUrl", "ftp://host"}}, "a.png").error,
             QStringLiteral("endpoint"));
    QCOMPARE(S3Sign::target(QJsonObject{{"bucket", "b"}, {"requestUrl", "https://"}}, "a.png").error,
             QStringLiteral("endpoint"));
    // Empty region means us-east-1, in the scope and the default AWS endpoint.
    const S3Sign::Target t = S3Sign::target(QJsonObject{{"bucket", "b"}}, "a.png");
    QCOMPARE(t.region, QStringLiteral("us-east-1"));
    QCOMPARE(t.url.toString(), QStringLiteral("https://b.s3.us-east-1.amazonaws.com/a.png"));
    QCOMPARE(t.key, QStringLiteral("a.png"));
    // Special characters in the name: the wire path and the signed path agree.
    const S3Sign::Target odd = S3Sign::target(
        QJsonObject{{"bucket", "b"}, {"requestUrl", "http://h:9000"}}, QString::fromUtf8("a+b (1)ż.png"));
    QCOMPARE(odd.url.path(QUrl::FullyEncoded).toLatin1(), odd.canonicalUri);
    QCOMPARE(odd.canonicalUri, QByteArray("/b/a%2Bb%20%281%29%C5%BC.png"));
}

// The header list may come in any order (s3Upload appends x-amz-acl last) and
// values may carry stray whitespace: both must sign identically.
void UploadUrlTest::s3SignedHeadersAreSorted()
{
    S3Sign::Request a;
    a.method = "PUT";
    a.canonicalUri = "/k";
    a.payloadHash = S3Sign::kUnsignedPayload;
    a.region = "auto";
    a.accessKeyId = "id";
    a.secretAccessKey = "secret";
    a.when = QDateTime(QDate(2026, 10, 12), QTime(8, 0), QTimeZone::utc());
    a.headers = {{"x-amz-date", "20261012T080000Z"}, {"host", "h"}, {"x-amz-acl", "public-read"},
                 {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}};
    S3Sign::Request b = a;
    b.headers = {{"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}, {"host", "  h "},
                 {"x-amz-acl", "public-read"}, {"x-amz-date", "20261012T080000Z"}};
    const QByteArray auth = S3Sign::authorization(a);
    QCOMPARE(S3Sign::authorization(b), auth);
    QVERIFY(auth.contains("SignedHeaders=host;x-amz-acl;x-amz-content-sha256;x-amz-date,"));
    QVERIFY(auth.startsWith("AWS4-HMAC-SHA256 Credential=id/20261012/auto/s3/aws4_request,"));
    // And the secret matters: a different one is a different signature.
    b.secretAccessKey = "other";
    QVERIFY(S3Sign::authorization(b) != auth);
}

// The editor draws chips from templateHelp(); an s3 endpoint substitutes
// nothing, so offering %file% there would be a chip that silently does nothing.
void UploadUrlTest::s3OffersNoTemplateVars()
{
    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    Settings settings;
    UploadManager uploads(&settings);
    for (const char *field : {"requestUrl", "data", "urlPath"})
        QVERIFY(uploads.templateHelp(QString::fromLatin1(field), "s3")
                    .value("vars").toList().isEmpty());
}

// The whole s3 path against a real store. Off unless pointed at one, e.g. a
// throwaway MinIO:
//   UNISIC_LIVE_S3_ENDPOINT=http://localhost:9000 UNISIC_LIVE_S3_BUCKET=test
//   UNISIC_LIVE_S3_KEY_ID=minioadmin UNISIC_LIVE_S3_SECRET=minioadmin
//   [UNISIC_LIVE_S3_REGION=us-east-1] ctest --test-dir build -R upload_url
// Both payload kinds go up: in-memory (signed hash) and a file (UNSIGNED-PAYLOAD).
void UploadUrlTest::liveS3Upload()
{
    const QByteArray bucket = qgetenv("UNISIC_LIVE_S3_BUCKET");
    if (bucket.isEmpty())
        QSKIP("live s3 upload: set UNISIC_LIVE_S3_BUCKET, _KEY_ID, _SECRET and optionally _ENDPOINT/_REGION");

    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    Settings settings;
    UploadManager uploads(&settings);
    const QVariantMap dest{
        {"name", "s3 live test"}, {"type", "s3"},
        {"requestUrl", QString::fromUtf8(qgetenv("UNISIC_LIVE_S3_ENDPOINT"))},
        {"bucket", QString::fromUtf8(bucket)},
        {"region", QString::fromUtf8(qgetenv("UNISIC_LIVE_S3_REGION"))},
        {"accessKeyId", QString::fromUtf8(qgetenv("UNISIC_LIVE_S3_KEY_ID"))},
        {"secretAccessKey", QString::fromUtf8(qgetenv("UNISIC_LIVE_S3_SECRET"))},
        {"keyPrefix", "unisic-live-test"}};

    bool done = false, ok = false;
    QString url, err;
    uploads.testDestination(dest, [&](bool o, const QString &u, const QString &e) {
        done = true; ok = o; url = u; err = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY2(url.endsWith(QStringLiteral("/unisic-live-test/unisic-test.png")), qPrintable(url));
    qInfo() << "live s3 upload (memory) landed at" << url;

    // The streamed-file path, through the store's own settings: make it active.
    uploads.saveDestination(dest);
    settings.setActiveDestination(QStringLiteral("s3 live test"));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("streamed file.txt"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(256 * 1024, 'u'));
    f.close();
    done = false;
    uploads.uploadFile(path, [&](const QString &u, const QString &, const QString &e) {
        done = true; ok = e.isEmpty(); url = u; err = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY2(url.endsWith(QStringLiteral("/streamed%20file.txt")), qPrintable(url));
    qInfo() << "live s3 upload (file) landed at" << url;

    // A name that tries to climb out of the folder stays inside it.
    done = false;
    uploads.uploadData(QByteArray("x"), QStringLiteral("../../evil name.png"), QStringLiteral("image/png"),
                       [&](const QString &u, const QString &, const QString &e) {
        done = true; ok = e.isEmpty(); url = u; err = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY2(url.endsWith(QStringLiteral("/unisic-live-test/.._.._evil%20name.png")), qPrintable(url));
}

// The whole curl path against the real host it was built for: the %file% token
// puts the name in the URL, curl's stdout is parsed, and the link comes back.
// OFF by default - it puts a small PNG on a public file host, which is not
// something a plain `ctest` run should do behind anyone's back. Run it with
//   UNISIC_LIVE_UPLOAD=1 ctest --test-dir build -R upload_url --output-on-failure
void UploadUrlTest::liveBuzzheavierUpload()
{
    if (qgetenv("UNISIC_LIVE_UPLOAD") != QByteArray("1"))
        QSKIP("live upload: set UNISIC_LIVE_UPLOAD=1 (uploads a test image to buzzheavier.com)");

    // UploadManager writes destinations.json on construction; keep that out of
    // the real config directory.
    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());

    Settings settings;
    UploadManager uploads(&settings);
    bool done = false, ok = false;
    QString url, err;
    uploads.testDestination(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("buzzheavier live test")},
        {QStringLiteral("type"), QStringLiteral("curl")},
        {QStringLiteral("requestUrl"), QStringLiteral("https://w.buzzheavier.com/%file%")},
        {QStringLiteral("urlPath"), QStringLiteral("https://buzzheavier.com/$json:data.id$")},
    }, [&](bool o, const QString &u, const QString &e) {
        done = true; ok = o; url = u; err = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY2(url.startsWith(QStringLiteral("https://buzzheavier.com/"))
                 && url.size() > int(sizeof("https://buzzheavier.com/")),
             qPrintable(QStringLiteral("no id in the returned link: '%1'").arg(url)));
    qInfo() << "live upload landed at" << url;
}

void UploadUrlTest::liveVgyMeUpload()
{
    const QByteArray key = qgetenv("UNISIC_LIVE_VGYME_KEY");
    if (key.isEmpty())
        QSKIP("live vgy.me upload: set UNISIC_LIVE_VGYME_KEY=<your-key> to test real upload to vgy.me");

    QTemporaryDir cfg;
    QVERIFY(cfg.isValid());
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());

    Settings settings;
    UploadManager uploads(&settings);
    bool done = false, ok = false;
    QString url, err;
    uploads.testDestination(QVariantMap{
        {QStringLiteral("name"), QStringLiteral("vgy.me live test")},
        {QStringLiteral("type"), QStringLiteral("http")},
        {QStringLiteral("requestUrl"), QStringLiteral("https://vgy.me/upload")},
        {QStringLiteral("method"), QStringLiteral("POST")},
        {QStringLiteral("fileFormName"), QStringLiteral("file")},
        {QStringLiteral("responseType"), QStringLiteral("json")},
        {QStringLiteral("urlPath"), QStringLiteral("$json:image$")},
        {QStringLiteral("deletionUrlPath"), QStringLiteral("$json:delete$")},
        {QStringLiteral("arguments"), QVariantMap{{QStringLiteral("userkey"), QString::fromUtf8(key)}}},
    }, [&](bool o, const QString &u, const QString &e) {
        done = true; ok = o; url = u; err = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY2(url.contains(QStringLiteral("vgy.me")), qPrintable(url));
    qInfo() << "live vgy.me upload landed at" << url;
}

QTEST_MAIN(UploadUrlTest)
#include "UploadUrlTest.moc"
