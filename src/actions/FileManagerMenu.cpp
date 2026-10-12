#include "FileManagerMenu.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace FileManagerMenu {

QString desktopExecArg(const QString &arg)
{
    // Reserved characters per the Desktop Entry spec: any of them forces the
    // argument into double quotes.
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    bool needsQuotes = arg.isEmpty();
    for (const QChar c : arg) {
        if (reserved.contains(c)) {
            needsQuotes = true;
            break;
        }
    }
    QString out;
    for (const QChar c : arg) {
        if (c == QLatin1Char('%')) {
            out += QStringLiteral("%%"); // a literal %, not a field code
            continue;
        }
        if (needsQuotes && (c == QLatin1Char('"') || c == QLatin1Char('`')
                            || c == QLatin1Char('$') || c == QLatin1Char('\\')))
            out += QLatin1Char('\\');
        out += c;
    }
    if (needsQuotes)
        out = QLatin1Char('"') + out + QLatin1Char('"');
    // Exec is a string value too, and the string-level escapes are undone
    // FIRST: every backslash has to be doubled once more to survive that.
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    return out;
}

QString shellQuote(const QString &arg)
{
    QString out = arg;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

// One line per file kind: the program, the fixed flags, then the file list
// spelled the way that file manager expands it.
static QString commandLine(const Launcher &launcher, QString (*quote)(const QString &),
                           const QString &files)
{
    QStringList parts;
    for (int i = 0; i < launcher.program.size(); ++i) {
        parts << quote(launcher.program.at(i));
        // `flatpak run [options] <app-id>`: --file-forwarding is a run
        // option, so it goes right before the app id (the prefix's last item).
        if (launcher.fileForwarding && i == launcher.program.size() - 2)
            parts << QStringLiteral("--file-forwarding");
    }
    parts << QStringLiteral("--upload") << QStringLiteral("--");
    if (launcher.fileForwarding)
        parts << QStringLiteral("@@") << files << QStringLiteral("@@");
    else
        parts << files;
    return parts.join(QLatin1Char(' '));
}

QString serviceMenuPath(const QString &dataHome, const QString &id)
{
    return dataHome + QStringLiteral("/kio/servicemenus/") + id + QStringLiteral(".upload.desktop");
}

static QString nautilusDir(const QString &dataHome)
{
    return dataHome + QStringLiteral("/nautilus/scripts");
}

static QString nemoPath(const QString &dataHome, const QString &id)
{
    return dataHome + QStringLiteral("/nemo/actions/") + id + QStringLiteral(".upload.nemo_action");
}

// A Nautilus script's FILE NAME is its menu label, so it cannot contain '/'.
static QString scriptName(const QString &label)
{
    QString name = label;
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    return name.trimmed().isEmpty() ? QStringLiteral("Upload") : name;
}

QList<Entry> entries(const QString &dataHome, const QString &id, const Launcher &launcher,
                     const QString &label, const QString &icon, bool withNautilus,
                     bool withNemo)
{
    const QByteArray marker = QByteArray(kMarker) + ' ' + id.toUtf8();
    // A label is a localestring: a newline in it would end the key.
    QString oneLineLabel = label;
    oneLineLabel.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QByteArray name = oneLineLabel.toUtf8();
    QList<Entry> out;

    // KDE. application/octet-stream is what every regular file inherits from,
    // so the action is offered on any file and on none of the folders.
    // TopLevel keeps it out of the "Actions" submenu: one click, not two.
    out.append({serviceMenuPath(dataHome, id),
                "# " + marker + "\n"
                "[Desktop Entry]\n"
                "Type=Service\n"
                "MimeType=application/octet-stream;\n"
                "X-KDE-ServiceTypes=KonqPopupMenu/Plugin\n"
                "X-KDE-Priority=TopLevel\n"
                "Actions=upload;\n"
                "\n"
                "[Desktop Action upload]\n"
                "Name=" + name + "\n"
                "Icon=" + icon.toUtf8() + "\n"
                "Exec=" + commandLine(launcher, desktopExecArg, QStringLiteral("%F")).toUtf8()
                    + "\n",
                true});

    if (withNautilus) {
        // Nautilus runs the script in the current folder with the selected
        // names as arguments; --upload resolves them against that folder.
        out.append({nautilusDir(dataHome) + QLatin1Char('/') + scriptName(oneLineLabel),
                    "#!/bin/sh\n"
                    "# " + marker + "\n"
                    "exec " + commandLine(launcher, shellQuote, QStringLiteral("\"$@\"")).toUtf8()
                        + "\n",
                    true});
    }

    if (withNemo) {
        // Nemo splits Exec like a shell and expands %F to the quoted paths.
        out.append({nemoPath(dataHome, id),
                    "# " + marker + "\n"
                    "[Nemo Action]\n"
                    "Name=" + name + "\n"
                    "Icon-Name=" + icon.toUtf8() + "\n"
                    "Exec=" + commandLine(launcher, shellQuote, QStringLiteral("%F")).toUtf8()
                        + "\n"
                    "Selection=notnone\n"
                    "Extensions=nodirs;\n",
                    false});
    }
    return out;
}

static bool isOurs(const QString &path, const QByteArray &marker)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    // The marker sits in the first two lines; never read a big stranger whole.
    return f.read(512).contains(marker);
}

QStringList apply(bool enabled, const QString &dataHome, const QString &id,
                  const Launcher &launcher, const QString &label, const QString &icon,
                  QString *error)
{
    const QByteArray marker = QByteArray(kMarker) + ' ' + id.toUtf8();
    const QString nautilus = nautilusDir(dataHome);
    const bool withNautilus = QFileInfo::exists(nautilus)
                              || QFileInfo::exists(dataHome + QStringLiteral("/nautilus"));
    const bool withNemo = QFileInfo::exists(dataHome + QStringLiteral("/nemo"));
    const QList<Entry> wanted =
        entries(dataHome, id, launcher, label, icon, withNautilus, withNemo);

    // Our earlier scripts under another name (the label is translated, and the
    // language may have changed since) would be a second, stale menu item.
    const QFileInfoList scripts = QDir(nautilus).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : scripts) {
        const bool keep = enabled && std::any_of(wanted.cbegin(), wanted.cend(),
            [&fi](const Entry &e) { return e.path == fi.absoluteFilePath(); });
        if (!keep && isOurs(fi.absoluteFilePath(), marker))
            QFile::remove(fi.absoluteFilePath());
    }

    if (!enabled) {
        for (const QString &path : {serviceMenuPath(dataHome, id), nemoPath(dataHome, id)}) {
            if (isOurs(path, marker))
                QFile::remove(path);
        }
        return {};
    }

    QStringList written;
    for (const Entry &e : wanted) {
        // Unchanged: leave the file (and its mtime) alone, so KDE's service
        // cache is not invalidated on every launch.
        QFile existing(e.path);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == e.content
            && (!e.executable || (existing.permissions() & QFileDevice::ExeUser))) {
            written << e.path;
            continue;
        }
        existing.close();
        // A file of the same name that is NOT ours is the user's: keep it.
        if (QFileInfo::exists(e.path) && !isOurs(e.path, marker))
            continue;
        QDir().mkpath(QFileInfo(e.path).absolutePath());
        QSaveFile f(e.path);
        if (!f.open(QIODevice::WriteOnly) || f.write(e.content) != e.content.size()
            || !f.commit()) {
            if (error)
                *error = f.errorString() + QStringLiteral(": ") + e.path;
            continue;
        }
        if (e.executable) {
            QFile::setPermissions(e.path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                              | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                              | QFileDevice::ExeOther);
        }
        written << e.path;
    }
    return written;
}

} // namespace FileManagerMenu
