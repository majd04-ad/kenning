/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "faceunlockkcm.h"
#include "camerafinder.h"
#include "camerapreview.h"
#include "lockscreenstate.h"
#include "paths.h"
#include "settingsschema.h"

#include <KAuth/Action>
#include <KAuth/ExecuteJob>
#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>
#include <KUser>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlEngine>
#include <QUrl>

K_PLUGIN_CLASS_WITH_JSON(FaceUnlockKcm, "kcm_kenning.json")

namespace
{
const QString helperId = QStringLiteral("org.kde.kcontrol.kcmkenning");

namespace Key
{
const QString animationStyle = QStringLiteral("Appearance/AnimationStyle");
const QString autoScan = QStringLiteral("LoginScreen/AutoScan");
const QString welcomePause = QStringLiteral("LoginScreen/WelcomePause");
const QString showButton = QStringLiteral("lock/faceUnlockShowButton");
const QString unlockDelay = QStringLiteral("lock/unlockDelay");
const QString preferredMethod = QStringLiteral("lock/preferredMethod");
const QString useLastMethod = QStringLiteral("lock/useLastMethod");
const QString lockScreenFace = QStringLiteral("pam/lockscreen");
const QString pamPrefix = QStringLiteral("pam/");
}

QVariantMap readEngineConfig()
{
    QVariantMap values;
    const FaceUnlock::Config config(FaceUnlock::configPath);
    for (const auto &[key, value] : config.values()) {
        values.insert(QString::fromStdString(key), QString::fromStdString(value));
    }
    return values;
}

bool pamUsesEngine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    return std::any_of(lines.cbegin(), lines.cend(), FaceUnlock::isPamLine);
}

KConfigGroup lockScreenGroup(const KSharedConfig::Ptr &config)
{
    return config->group(QStringLiteral("Greeter")).group(QStringLiteral("LnF")).group(QStringLiteral("General"));
}

// Whether fprintd has a fingerprint of this user on any reader.
bool hasFingerprint(const QString &user)
{
    constexpr int timeoutMs = 3000;
    const QString service = QStringLiteral("net.reactivated.Fprint");
    const QDBusMessage devices = QDBusConnection::systemBus().call(QDBusMessage::createMethodCall(service,
                                                                                                  QStringLiteral("/net/reactivated/Fprint/Manager"),
                                                                                                  service + QStringLiteral(".Manager"),
                                                                                                  QStringLiteral("GetDevices")),
                                                                   QDBus::Block,
                                                                   timeoutMs);
    if (devices.type() != QDBusMessage::ReplyMessage || devices.arguments().isEmpty()) {
        return false;
    }
    const auto paths = qdbus_cast<QList<QDBusObjectPath>>(devices.arguments().constFirst());
    return std::any_of(paths.cbegin(), paths.cend(), [&](const QDBusObjectPath &path) {
        QDBusMessage call = QDBusMessage::createMethodCall(service, path.path(), service + QStringLiteral(".Device"), QStringLiteral("ListEnrolledFingers"));
        call << user;
        // No fingerprints is an error reply (NoEnrolledPrints), not an empty list.
        const QDBusMessage fingers = QDBusConnection::systemBus().call(call, QDBus::Block, timeoutMs);
        return fingers.type() == QDBusMessage::ReplyMessage && !fingers.arguments().isEmpty() && !fingers.arguments().constFirst().toStringList().isEmpty();
    });
}

QVariant fromConfigText(const QVariant &defaultValue, const QString &text)
{
    switch (defaultValue.typeId()) {
    case QMetaType::Bool:
        return text == QLatin1String("true");
    case QMetaType::Int:
        return text.toInt();
    case QMetaType::Double:
        return text.toDouble();
    default:
        return text;
    }
}

QString toConfigText(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return value.toString();
}

KAuth::ExecuteJob *helperJob(const QString &action, const QVariantMap &arguments = {}, int timeoutMs = -1)
{
    KAuth::Action request(helperId + QLatin1Char('.') + action);
    request.setHelperId(helperId);
    request.setArguments(arguments);
    request.setTimeout(timeoutMs);
    return request.execute();
}
}

FaceUnlockKcm::FaceUnlockKcm(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
{
    qmlRegisterType<CameraPreview>("org.kde.kenning", 1, 0, "CameraPreview");
    setButtons(Apply | Default);
    // The page binds to every setting as it is created, before load() has read them.
    m_settings = defaultSettings();
    m_loaded = m_settings;
}

QVariantMap FaceUnlockKcm::defaultSettings()
{
    return {
        {QStringLiteral("core/disabled"), false},
        {QStringLiteral("core/detection_notice"), true},
        {QStringLiteral("core/no_confirmation"), false},
        {QStringLiteral("core/abort_if_ssh"), true},
        {QStringLiteral("core/abort_if_lid_closed"), true},
        {QStringLiteral("core/password_after_boot"), true},
        {QStringLiteral("video/certainty"), 6.0},
        {QStringLiteral("video/timeout"), 4},
        {QStringLiteral("video/dark_threshold"), 60},
        {QStringLiteral("pam/login"), true},
        {Key::lockScreenFace, true},
        {QStringLiteral("pam/sudo"), true},
        {QStringLiteral("pam/polkit"), false},
        {Key::animationStyle, QStringLiteral("hello")},
        {Key::autoScan, true},
        {Key::welcomePause, 1000},
        {Key::showButton, false},
        {Key::unlockDelay, 500},
        {Key::preferredMethod, QStringLiteral("face")},
        {Key::useLastMethod, false},
    };
}

void FaceUnlockKcm::load()
{
    const QVariantMap defaults = defaultSettings();
    QVariantMap settings = defaults;

    const QVariantMap engine = readEngineConfig();
    for (auto it = engine.cbegin(); it != engine.cend(); ++it) {
        if (defaults.contains(it.key())) {
            settings.insert(it.key(), fromConfigText(defaults.value(it.key()), it.value().toString()));
        }
    }
    // The camera scans use. With one configured that is away (switched off, unplugged), Set Up
    // could still move to another that is there now.
    const FaceUnlock::Config engineConfig(FaceUnlock::configPath);
    const bool configured = FaceUnlock::cameraConfigured(engineConfig);
    const std::optional<FaceUnlock::Camera> inUse = FaceUnlock::cameraFor(engineConfig);
    const FaceUnlock::Camera camera = inUse ? *inUse : FaceUnlock::findCamera();
    m_devicePath = QString::fromStdString(camera.path);
    m_colourCamera = camera.colour;
    m_cameraAway = configured && !inUse;
    // The IR picture looks unsettling; preview the webcam when there is one.
    const QString colourPath = m_colourCamera ? QString() : FaceUnlock::findPreviewCamera();
    m_previewPath = colourPath.isEmpty() ? m_devicePath : colourPath;

    for (const auto &service : FaceUnlock::pamServices()) {
        settings.insert(Key::pamPrefix + service.id, pamUsesEngine(service.path));
    }
    const FaceUnlock::LockScreenState lockScreen = FaceUnlock::lockScreenState();
    m_lockScreenFaceService = lockScreen.faceService;
    if (lockScreen.known) {
        settings.insert(Key::lockScreenFace, lockScreen.enabled);
    }

    KConfig system(FaceUnlock::systemConfigPath, KConfig::SimpleConfig);
    for (const QString &key : {Key::animationStyle, Key::autoScan, Key::welcomePause}) {
        const auto setting = FaceUnlock::systemSetting(key);
        const QString text = system.group(setting->group).readEntry(setting->key, toConfigText(defaults.value(key)));
        settings.insert(key, fromConfigText(defaults.value(key), text));
    }

    const KConfigGroup lock = lockScreenGroup(KSharedConfig::openConfig(QStringLiteral("kscreenlockerrc")));
    settings.insert(Key::showButton, lock.readEntry("faceUnlockShowButton", false));
    settings.insert(Key::unlockDelay, lock.readEntry("unlockDelay", 500));
    const QString preferred = lock.readEntry("preferredMethod", QString());
    settings.insert(Key::preferredMethod, preferred == QLatin1String("fingerprint") ? preferred : QStringLiteral("face"));
    settings.insert(Key::useLastMethod, lock.readEntry("useLastMethod", false));
    m_fingerprintEnrolled = hasFingerprint(userName());

    m_settings = settings;
    m_loaded = settings;
    Q_EMIT settingsChanged();
    updateState();
    refreshFaces();
    refreshThemes();
}

void FaceUnlockKcm::save()
{
    QVariantMap engine;
    QVariantMap pam;
    QVariantMap system;
    for (auto it = m_settings.cbegin(); it != m_settings.cend(); ++it) {
        if (m_loaded.value(it.key()) == it.value()) {
            continue;
        }
        const QString key = it.key();
        if (key.startsWith(Key::pamPrefix)) {
            pam.insert(key.mid(Key::pamPrefix.size()), it.value());
        } else if (FaceUnlock::systemSetting(key)) {
            system.insert(key, toConfigText(it.value()));
        } else if (FaceUnlock::engineSetting(key)) {
            engine.insert(key, toConfigText(it.value()));
        }
    }

    auto config = KSharedConfig::openConfig(QStringLiteral("kscreenlockerrc"));
    KConfigGroup lock = lockScreenGroup(config);
    lock.writeEntry("faceUnlockShowButton", m_settings.value(Key::showButton).toBool());
    lock.writeEntry("unlockDelay", m_settings.value(Key::unlockDelay).toInt());
    lock.writeEntry("faceAnimation", m_settings.value(Key::animationStyle).toString());
    // Plasma applies a preferred method only while there is a fingerprint to choose.
    const QString preferred = m_settings.value(Key::preferredMethod).toString();
    const bool useLast = m_settings.value(Key::useLastMethod).toBool();
    lock.writeEntry("preferredMethod", m_fingerprintEnrolled ? preferred : QString());
    lock.writeEntry("useLastMethod", useLast);
    if (m_fingerprintEnrolled && !useLast && preferred == QLatin1String("fingerprint")) {
        // Plasma only offers fingerprint as a method of its own when this is on.
        config->group(QStringLiteral("Authenticators")).writeEntry("Fingerprint", true);
    }
    if (m_lockScreenFaceService) {
        const bool face = m_settings.value(Key::lockScreenFace).toBool();
        config->group(QStringLiteral("Authenticators")).writeEntry("Face", face);
        if (!face) {
            // A remembered method that is switched off crashes the lock screen (https://bugs.kde.org/528042).
            auto state = KSharedConfig::openStateConfig(QStringLiteral("kscreenlockerrc"));
            KConfigGroup greeter = state->group(QStringLiteral("Greeter"));
            if (greeter.readEntry("Authenticator") == QLatin1String("Face")) {
                greeter.deleteEntry("Authenticator");
                state->sync();
            }
        }
    }
    config->sync();

    if (engine.isEmpty() && pam.isEmpty() && system.isEmpty()) {
        m_loaded = m_settings;
        updateState();
        return;
    }

    auto *job = helperJob(QStringLiteral("save"), {{QStringLiteral("engine"), engine}, {QStringLiteral("pam"), pam}, {QStringLiteral("system"), system}});
    connect(job, &KJob::result, this, [this, job] {
        if (job->error()) {
            Q_EMIT errorOccurred(job->errorString());
            setNeedsSave(true);
        } else {
            load();
        }
    });
    job->start();
}

void FaceUnlockKcm::defaults()
{
    const QVariantMap defaults = defaultSettings();
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
        m_settings.insert(it.key(), it.value());
    }
    Q_EMIT settingsChanged();
    updateState();
}

void FaceUnlockKcm::updateState()
{
    setNeedsSave(m_settings != m_loaded);
    setRepresentsDefaults(m_settings == defaultSettings());
}

QVariantMap FaceUnlockKcm::settings() const
{
    return m_settings;
}

void FaceUnlockKcm::setValue(const QString &key, const QVariant &value)
{
    if (!m_settings.contains(key) || m_settings.value(key) == value) {
        return;
    }
    // Keep the type the defaults use, so QML numbers compare equal to loaded ones.
    QVariant converted = value;
    converted.convert(defaultSettings().value(key).metaType());
    m_settings.insert(key, converted);
    Q_EMIT settingsChanged();
    updateState();
}

QVariantList FaceUnlockKcm::faces() const
{
    return m_faces;
}

bool FaceUnlockKcm::busy() const
{
    return m_busy;
}

void FaceUnlockKcm::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

QString FaceUnlockKcm::userName() const
{
    return KUser().loginName();
}

QString FaceUnlockKcm::displayName() const
{
    const QString fullName = KUser().property(KUser::FullName).toString().trimmed();
    return fullName.isEmpty() ? userName() : fullName;
}

QString FaceUnlockKcm::devicePath() const
{
    return m_devicePath;
}

QString FaceUnlockKcm::previewPath() const
{
    return m_previewPath;
}

bool FaceUnlockKcm::colourCamera() const
{
    return m_colourCamera;
}

bool FaceUnlockKcm::cameraAway() const
{
    return m_cameraAway;
}

bool FaceUnlockKcm::engineInstalled() const
{
    return QFile::exists(FaceUnlock::pamModulePath) && QFile::exists(FaceUnlock::engineBinary);
}

bool FaceUnlockKcm::lockScreenPatched() const
{
    QFile file(FaceUnlock::lockScreenPath);
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(FaceUnlock::lockScreenMarker.toUtf8());
}

bool FaceUnlockKcm::loginScreenSupported() const
{
    return QFile::exists(FaceUnlock::loginManagerSupportPath);
}

bool FaceUnlockKcm::fingerprintEnrolled() const
{
    return m_fingerprintEnrolled;
}

void FaceUnlockKcm::refreshFaces()
{
    auto *job = helperJob(QStringLiteral("list"));
    connect(job, &KJob::result, this, [this, job] {
        if (job->error()) {
            return;
        }
        m_faces.clear();
        const auto models = QJsonDocument::fromJson(job->data().value(QStringLiteral("models")).toString().toUtf8()).array();
        for (const auto &value : models) {
            const QJsonObject model = value.toObject();
            m_faces.append(QVariantMap{
                {QStringLiteral("id"), model.value(QStringLiteral("id")).toInt()},
                {QStringLiteral("label"), model.value(QStringLiteral("label")).toString()},
                {QStringLiteral("added"), QDateTime::fromSecsSinceEpoch(model.value(QStringLiteral("time")).toInteger())},
            });
        }
        Q_EMIT facesChanged();
    });
    job->start();
}

QVariantList FaceUnlockKcm::themes() const
{
    return m_themes;
}

void FaceUnlockKcm::refreshThemes()
{
    m_themes.clear();
    QStringList seen;
    for (const QString &base : {FaceUnlock::userThemesPath, FaceUnlock::systemThemesPath}) {
        const QDir dir(base);
        for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            QFile metadataFile(dir.filePath(id + QStringLiteral("/metadata.json")));
            if (seen.contains(id) || !FaceUnlock::isThemeId(id) || !metadataFile.open(QIODevice::ReadOnly)
                || !QFile::exists(dir.filePath(id + QStringLiteral("/contents/ui/main.qml")))) {
                continue;
            }
            const QJsonObject plugin = QJsonDocument::fromJson(metadataFile.readAll()).object().value(QStringLiteral("KPlugin")).toObject();
            const QJsonArray authors = plugin.value(QStringLiteral("Authors")).toArray();
            seen.append(id);
            m_themes.append(QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("name"), plugin.value(QStringLiteral("Name")).toString(id)},
                {QStringLiteral("description"), plugin.value(QStringLiteral("Description")).toString()},
                {QStringLiteral("author"), authors.isEmpty() ? QString() : authors.first().toObject().value(QStringLiteral("Name")).toString()},
                {QStringLiteral("removable"), base == FaceUnlock::userThemesPath},
            });
        }
    }
    Q_EMIT themesChanged();
}

void FaceUnlockKcm::installTheme(const QUrl &archive)
{
    // Read as the user; the helper only sees the bytes.
    QFile file(archive.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(i18n("Could not read %1", archive.toLocalFile()));
        return;
    }
    auto *job = helperJob(QStringLiteral("installtheme"), {{QStringLiteral("archive"), file.readAll()}});
    connect(job, &KJob::result, this, [this, job] {
        if (job->error()) {
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        refreshThemes();
        setValue(Key::animationStyle, job->data().value(QStringLiteral("id")).toString());
    });
    job->start();
}

void FaceUnlockKcm::removeTheme(const QString &id)
{
    auto *job = helperJob(QStringLiteral("removetheme"), {{QStringLiteral("id"), id}});
    connect(job, &KJob::result, this, [this, job, id] {
        if (job->error()) {
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        refreshThemes();
        if (m_settings.value(Key::animationStyle).toString() == id) {
            setValue(Key::animationStyle, QStringLiteral("hello"));
        }
    });
    job->start();
}

void FaceUnlockKcm::addFace(const QString &label)
{
    runHelper(QStringLiteral("add"), {{QStringLiteral("label"), label}});
}

void FaceUnlockKcm::removeFace(int id)
{
    runHelper(QStringLiteral("remove"), {{QStringLiteral("id"), id}});
}

void FaceUnlockKcm::testRecognition()
{
    // The service answers for the user's own account, so no administrator is needed.
    if (m_busy) {
        return;
    }
    setBusy(true);
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        setBusy(false);
        const QStringList lines = QString::fromUtf8(process->readAll()).trimmed().split(QLatin1Char('\n'));
        Q_EMIT operationFinished(QStringLiteral("test"), status == QProcess::NormalExit ? exitCode : -1, lines.value(0));
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setBusy(false);
            Q_EMIT operationFinished(QStringLiteral("test"), -1, process->errorString());
            process->deleteLater();
        }
    });
    process->start(FaceUnlock::engineBinary, {QStringLiteral("test")});
}

void FaceUnlockKcm::runHelper(const QString &operation, const QVariantMap &arguments)
{
    if (m_busy) {
        return;
    }
    setBusy(true);

    constexpr int helperTimeoutMs = 120000;
    auto *job = helperJob(operation, arguments, helperTimeoutMs);
    connect(job, &KAuth::ExecuteJob::newData, this, [this](const QVariantMap &data) {
        Q_EMIT progress(data.value(QStringLiteral("line")).toString());
    });
    connect(job, &KJob::result, this, [this, job, operation] {
        setBusy(false);
        if (job->error()) {
            Q_EMIT operationFinished(operation, -1, job->errorString());
            return;
        }
        const int exitCode = job->data().value(QStringLiteral("exitCode")).toInt();
        const QStringList lines = job->data().value(QStringLiteral("output")).toString().trimmed().split(QLatin1Char('\n'));
        Q_EMIT operationFinished(operation, exitCode, lines.isEmpty() ? QString() : lines.last());
        if (operation != QLatin1String("test")) {
            refreshFaces();
        }
    });
    job->start();
}

#include "faceunlockkcm.moc"
