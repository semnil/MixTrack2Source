/*
Mix Track to Source
Copyright (C) 2026 semnil info@semnil.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace mt2s {

using Version = std::array<int, 3>;

// Accepts only "X.Y.Z" made of decimal digits, the release tag form that .github/scripts/release-tag-shape
// accepts for a release that is not a prerelease
inline std::optional<Version> parse_version(const QString &text)
{
	static const QRegularExpression pattern(QRegularExpression::anchoredPattern("(\\d+)\\.(\\d+)\\.(\\d+)"));
	const QRegularExpressionMatch match = pattern.match(text);
	if (!match.hasMatch())
		return std::nullopt;

	Version version{};
	for (int i = 0; i < 3; i++) {
		bool ok = false;
		version[i] = match.captured(i + 1).toInt(&ok);
		if (!ok)
			return std::nullopt;
	}
	return version;
}

struct ReleaseAsset {
	QString url;
	QString sha256;
};

// Picks the first https asset whose name ends with the suffix and that has a sha256 digest
inline std::optional<ReleaseAsset> find_asset(const QJsonArray &assets, const QString &suffix)
{
	static const QRegularExpression sha256_pattern(QRegularExpression::anchoredPattern("sha256:([0-9a-f]{64})"));

	for (const QJsonValue &value : assets) {
		const QJsonObject asset = value.toObject();
		if (!asset.value("name").toString().endsWith(suffix))
			continue;

		const QRegularExpressionMatch digest = sha256_pattern.match(asset.value("digest").toString());
		const QString url = asset.value("browser_download_url").toString();
		if (!digest.hasMatch() || !url.startsWith("https://"))
			continue;

		return ReleaseAsset{url, digest.captured(1)};
	}
	return std::nullopt;
}

// Returns preferred when it names an existing directory, and fallback otherwise
inline QString existing_dir_or(const QString &preferred, const QString &fallback)
{
	return !preferred.isEmpty() && QFileInfo(preferred).isDir() ? preferred : fallback;
}

// Returns the lowercase hex sha256 of the file, or an empty string when it cannot be read
inline QString file_sha256(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file))
		return {};
	return QString::fromLatin1(hash.result().toHex());
}

// True for a regular file, not a symbolic link, with the given sha256; outside Windows the current user must own it
inline bool is_reusable_download(const QString &path, const QString &sha256)
{
	const QFileInfo info(path);
	if (!info.isFile() || info.isSymLink())
		return false;
#ifndef _WIN32
	if (info.ownerId() != getuid())
		return false;
#endif
	return file_sha256(path) == sha256;
}

} // namespace mt2s
