#include <QtTest>

#include "actions/FileManagerMenu.h"

// What the file managers are handed for "Upload with Unisic". The menu entry
// is the one piece of this feature nobody re-reads after it is written: a
// wrong Exec just makes the item do nothing when clicked, so the exact lines
// are pinned here.
class FileManagerMenuTest : public QObject
{
    Q_OBJECT

    static FileManagerMenu::Launcher native(const QString &binary)
    {
        FileManagerMenu::Launcher l;
        l.program << binary;
        return l;
    }

    static FileManagerMenu::Launcher flatpak()
    {
        FileManagerMenu::Launcher l;
        l.program << QStringLiteral("flatpak") << QStringLiteral("run") << QStringLiteral("--user")
                  << QStringLiteral("app.unisic.Unisic");
        l.fileForwarding = true;
        return l;
    }

    static QByteArray contentOf(const QList<FileManagerMenu::Entry> &entries,
                                const QString &suffix)
    {
        for (const auto &e : entries) {
            if (e.path.endsWith(suffix))
                return e.content;
        }
        return {};
    }

private slots:
    void desktopExecQuoting_data()
    {
        QTest::addColumn<QString>("arg");
        QTest::addColumn<QString>("want");
        QTest::newRow("plain") << "/usr/bin/unisic" << "/usr/bin/unisic";
        QTest::newRow("space") << "/opt/My Apps/unisic" << "\"/opt/My Apps/unisic\"";
        // Reserved escape, then the string-level backslash doubling on top.
        QTest::newRow("dollar") << "/opt/$x/unisic" << "\"/opt/\\\\$x/unisic\"";
        QTest::newRow("percent") << "/opt/100%/unisic" << "/opt/100%%/unisic";
    }
    void desktopExecQuoting()
    {
        QFETCH(QString, arg);
        QFETCH(QString, want);
        QCOMPARE(FileManagerMenu::desktopExecArg(arg), want);
    }

    void shellQuoting()
    {
        QCOMPARE(FileManagerMenu::shellQuote(QStringLiteral("it's")),
                 QStringLiteral("'it'\\''s'"));
    }

    void serviceMenuNative()
    {
        const auto entries = FileManagerMenu::entries(
            QStringLiteral("/d"), QStringLiteral("app.unisic.Unisic"),
            native(QStringLiteral("/usr/bin/unisic")), QStringLiteral("Upload with Unisic"),
            QStringLiteral("app.unisic.Unisic"), false, false);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().path,
                 QStringLiteral("/d/kio/servicemenus/app.unisic.Unisic.upload.desktop"));
        QVERIFY(entries.first().executable);
        const QByteArray c = entries.first().content;
        QVERIFY(c.contains("[Desktop Entry]\nType=Service\n"));
        QVERIFY(c.contains("MimeType=application/octet-stream;\n"));
        QVERIFY(c.contains("Actions=upload;\n"));
        QVERIFY(c.contains("[Desktop Action upload]\nName=Upload with Unisic\n"));
        QVERIFY(c.contains("Exec=/usr/bin/unisic --upload -- %F\n"));
    }

    // --file-forwarding is a `flatpak run` option: it must sit BEFORE the app
    // id, and the files between @@ markers, or the sandbox gets paths it
    // cannot open.
    void serviceMenuFlatpak()
    {
        const auto entries = FileManagerMenu::entries(
            QStringLiteral("/d"), QStringLiteral("app.unisic.Unisic"), flatpak(),
            QStringLiteral("Upload with Unisic"), QStringLiteral("app.unisic.Unisic"), true, true);
        QCOMPARE(entries.size(), 3);
        QVERIFY(contentOf(entries, QStringLiteral(".upload.desktop")).contains(
            "Exec=flatpak run --user --file-forwarding app.unisic.Unisic --upload -- @@ %F @@\n"));
        QVERIFY(contentOf(entries, QStringLiteral("/Upload with Unisic")).contains(
            "exec 'flatpak' 'run' '--user' --file-forwarding 'app.unisic.Unisic' --upload -- @@ \"$@\" @@\n"));
        const QByteArray nemo = contentOf(entries, QStringLiteral(".upload.nemo_action"));
        QVERIFY(nemo.contains("[Nemo Action]\n"));
        QVERIFY(nemo.contains("Extensions=nodirs;\n"));
        QVERIFY(nemo.contains("--upload -- @@ %F @@\n"));
    }

    void nautilusScriptIsNamedByLabel()
    {
        const auto entries = FileManagerMenu::entries(
            QStringLiteral("/d"), QStringLiteral("id"), native(QStringLiteral("/u")),
            QStringLiteral("Upload a/b"), QStringLiteral("id"), true, false);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.last().path, QStringLiteral("/d/nautilus/scripts/Upload a-b"));
        QVERIFY(entries.last().content.startsWith("#!/bin/sh\n"));
        QVERIFY(entries.last().executable);
    }

    void applyWritesAndRemovesOnlyItsOwn()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString home = tmp.path();
        QDir().mkpath(home + QStringLiteral("/nautilus/scripts"));
        // A script of the user's own, which must survive both directions.
        const QString theirs = home + QStringLiteral("/nautilus/scripts/Mine");
        {
            QFile f(theirs);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\necho mine\n");
        }
        QString error;
        QStringList written = FileManagerMenu::apply(
            true, home, QStringLiteral("id"), native(QStringLiteral("/u")),
            QStringLiteral("Upload with Unisic"), QStringLiteral("id"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(written.size(), 2); // service menu + Nautilus script, no Nemo dir
        const QString menu = FileManagerMenu::serviceMenuPath(home, QStringLiteral("id"));
        QVERIFY(QFileInfo(menu).permissions() & QFileDevice::ExeUser);

        // A new language renames the script: the old name must not linger.
        written = FileManagerMenu::apply(true, home, QStringLiteral("id"),
                                         native(QStringLiteral("/u")),
                                         QStringLiteral("Wyślij przez Unisic"),
                                         QStringLiteral("id"), &error);
        QVERIFY(!QFileInfo::exists(home + QStringLiteral("/nautilus/scripts/Upload with Unisic")));
        QVERIFY(QFileInfo::exists(home + QStringLiteral("/nautilus/scripts/Wyślij przez Unisic")));

        FileManagerMenu::apply(false, home, QStringLiteral("id"), native(QStringLiteral("/u")),
                               {}, {}, nullptr);
        QVERIFY(!QFileInfo::exists(menu));
        QVERIFY(!QFileInfo::exists(home + QStringLiteral("/nautilus/scripts/Wyślij przez Unisic")));
        QVERIFY(QFileInfo::exists(theirs));
    }

    // A service menu of the same name the user wrote by hand is theirs.
    void applyKeepsAForeignFileOfTheSameName()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString menu = FileManagerMenu::serviceMenuPath(tmp.path(), QStringLiteral("id"));
        QDir().mkpath(QFileInfo(menu).absolutePath());
        {
            QFile f(menu);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[Desktop Entry]\nType=Service\n");
        }
        FileManagerMenu::apply(true, tmp.path(), QStringLiteral("id"), native(QStringLiteral("/u")),
                               QStringLiteral("Upload"), QStringLiteral("id"), nullptr);
        FileManagerMenu::apply(false, tmp.path(), QStringLiteral("id"), native(QStringLiteral("/u")),
                               {}, {}, nullptr);
        QFile f(menu);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("[Desktop Entry]\nType=Service\n"));
    }
};

QTEST_GUILESS_MAIN(FileManagerMenuTest)
#include "FileManagerMenuTest.moc"
