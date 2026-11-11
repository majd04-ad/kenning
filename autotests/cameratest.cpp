/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "camera.h"

#include <QTemporaryDir>
#include <QTest>

#include <fstream>
#include <map>

using namespace FaceUnlock;

namespace
{
constexpr CameraFormats irSensor{.capture = true, .loopback = false, .grey = true, .colour = false};
constexpr CameraFormats webcam{.capture = true, .loopback = false, .grey = false, .colour = true};
constexpr CameraFormats metadataNode{};
constexpr CameraFormats virtualCamera{.capture = true, .loopback = true, .grey = false, .colour = true};

CameraProbe probing(const std::map<std::string, CameraFormats> &cameras)
{
    return [cameras](const std::string &path) {
        const auto found = cameras.find(path);
        return found == cameras.end() ? CameraFormats{} : found->second;
    };
}
}

class CameraTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_directory;

    Config config(const std::string &devicePath, bool colour = false)
    {
        const std::string path = m_directory.filePath(QStringLiteral("config.ini")).toStdString();
        std::ofstream(path) << "[video]\ndevice_path = " << devicePath << "\ncolour = " << (colour ? "true" : "false") << "\n";
        return Config(path);
    }

private Q_SLOTS:
    void prefersTheInfraredSensor()
    {
        const Camera camera = findCamera({"usb-1.0", "usb-1.2"}, probing({{"usb-1.0", webcam}, {"usb-1.2", irSensor}}));
        QCOMPARE(camera.path, std::string("usb-1.2"));
        QVERIFY(!camera.colour);
    }

    void fallsBackToTheFirstWebcam()
    {
        const Camera camera = findCamera({"meta", "usb-a", "usbv2-a"}, probing({{"meta", metadataNode}, {"usb-a", webcam}, {"usbv2-a", webcam}}));
        QCOMPARE(camera.path, std::string("usb-a"));
        QVERIFY(camera.colour);
    }

    void leavesOutVirtualCameras()
    {
        QVERIFY(findCamera({"loop"}, probing({{"loop", virtualCamera}})).path.empty());
    }

    void listsOnlyTheFirstNodeOfEachCamera()
    {
        QTemporaryDir directory;
        for (const char *name : {"pci-usb-0:6:1.2-video-index0", "pci-usb-0:6:1.0-video-index0", "pci-usb-0:6:1.0-video-index1", "other"}) {
            std::ofstream(directory.filePath(QString::fromLatin1(name)).toStdString()) << "";
        }
        const std::vector<std::string> paths = stableCameraPaths(directory.path().toStdString());
        QCOMPARE(paths.size(), std::size_t(2));
        QVERIFY(paths[0].ends_with("1.0-video-index0"));
        QVERIFY(paths[1].ends_with("1.2-video-index0"));
        QVERIFY(stableCameraPaths(directory.filePath(QStringLiteral("missing")).toStdString()).empty());
    }

    void usesTheConfiguredCamera()
    {
        // /dev/null stands in for a camera that is there: a character device.
        const std::optional<Camera> camera = cameraFor(config("/dev/null", true), [] {
            return Camera{.path = "another", .colour = false};
        });
        QVERIFY(camera);
        QCOMPARE(camera->path, std::string("/dev/null"));
        QVERIFY(camera->colour);
    }

    void neverReplacesAConfiguredCameraThatIsAway()
    {
        // Switched off or unplugged: an IR sensor must not quietly become whatever else is there.
        QVERIFY(!cameraFor(config("/dev/v4l/by-path/gone-video-index0"), [] {
            return Camera{.path = "/dev/null", .colour = true};
        }));
        // Nor is an ordinary file a camera.
        QVERIFY(!cameraFor(config(m_directory.filePath(QStringLiteral("config.ini")).toStdString())));
    }

    void findsAnInfraredCameraWhenNoneIsConfigured_data()
    {
        QTest::addColumn<QString>("configured");
        QTest::newRow("none") << QStringLiteral("none");
        QTest::newRow("empty") << QString();
    }

    void findsAnInfraredCameraWhenNoneIsConfigured()
    {
        QFETCH(QString, configured);
        const Config unconfigured = config(configured.toStdString());
        QVERIFY(!cameraConfigured(unconfigured));
        const std::optional<Camera> camera = cameraFor(unconfigured, [] {
            return Camera{.path = "/dev/ir", .colour = false};
        });
        QVERIFY(camera);
        QCOMPARE(camera->path, std::string("/dev/ir"));
        QVERIFY(!camera->colour);
    }

    void aWebcamHasToBeChosenInSetUp()
    {
        QVERIFY(!cameraFor(config("none"), [] {
            return Camera{.path = "/dev/webcam", .colour = true};
        }));
        QVERIFY(!cameraFor(config("none"), [] {
            return Camera{};
        }));
    }
};

QTEST_GUILESS_MAIN(CameraTest)
#include "cameratest.moc"
