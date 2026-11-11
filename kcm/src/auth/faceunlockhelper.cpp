/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "faceunlockhelper.h"
#include "camerafinder.h"
#include "lockscreenstate.h"
#include "paths.h"
#include "service.h"
#include "settingsschema.h"

#include <KAuth/HelperSupport>
#include <KConfig>
#include <KConfigGroup>
#include <KUser>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>
#include <memory>
#include <unistd.h>

namespace
{
QString callerName()
{
    return KUser(KUserId(HelperSupport::callerUid())).loginName();
}

ActionReply failure(const QString &message)
{
    ActionReply reply = ActionReply::HelperErrorReply();
    reply.setErrorDescription(message);
    return reply;
}

constexpr qsizetype maxThemeArchiveBytes = 32 * 1024 * 1024;
constexpr qint64 maxThemeUnpackedBytes = 64 * 1024 * 1024;
constexpr int maxThemeEntries = 1000;
constexpr int maxThemeDepth = 16;

// Counts what the archive really unpacks to, rather than trusting the sizes it states.
QString checkThemeArchiveSize(const QString &archivePath)
{
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(archivePath).constData(), 64 * 1024) != ARCHIVE_OK) {
        return QStringLiteral("The file is not a theme archive that can be unpacked");
    }
    qint64 unpackedBytes = 0;
    int entries = 0;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++entries > maxThemeEntries) {
            return QStringLiteral("The theme archive has more than %1 files").arg(maxThemeEntries);
        }
        if (QString::fromUtf8(archive_entry_pathname_utf8(entry)).count(QLatin1Char('/')) > maxThemeDepth) {
            return QStringLiteral("The theme archive's folders are nested too deeply");
        }
        char buffer[64 * 1024];
        la_ssize_t read = 0;
        while ((read = archive_read_data(reader.get(), buffer, sizeof(buffer))) > 0) {
            unpackedBytes += read;
            if (unpackedBytes > maxThemeUnpackedBytes) {
                return QStringLiteral("The theme unpacks to more than %1 MB").arg(maxThemeUnpackedBytes / 1024 / 1024);
            }
        }
        if (read < 0) {
            return QStringLiteral("The file is not a theme archive that can be unpacked");
        }
    }
    return status == ARCHIVE_EOF ? QString() : QStringLiteral("The file is not a theme archive that can be unpacked");
}

// Returns an error message, or an empty string on success.
QString unpackThemeArchive(const QByteArray &archive, const QString &archivePath, const QString &extractPath)
{
    QDir().mkpath(extractPath);
    QSaveFile file(archivePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(archive) != archive.size() || !file.commit()) {
        return QStringLiteral("Could not store the theme archive");
    }
    if (const QString error = checkThemeArchiveSize(archivePath); !error.isEmpty()) {
        return error;
    }

    // bsdtar refuses absolute paths and ".." by default; don't keep owners or modes.
    constexpr int unpackTimeoutMs = 60000;
    QProcess bsdtar;
    bsdtar.start(
        QStringLiteral("/usr/bin/bsdtar"),
        {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), extractPath, QStringLiteral("--no-same-owner"), QStringLiteral("--no-same-permissions")});
    if (!bsdtar.waitForFinished(unpackTimeoutMs) || bsdtar.exitStatus() != QProcess::NormalExit || bsdtar.exitCode() != 0) {
        return QStringLiteral("The file is not a theme archive that can be unpacked");
    }

    // Only plain files and directories: a symlink could point anywhere on the system.
    QDirIterator entries(extractPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QFileInfo entry(entries.next());
        if (entry.isSymLink() || !(entry.isFile() || entry.isDir())) {
            return QStringLiteral("Theme archives may only contain plain files and folders");
        }
    }
    return {};
}

// metadata.json and contents/ui/main.qml, at the top of the archive or in its only entry.
QString findThemeRoot(const QString &extractPath)
{
    const QStringList entries = QDir(extractPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (!QFile::exists(extractPath + QStringLiteral("/metadata.json")) && entries.size() == 1) {
        return extractPath + QLatin1Char('/') + entries.first();
    }
    return extractPath;
}

ActionReply runStreaming(const QString &program, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted()) {
        return failure(QStringLiteral("Could not start %1").arg(program));
    }

    QString output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        while (process.canReadLine()) {
            const QString line = QString::fromUtf8(process.readLine()).trimmed();
            output += line + QLatin1Char('\n');
            if (!line.isEmpty()) {
                HelperSupport::progressStep(QVariantMap{{QStringLiteral("line"), line}});
            }
        }
        if (HelperSupport::isStopped()) {
            process.kill();
        }
    }
    output += QString::fromUtf8(process.readAll());

    ActionReply reply = ActionReply::SuccessReply();
    reply.addData(QStringLiteral("exitCode"), process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1);
    reply.addData(QStringLiteral("output"), output);
    return reply;
}

bool writeFile(const QString &path, const QByteArray &contents, QFileDevice::Permissions permissions)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(contents);
    return file.commit() && QFile::setPermissions(path, permissions);
}

constexpr auto standardPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;

// Keeps comments and layout.
bool updateEngineConfig(const QVariantMap &values)
{
    QFile file(FaceUnlock::engineConfigPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();

    QVariantMap missing = values;
    QString section;
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            section = trimmed.mid(1, trimmed.size() - 2);
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QString key = section + QLatin1Char('/') + line.left(equals).trimmed();
        if (values.contains(key)) {
            line = line.left(equals).trimmed() + QStringLiteral(" = ") + values.value(key).toString();
            missing.remove(key);
        }
    }
    // Keys newer than the user's config.ini (pacman keeps an edited one) go under their section.
    for (auto it = missing.cbegin(); it != missing.cend(); ++it) {
        const QString group = it.key().section(QLatin1Char('/'), 0, 0);
        const QString entry = it.key().section(QLatin1Char('/'), 1) + QStringLiteral(" = ") + it.value().toString();
        const auto header = std::find_if(lines.begin(), lines.end(), [&group](const QString &line) {
            return line.trimmed() == QLatin1Char('[') + group + QLatin1Char(']');
        });
        if (header == lines.end()) {
            lines << QString() << QLatin1Char('[') + group + QLatin1Char(']') << entry;
        } else {
            lines.insert(header + 1, entry);
        }
    }
    return writeFile(FaceUnlock::engineConfigPath, lines.join(QLatin1Char('\n')).toUtf8(), standardPermissions);
}

// A service without an /etc override is first copied from /usr/lib/pam.d.
bool setPamFace(const FaceUnlock::PamService &service, bool enabled)
{
    // The lock screen may use Plasma's own Face sign-in method instead; pam-tidy knows which.
    if (service.id == QLatin1String("lockscreen")) {
        return QProcess::execute(FaceUnlock::pamTidy, {QStringLiteral("lockscreen"), enabled ? QStringLiteral("on") : QStringLiteral("off")}) == 0;
    }
    QString source = service.path;
    if (!QFile::exists(source)) {
        if (!enabled) {
            return true;
        }
        source = QStringLiteral("/usr/lib/pam.d/") + QFileInfo(service.path).fileName();
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        return !enabled;
    }
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();

    const bool present = std::any_of(lines.cbegin(), lines.cend(), FaceUnlock::isPamLine);
    const bool leftover = std::any_of(lines.cbegin(), lines.cend(), [](const QString &line) {
        return FaceUnlock::isFacePamLine(line) && !FaceUnlock::isPamLine(line);
    });
    if (present == enabled && !leftover && source == service.path) {
        return true;
    }

    lines.erase(std::remove_if(lines.begin(), lines.end(), FaceUnlock::isFacePamLine), lines.end());
    if (enabled) {
        const auto firstAuth = std::find_if(lines.begin(), lines.end(), [](const QString &line) {
            return line.trimmed().startsWith(QLatin1String("auth"));
        });
        lines.insert(firstAuth, FaceUnlock::pamLine);
    }
    return writeFile(service.path, lines.join(QLatin1Char('\n')).toUtf8(), standardPermissions);
}
}

namespace
{
// The config stays readable: pam_kenning checks it in the caller's process.
bool secureModels()
{
    const QString &models = FaceUnlock::modelsPath;
    QDir().mkpath(models);
    QFile::setPermissions(models, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString owner = FaceUnlock::serviceUserName + QLatin1Char(':') + FaceUnlock::serviceUserName;
    const bool owned = QProcess::execute(QStringLiteral("/usr/bin/chown"), {QStringLiteral("-R"), QStringLiteral("-h"), owner, models}) == 0;
    bool secured = owned;
    for (const QFileInfo &model : QDir(models).entryInfoList(QDir::Files)) {
        secured &= QFile::setPermissions(model.absoluteFilePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QFile::setPermissions(FaceUnlock::engineConfigPath, standardPermissions);
    return secured;
}

// Writes down the camera when none is: first setup, or a config.ini that lost its settings.
// A configured camera that is away (switched off, unplugged) is kept: it comes back.
void rememberCamera()
{
    if (FaceUnlock::cameraConfigured(FaceUnlock::Config(FaceUnlock::configPath))) {
        return;
    }
    if (const FaceUnlock::Camera camera = FaceUnlock::findCamera(); !camera.path.empty()) {
        updateEngineConfig({{QStringLiteral("video/device_path"), QString::fromStdString(camera.path)},
                            {QStringLiteral("video/colour"), camera.colour ? QStringLiteral("true") : QStringLiteral("false")}});
    }
}

// Set Up may also move to the camera that is there now, when the configured one is gone for good.
void prepareEngine()
{
    const FaceUnlock::Config config(FaceUnlock::configPath);
    if (FaceUnlock::cameraConfigured(config) && !FaceUnlock::isCharacterDevice(config.string("video/device_path"))) {
        if (const FaceUnlock::Camera camera = FaceUnlock::findCamera(); !camera.path.empty()) {
            updateEngineConfig({{QStringLiteral("video/device_path"), QString::fromStdString(camera.path)},
                                {QStringLiteral("video/colour"), camera.colour ? QStringLiteral("true") : QStringLiteral("false")}});
        }
    }
    rememberCamera();
    secureModels();
}
}

ActionReply FaceUnlockHelper::list(const QVariantMap &)
{
    // Needs no password, so only what the page shows: never the face data itself.
    QJsonArray models;
    QFile file(FaceUnlock::modelPath(callerName()));
    if (file.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).array()) {
            const QJsonObject model = value.toObject();
            models.append(QJsonObject{
                {QStringLiteral("id"), model.value(QStringLiteral("id"))},
                {QStringLiteral("label"), model.value(QStringLiteral("label"))},
                {QStringLiteral("time"), model.value(QStringLiteral("time"))},
            });
        }
    }
    ActionReply reply = ActionReply::SuccessReply();
    reply.addData(QStringLiteral("models"), QString::fromUtf8(QJsonDocument(models).toJson(QJsonDocument::Compact)));
    return reply;
}

ActionReply FaceUnlockHelper::add(const QVariantMap &args)
{
    QString label = args.value(QStringLiteral("label")).toString().remove(QLatin1Char(',')).simplified().left(FaceUnlock::maxLabelLength);
    if (label.isEmpty()) {
        label = QStringLiteral("Face");
    }
    prepareEngine();
    // The service captures the face in its sandbox; this helper never runs OpenCV as root.
    constexpr int enrolmentLineTimeoutSeconds = 30;
    QString output;
    const FaceUnlock::Reply result = FaceUnlock::callService({
        .command = "enroll",
        .user = callerName().toStdString(),
        .argument = label.toStdString(),
        .idleTimeoutSeconds = enrolmentLineTimeoutSeconds,
        .onMessage =
            [&output](FaceUnlock::MessageKind, const std::string &text) {
                const QString line = QString::fromStdString(text).trimmed();
                output += line + QLatin1Char('\n');
                if (!line.isEmpty()) {
                    HelperSupport::progressStep(QVariantMap{{QStringLiteral("line"), line}});
                }
            },
        .cancelled = HelperSupport::isStopped,
    });
    if (result.code < 0) {
        output += QStringLiteral("The Kenning service isn't reachable\n");
    }
    if (!secureModels() && result.code == FaceUnlock::Recognized) {
        return failure(QStringLiteral("The face was saved, but the Kenning service can't be given access to it"));
    }
    ActionReply reply = ActionReply::SuccessReply();
    reply.addData(QStringLiteral("exitCode"), result.code);
    reply.addData(QStringLiteral("output"), output);
    return reply;
}

ActionReply FaceUnlockHelper::remove(const QVariantMap &args)
{
    bool ok = false;
    const int id = args.value(QStringLiteral("id")).toInt(&ok);
    if (!ok || id < 0) {
        return failure(QStringLiteral("Invalid face model id"));
    }
    return runStreaming(FaceUnlock::engineBinary,
                        {QStringLiteral("--plain"), QStringLiteral("-y"), QStringLiteral("-U"), callerName(), QStringLiteral("remove"), QString::number(id)});
}

ActionReply FaceUnlockHelper::save(const QVariantMap &args)
{
    const QVariantMap engine = args.value(QStringLiteral("engine")).toMap();
    QVariantMap accepted;
    for (auto it = engine.cbegin(); it != engine.cend(); ++it) {
        const auto setting = FaceUnlock::engineSetting(it.key());
        if (!setting || !setting->accepts(it.value().toString())) {
            return failure(QStringLiteral("Rejected engine setting %1").arg(it.key()));
        }
        accepted.insert(it.key(), it.value());
    }
    if (!accepted.isEmpty() && !updateEngineConfig(accepted)) {
        return failure(QStringLiteral("Could not write %1").arg(FaceUnlock::engineConfigPath));
    }
    rememberCamera();

    const QVariantMap pam = args.value(QStringLiteral("pam")).toMap();
    for (const auto &service : FaceUnlock::pamServices()) {
        if (pam.contains(service.id) && !setPamFace(service, pam.value(service.id).toBool())) {
            return failure(QStringLiteral("Could not update %1").arg(service.path));
        }
    }

    const QVariantMap system = args.value(QStringLiteral("system")).toMap();
    if (!system.isEmpty()) {
        KConfig config(FaceUnlock::systemConfigPath, KConfig::SimpleConfig);
        for (auto it = system.cbegin(); it != system.cend(); ++it) {
            const auto setting = FaceUnlock::systemSetting(it.key());
            if (!setting || !setting->accepts(it.value().toString())) {
                return failure(QStringLiteral("Rejected setting %1").arg(it.key()));
            }
            config.group(setting->group).writeEntry(setting->key, it.value().toString());
        }
        if (!config.sync() || !QFile::setPermissions(FaceUnlock::systemConfigPath, standardPermissions)) {
            return failure(QStringLiteral("Could not write %1").arg(FaceUnlock::systemConfigPath));
        }
    }

    return ActionReply::SuccessReply();
}

ActionReply FaceUnlockHelper::installtheme(const QVariantMap &args)
{
    // Read by the page as the user, so the helper never opens a path chosen by the caller.
    const QByteArray archive = args.value(QStringLiteral("archive")).toByteArray();
    if (archive.isEmpty() || archive.size() > maxThemeArchiveBytes) {
        return failure(QStringLiteral("The theme archive is empty or larger than 32 MB"));
    }

    // Unpack next to the themes directory so the final move stays on one filesystem.
    QDir().mkpath(FaceUnlock::userThemesPath);
    QTemporaryDir work(FaceUnlock::userThemesPath + QStringLiteral("/../.install-XXXXXX"));
    if (!work.isValid()) {
        return failure(QStringLiteral("Could not create a temporary directory"));
    }
    const QString extractPath = work.filePath(QStringLiteral("theme"));
    if (const QString error = unpackThemeArchive(archive, work.filePath(QStringLiteral("theme.archive")), extractPath); !error.isEmpty()) {
        return failure(error);
    }

    const QString themeRoot = findThemeRoot(extractPath);
    QFile metadataFile(themeRoot + QStringLiteral("/metadata.json"));
    if (!metadataFile.open(QIODevice::ReadOnly) || !QFile::exists(themeRoot + QStringLiteral("/contents/ui/main.qml"))) {
        return failure(QStringLiteral("The archive has no metadata.json and contents/ui/main.qml"));
    }
    const QJsonObject metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    const QString id = metadata.value(QStringLiteral("KPlugin")).toObject().value(QStringLiteral("Id")).toString();
    if (!FaceUnlock::isThemeId(id)) {
        return failure(QStringLiteral("The theme metadata has no valid KPlugin.Id"));
    }

    const QString destination = FaceUnlock::userThemesPath + QLatin1Char('/') + id;
    QDir(destination).removeRecursively();
    if (!QDir().rename(themeRoot, destination)) {
        return failure(QStringLiteral("Could not install the theme to %1").arg(destination));
    }
    QProcess::execute(QStringLiteral("/usr/bin/chown"), {QStringLiteral("-R"), QStringLiteral("-h"), QStringLiteral("root:root"), destination});
    QProcess::execute(QStringLiteral("/usr/bin/chmod"), {QStringLiteral("-R"), QStringLiteral("u=rwX,go=rX"), destination});

    ActionReply reply = ActionReply::SuccessReply();
    reply.addData(QStringLiteral("id"), id);
    return reply;
}

ActionReply FaceUnlockHelper::removetheme(const QVariantMap &args)
{
    const QString id = args.value(QStringLiteral("id")).toString();
    if (!FaceUnlock::isThemeId(id)) {
        return failure(QStringLiteral("Invalid theme id"));
    }
    if (!QDir(FaceUnlock::userThemesPath + QLatin1Char('/') + id).removeRecursively()) {
        return failure(QStringLiteral("Could not remove the theme"));
    }
    return ActionReply::SuccessReply();
}

KAUTH_HELPER_MAIN("org.kde.kcontrol.kcmkenning", FaceUnlockHelper)

#include "moc_faceunlockhelper.cpp"
