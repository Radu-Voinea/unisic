#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// "Upload with Unisic" in a file manager's right-click menu: a KDE service
// menu (Dolphin, the Plasma desktop and every KDE file dialog), a Nautilus
// script and a Nemo action, each of which runs `unisic --upload -- <files>`.
// That command hands the files to the running instance, which uploads them to
// the active server and copies the link.
//
// Written by the app at runtime instead of installed by the packages: a
// Flatpak cannot export a service menu at all, and one code path for every
// install keeps a package-installed copy from showing up twice next to the
// runtime one. Generation is pure (tests/FileManagerMenuTest.cpp); apply()
// is the only part that touches disk.
namespace FileManagerMenu {

// Every file this writes carries this line, so remove() and a renamed
// (re-translated) Nautilus script never touch a file the user made.
inline constexpr char kMarker[] = "unisic-upload-menu";

struct Launcher {
    // The argv prefix that starts THIS install of Unisic from the host:
    // {"/usr/bin/unisic"}, or {"flatpak", "run", "--user", "app.unisic.Unisic"}.
    QStringList program;
    // Flatpak only: wrap the file list in @@ ... @@ after --file-forwarding,
    // so each file is granted to the sandbox through the document portal.
    // Without it the sandbox can open nothing outside Pictures and Videos.
    bool fileForwarding = false;
};

struct Entry {
    QString path;        // absolute
    QByteArray content;
    bool executable;     // Dolphin refuses a user-dir service menu without +x
};

// Desktop Entry spec Exec quoting for one argument (quoted only when needed,
// reserved characters escaped, then the string-value backslash rule).
QString desktopExecArg(const QString &arg);
// POSIX sh single-quote form.
QString shellQuote(const QString &arg);

// The files for `id` (desktop file id, e.g. "app.unisic.Unisic") under the
// host's XDG data home. Nautilus and Nemo entries only when that file
// manager's data dir exists (`withNautilus` / `withNemo`), so a desktop that
// has neither does not grow their folders.
QList<Entry> entries(const QString &dataHome, const QString &id, const Launcher &launcher,
                     const QString &label, const QString &icon, bool withNautilus,
                     bool withNemo);

// Writes the entries (enabled) or deletes every marked file of `id`
// (disabled). Returns the paths written, or an error string in *error.
QStringList apply(bool enabled, const QString &dataHome, const QString &id,
                  const Launcher &launcher, const QString &label, const QString &icon,
                  QString *error);

// The KDE service menu path for `id`: what the F8 check and the dev button
// read back.
QString serviceMenuPath(const QString &dataHome, const QString &id);

} // namespace FileManagerMenu
