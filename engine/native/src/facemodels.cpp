/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "facemodels.h"
#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <iostream>
#include <pwd.h>
#include <unistd.h>

namespace FaceUnlock
{
namespace
{
const QString idKey = QStringLiteral("id");
const QString labelKey = QStringLiteral("label");
const QString timeKey = QStringLiteral("time");
const QString encodingsKey = QStringLiteral("data");

cv::Mat featureFromJson(const QJsonArray &numbers)
{
    cv::Mat feature(1, int(numbers.size()), CV_32F);
    for (int i = 0; i < numbers.size(); ++i) {
        feature.at<float>(i) = float(numbers.at(i).toDouble());
    }
    return feature;
}

QJsonArray featureToJson(const cv::Mat &feature)
{
    QJsonArray numbers;
    for (int i = 0; i < feature.cols; ++i) {
        numbers.append(double(feature.at<float>(i)));
    }
    return numbers;
}
}

std::vector<FaceModel> readModels(const std::string &path)
{
    std::vector<FaceModel> models;
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return models;
    }
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject object = value.toObject();
        FaceModel model{
            .id = object.value(idKey).toInt(),
            .label = object.value(labelKey).toString().toStdString(),
            .time = object.value(timeKey).toInteger(),
            .encodings = {},
        };
        for (const auto &encoding : object.value(encodingsKey).toArray()) {
            model.encodings.push_back(featureFromJson(encoding.toArray()));
        }
        models.push_back(model);
    }
    return models;
}

bool writeModels(const std::string &path, const std::vector<FaceModel> &models)
{
    QJsonArray array;
    for (const FaceModel &model : models) {
        QJsonArray encodings;
        for (const cv::Mat &feature : model.encodings) {
            encodings.append(featureToJson(feature));
        }
        array.append(QJsonObject{
            {idKey, model.id},
            {labelKey, QString::fromStdString(model.label)},
            {timeKey, model.time},
            {encodingsKey, encodings},
        });
    }
    const QString filePath = QString::fromStdString(path);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    // Set on the path: after commit() the QSaveFile no longer refers to the saved file.
    return file.commit() && QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

std::vector<FaceModel> loadModels(const std::string &user)
{
    return readModels(modelPath(user));
}

bool saveModels(const std::string &user, const std::vector<FaceModel> &models)
{
    const QString directory = QString::fromUtf8(modelsDir);
    if (QDir().mkpath(directory)) {
        QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    const std::string path = modelPath(user);
    return writeModels(path, models) && giveToService(path);
}

std::string faceLabel(std::string label, int id)
{
    std::erase_if(label, [](char c) {
        return c == ',' || (static_cast<unsigned char>(c) < 0x20) || c == 0x7f;
    });
    if (label.size() > std::size_t(maxLabelLength)) {
        label.resize(std::size_t(maxLabelLength));
        // Drop a character the cut split: trailing continuation bytes, then their lead byte.
        while (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xC0) == 0x80) {
            label.pop_back();
        }
        if (!label.empty() && static_cast<unsigned char>(label.back()) >= 0xC0) {
            label.pop_back();
        }
    }
    return label.empty() ? "Model #" + std::to_string(id) : label;
}

bool giveToService(const std::string &path)
{
    if (geteuid() != 0) {
        return true;
    }
    const passwd *account = getpwnam(serviceUser);
    if (!account || chown(path.c_str(), account->pw_uid, account->pw_gid) != 0 || chown(modelsDir, account->pw_uid, account->pw_gid) != 0) {
        std::cerr << "Could not hand " << path << " to the " << serviceUser << " user, so face unlock can't read it\n";
        return false;
    }
    return true;
}
}
