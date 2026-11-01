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

#include "update-check.h"
#include "update-version.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <curl/curl.h>

#include <QAction>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

namespace {

constexpr const char *LATEST_RELEASE_URL = "https://api.github.com/repos/semnil/MixTrack2Source/releases/latest";
constexpr const char *CONFIG_FILE = "config.json";
constexpr const char *CHECK_AT_STARTUP_KEY = "check_updates_at_startup";
constexpr long CONNECT_TIMEOUT_SECONDS = 30;
constexpr long LOW_SPEED_TIME_SECONDS = 60;

// Mirrors the release file names produced by .github/scripts/Package-Windows.ps1,
// .github/scripts/package-macos and CPACK_PACKAGE_FILE_NAME in cmake/linux/defaults.cmake
#if defined(_WIN32)
constexpr const char *ASSET_SUFFIX = "-windows-x64-Installer.exe";
#elif defined(__APPLE__)
constexpr const char *ASSET_SUFFIX = "-macos-universal.pkg";
#elif defined(__x86_64__)
constexpr const char *ASSET_SUFFIX = "-x86_64-linux-gnu.deb";
#else
constexpr const char *ASSET_SUFFIX = nullptr;
#endif

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool CAN_START_INSTALLER = true;
#else
constexpr bool CAN_START_INSTALLER = false;
#endif

// The worker thread only touches Qt objects that are not shared with the UI thread,
// and hands results to the UI thread through queued calls on receiver
std::thread worker;
std::atomic_bool cancel_requested{false};
QObject *receiver = nullptr;

QWidget *main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool load_check_at_startup()
{
	char *path = obs_module_config_path(CONFIG_FILE);
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	if (!data)
		return true;

	obs_data_set_default_bool(data, CHECK_AT_STARTUP_KEY, true);
	const bool enabled = obs_data_get_bool(data, CHECK_AT_STARTUP_KEY);
	obs_data_release(data);
	return enabled;
}

void save_check_at_startup(bool enabled)
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	char *path = obs_module_config_path(CONFIG_FILE);
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, CHECK_AT_STARTUP_KEY, enabled);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		obs_log(LOG_WARNING, "failed to save %s", CONFIG_FILE);
	obs_data_release(data);
	bfree(path);
}

size_t append_to_buffer(char *data, size_t size, size_t count, void *param)
{
	static_cast<QByteArray *>(param)->append(data, static_cast<qsizetype>(size * count));
	return size * count;
}

struct FileSink {
	QSaveFile *file;
	QCryptographicHash *hash;
};

size_t append_to_file(char *data, size_t size, size_t count, void *param)
{
	auto *sink = static_cast<FileSink *>(param);
	const qint64 length = static_cast<qint64>(size * count);
	sink->hash->addData(QByteArrayView(data, length));
	return sink->file->write(data, length) == length ? size * count : 0;
}

int abort_when_cancelled(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return cancel_requested ? 1 : 0;
}

// Runs one https GET and passes the body to write; false on any failure
bool fetch(const QString &url, const char *accept, curl_write_callback write, void *param)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_WARNING, "curl_easy_init failed");
		return false;
	}

	const QByteArray url_bytes = url.toUtf8();
	const QByteArray accept_header = QByteArray("Accept: ") + accept;
	const QByteArray user_agent = QString("%1/%2").arg(PLUGIN_NAME, PLUGIN_VERSION).toUtf8();
	curl_slist *headers = curl_slist_append(nullptr, accept_header.constData());

	curl_easy_setopt(curl, CURLOPT_URL, url_bytes.constData());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.constData());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME_SECONDS);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_cancelled);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, param);
#ifdef _WIN32
	// An unreachable revocation server does not fail the request
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif

	const CURLcode code = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (code != CURLE_OK) {
		if (code != CURLE_ABORTED_BY_CALLBACK)
			obs_log(LOG_WARNING, "request to %s failed: %s", url_bytes.constData(),
				curl_easy_strerror(code));
		return false;
	}
	return true;
}

QString file_sha256(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file))
		return {};
	return QString::fromLatin1(hash.result().toHex());
}

template<typename F> void run_on_ui(F function)
{
	QMetaObject::invokeMethod(receiver, std::move(function), Qt::QueuedConnection);
}

// Must be called on the UI thread
void start_worker(std::function<void()> task)
{
	if (worker.joinable())
		worker.join();
	worker = std::thread(std::move(task));
}

// Shows a dialog with an accept button and a later button; on_accept runs on the UI thread
template<typename F> void ask(const QString &message, const char *accept_key, F on_accept)
{
	QWidget *parent = main_window();
	if (!parent)
		return;

	auto *box = new QMessageBox(parent);
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->setIcon(QMessageBox::Information);
	box->setWindowTitle(QString::fromUtf8(obs_module_name()));
	box->setText(message);

	QPushButton *accept = box->addButton(text(accept_key), QMessageBox::AcceptRole);
	box->addButton(text("UpdateCheck.Later"), QMessageBox::RejectRole);

	QObject::connect(box, &QMessageBox::buttonClicked, box, [accept, on_accept](QAbstractButton *button) {
		if (button == accept)
			on_accept();
	});
	box->open();
}

void show_downloaded(const QString &version, const QString &path)
{
	const QString target = CAN_START_INSTALLER ? path : QFileInfo(path).absolutePath();
	const QString message =
		CAN_START_INSTALLER
			? text("UpdateCheck.Downloaded").arg(version)
			: text("UpdateCheck.DownloadedPackage").arg(version, QDir::toNativeSeparators(path));
	const char *accept_key = CAN_START_INSTALLER ? "UpdateCheck.StartInstaller" : "UpdateCheck.OpenFolder";

	ask(message, accept_key, [target]() {
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
			obs_log(LOG_WARNING, "failed to open %s", qUtf8Printable(QFileInfo(target).fileName()));
	});
}

bool download(const mt2s::ReleaseAsset &asset, const QString &path)
{
	const QString name = QFileInfo(path).fileName();
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		obs_log(LOG_WARNING, "failed to create %s: %s", qUtf8Printable(name),
			qUtf8Printable(file.errorString()));
		return false;
	}

	obs_log(LOG_INFO, "downloading update as %s", qUtf8Printable(name));
	QCryptographicHash hash(QCryptographicHash::Sha256);
	FileSink sink{&file, &hash};
	if (!fetch(asset.url, "application/octet-stream", append_to_file, &sink)) {
		file.cancelWriting();
		return false;
	}

	const QString actual = QString::fromLatin1(hash.result().toHex());
	if (actual != asset.sha256) {
		file.cancelWriting();
		obs_log(LOG_WARNING, "discarded %s: sha256 %s does not match %s", qUtf8Printable(name),
			qUtf8Printable(actual), qUtf8Printable(asset.sha256));
		return false;
	}

	if (!file.commit()) {
		obs_log(LOG_WARNING, "failed to save %s: %s", qUtf8Printable(name), qUtf8Printable(file.errorString()));
		return false;
	}

	obs_log(LOG_INFO, "downloaded update as %s", qUtf8Printable(name));
	return true;
}

void show_available(const QString &version, const mt2s::ReleaseAsset &asset, const QString &path)
{
	ask(text("UpdateCheck.Available").arg(version), "UpdateCheck.Download", [version, asset, path]() {
		start_worker([version, asset, path]() {
			if (download(asset, path))
				run_on_ui([version, path]() { show_downloaded(version, path); });
		});
	});
}

void check_for_update(const QString &download_dir)
{
	QByteArray body;
	if (!fetch(LATEST_RELEASE_URL, "application/vnd.github+json", append_to_buffer, &body))
		return;

	const QJsonObject release = QJsonDocument::fromJson(body).object();
	const QString tag = release.value("tag_name").toString();

	const std::optional<mt2s::Version> latest = mt2s::parse_version(tag);
	const std::optional<mt2s::Version> current = mt2s::parse_version(QString::fromUtf8(PLUGIN_VERSION));
	if (!latest || !current) {
		obs_log(LOG_WARNING, "update check skipped: cannot compare '%s' with '%s'", qUtf8Printable(tag),
			PLUGIN_VERSION);
		return;
	}

	if (*latest <= *current) {
		obs_log(LOG_INFO, "no update (latest release %s)", qUtf8Printable(tag));
		return;
	}

	const std::optional<mt2s::ReleaseAsset> asset =
		mt2s::find_asset(release.value("assets").toArray(), QString::fromLatin1(ASSET_SUFFIX));
	if (!asset) {
		obs_log(LOG_WARNING, "release %s has no verifiable asset ending with %s", qUtf8Printable(tag),
			ASSET_SUFFIX);
		return;
	}

	const QString path = QDir(download_dir).filePath(QString("%1-%2%3").arg(PLUGIN_NAME, tag, ASSET_SUFFIX));
	if (QFileInfo::exists(path) && file_sha256(path) == asset->sha256) {
		obs_log(LOG_INFO, "update %s already downloaded as %s", qUtf8Printable(tag),
			qUtf8Printable(QFileInfo(path).fileName()));
		run_on_ui([tag, path]() { show_downloaded(tag, path); });
		return;
	}

	obs_log(LOG_INFO, "update %s is available", qUtf8Printable(tag));
	run_on_ui([tag, found = *asset, path]() { show_available(tag, found, path); });
}

void start_check()
{
	if (!ASSET_SUFFIX || receiver)
		return;

	QString download_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	if (download_dir.isEmpty())
		download_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

	receiver = new QObject();
	start_worker([download_dir]() { check_for_update(download_dir); });
}

void add_menu_item(bool checked)
{
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("UpdateCheck.Menu")));
	action->setCheckable(true);
	action->setChecked(checked);
	QObject::connect(action, &QAction::toggled, action, [](bool enabled) { save_check_at_startup(enabled); });
}

void on_frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	const bool enabled = load_check_at_startup();
	add_menu_item(enabled);
	if (enabled)
		start_check();
}

} // namespace

void mt2s_update_check_load(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void mt2s_update_check_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	cancel_requested = true;
	if (worker.joinable())
		worker.join();
	delete receiver;
	receiver = nullptr;
}
