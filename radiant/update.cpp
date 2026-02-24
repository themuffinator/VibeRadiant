/*
   Copyright (C) 2026
*/

#include "update.h"

#include "debugging/debugging.h"
#include "environment.h"
#include "gtkutil/messagebox.h"
#include "mainframe.h"
#include "preferences.h"
#include "preferencesystem.h"
#include "qe3.h"
#include "url.h"
#include "version.h"
#include "stringio.h"
#include "stream/stringstream.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVersionNumber>

#include <functional>
#include <utility>

#ifndef RADIANT_GITHUB_REPO
#define RADIANT_GITHUB_REPO "themuffinator/VibeRadiant"
#endif

namespace
{
constexpr int k_update_check_interval_seconds = 60 * 60 * 24;
constexpr int k_update_check_timeout_ms = 20000;

bool g_update_auto_check = true;
bool g_update_allow_prerelease = false;
int g_update_last_check = 0;

struct UpdateAsset
{
	QString platform;
	QString url;
	QString sha256;
	QString name;
	QString type;
	qint64 size = 0;
};

struct UpdateManifest
{
	QString version;
	QString notes;
	QString published_at;
	QMap<QString, UpdateAsset> assets;
};

struct ReleaseMetadata
{
	QString version;
	QString notes_url;
	QString manifest_url;
	bool prerelease = false;
};

QString github_repo(){
	return QString::fromLatin1( RADIANT_GITHUB_REPO );
}

QString releases_api_url(){
	return QStringLiteral( "https://api.github.com/repos/%1/releases" ).arg( github_repo() );
}

QString fallback_manifest_url(){
	return QString::fromLatin1( RADIANT_UPDATE_URL );
}

QString releases_url(){
	return QString::fromLatin1( RADIANT_RELEASES_URL );
}

QString current_version(){
	return QString::fromLatin1( RADIANT_VERSION_NUMBER );
}

QString normalized_tag_version( const QString& tag ){
	QString normalized = tag.trimmed();
	if ( normalized.startsWith( 'v', Qt::CaseInsensitive ) ) {
		normalized = normalized.mid( 1 );
	}
	return normalized;
}

QString platform_key(){
#if defined( WIN32 )
#if defined( _WIN64 )
	return "windows-x86_64";
#else
	return "windows-x86";
#endif
#elif defined( __linux__ ) || defined( __FreeBSD__ )
#if defined( __x86_64__ ) || defined( _M_X64 )
	return "linux-x86_64";
#elif defined( __aarch64__ )
	return "linux-arm64";
#else
	return "linux-unknown";
#endif
#elif defined( __APPLE__ )
	return "macos-unknown";
#else
	return "unknown";
#endif
}

bool is_prerelease_version( const QString& version ){
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
	qsizetype suffix_index = -1;
#else
	int suffix_index = -1;
#endif
	const QVersionNumber parsed = QVersionNumber::fromString( version, &suffix_index );
	if ( parsed.isNull() ) {
		return false;
	}
	return suffix_index >= 0 && suffix_index < version.size();
}

int compare_versions( const QString& current, const QString& latest ){
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
	qsizetype current_suffix_index = -1;
	qsizetype latest_suffix_index = -1;
#else
	int current_suffix_index = -1;
	int latest_suffix_index = -1;
#endif
	const QVersionNumber current_ver = QVersionNumber::fromString( current, &current_suffix_index );
	const QVersionNumber latest_ver = QVersionNumber::fromString( latest, &latest_suffix_index );
	const QString current_suffix = ( current_suffix_index >= 0 && current_suffix_index < current.size() )
		? current.mid( current_suffix_index )
		: QString();
	const QString latest_suffix = ( latest_suffix_index >= 0 && latest_suffix_index < latest.size() )
		? latest.mid( latest_suffix_index )
		: QString();
	const int base_compare = QVersionNumber::compare( current_ver, latest_ver );
	if ( base_compare != 0 ) {
		return base_compare;
	}
	if ( current_suffix.isEmpty() && !latest_suffix.isEmpty() ) {
		return 1;
	}
	if ( !current_suffix.isEmpty() && latest_suffix.isEmpty() ) {
		return -1;
	}
	return QString::compare( current_suffix, latest_suffix, Qt::CaseInsensitive );
}

QString escape_powershell_string( const QString& value ){
	QString escaped = value;
	escaped.replace( "'", "''" );
	return QString( "'" ) + escaped + "'";
}

QString sha256_file( const QString& path, QString& error ){
	QFile file( path );
	if ( !file.open( QIODevice::ReadOnly ) ) {
		error = QString( "Failed to open " ) + path;
		return QString();
	}
	QCryptographicHash hash( QCryptographicHash::Sha256 );
	while ( !file.atEnd() ) {
		hash.addData( file.read( 1 << 20 ) );
	}
	return QString::fromLatin1( hash.result().toHex() );
}

bool parse_manifest( const QByteArray& data, UpdateManifest& manifest, QString& error ){
	QJsonParseError parse_error{};
	const QJsonDocument doc = QJsonDocument::fromJson( data, &parse_error );
	if ( parse_error.error != QJsonParseError::NoError ) {
		error = QString( "Update manifest parse error: " ) + parse_error.errorString();
		return false;
	}
	if ( !doc.isObject() ) {
		error = "Update manifest is not a JSON object.";
		return false;
	}

	const QJsonObject root = doc.object();
	manifest.version = root.value( "version" ).toString();
	manifest.notes = root.value( "notes" ).toString();
	manifest.published_at = root.value( "published_at" ).toString();
	const QJsonObject assets = root.value( "assets" ).toObject();
	for ( auto it = assets.begin(); it != assets.end(); ++it ) {
		const QJsonObject asset_object = it.value().toObject();
		UpdateAsset asset;
		asset.platform = it.key();
		asset.url = asset_object.value( "url" ).toString();
		asset.sha256 = asset_object.value( "sha256" ).toString();
		asset.name = asset_object.value( "name" ).toString();
		asset.type = asset_object.value( "type" ).toString();
		asset.size = static_cast<qint64>( asset_object.value( "size" ).toDouble( 0 ) );
		if ( !asset.url.isEmpty() ) {
			manifest.assets.insert( asset.platform, asset );
		}
	}

	if ( manifest.version.isEmpty() ) {
		error = "Update manifest missing version.";
		return false;
	}
	if ( manifest.assets.isEmpty() ) {
		error = "Update manifest contains no assets.";
		return false;
	}
	return true;
}

bool parse_release_object( const QJsonObject& object, bool allow_prerelease, ReleaseMetadata& release ){
	if ( object.value( "draft" ).toBool() ) {
		return false;
	}

	release.prerelease = object.value( "prerelease" ).toBool();
	if ( !allow_prerelease && release.prerelease ) {
		return false;
	}

	release.version = normalized_tag_version( object.value( "tag_name" ).toString() );
	release.notes_url = object.value( "html_url" ).toString();
	release.manifest_url.clear();

	const QJsonArray assets = object.value( "assets" ).toArray();
	for ( const QJsonValue& value : assets ) {
		const QJsonObject asset = value.toObject();
		const QString name = asset.value( "name" ).toString();
		if ( QString::compare( name, "update.json", Qt::CaseInsensitive ) != 0 ) {
			continue;
		}
		release.manifest_url = asset.value( "browser_download_url" ).toString();
		if ( !release.manifest_url.isEmpty() ) {
			break;
		}
	}

	return !release.manifest_url.isEmpty();
}

bool parse_release_payload( const QByteArray& payload, bool allow_prerelease, ReleaseMetadata& release, QString& error ){
	QJsonParseError parse_error{};
	const QJsonDocument doc = QJsonDocument::fromJson( payload, &parse_error );
	if ( parse_error.error != QJsonParseError::NoError ) {
		error = QStringLiteral( "Update release metadata parse error: %1" ).arg( parse_error.errorString() );
		return false;
	}

	if ( doc.isObject() ) {
		if ( parse_release_object( doc.object(), allow_prerelease, release ) ) {
			return true;
		}
		error = QStringLiteral( "No release metadata with update.json was found." );
		return false;
	}

	if ( doc.isArray() ) {
		const QJsonArray array = doc.array();
		for ( const QJsonValue& value : array ) {
			if ( !value.isObject() ) {
				continue;
			}
			if ( parse_release_object( value.toObject(), allow_prerelease, release ) ) {
				return true;
			}
		}
		error = QStringLiteral( "No matching release with update.json was found." );
		return false;
	}

	error = QStringLiteral( "Update release metadata response was not valid JSON." );
	return false;
}

void configure_update_request( QNetworkRequest& request ){
	request.setHeader( QNetworkRequest::UserAgentHeader, QStringLiteral( "VibeRadiant-Updater/%1" ).arg( current_version() ) );
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
	request.setTransferTimeout( k_update_check_timeout_ms );
#endif
}

void Update_constructPreferences( PreferencesPage& page ){
	page.appendCheckBox( "Updates", "Check for updates at startup", g_update_auto_check );
	page.appendCheckBox( "", "Include prerelease builds", g_update_allow_prerelease );
}

class UpdateManager final : public QObject
{
public:
	~UpdateManager() override{
		cancel_reply();
	}

	void construct(){
		if ( m_constructed ) {
			return;
		}
		m_constructed = true;

		PreferencesDialog_addSettingsPreferences( makeCallbackF( Update_constructPreferences ) );
		GlobalPreferenceSystem().registerPreference( "UpdateAutoCheck", BoolImportStringCaller( g_update_auto_check ), BoolExportStringCaller( g_update_auto_check ) );
		GlobalPreferenceSystem().registerPreference( "UpdateAllowPrerelease", BoolImportStringCaller( g_update_allow_prerelease ), BoolExportStringCaller( g_update_allow_prerelease ) );
		GlobalPreferenceSystem().registerPreference( "UpdateLastCheck", IntImportStringCaller( g_update_last_check ), IntExportStringCaller( g_update_last_check ) );
	}

	void destroy(){
		cancel_reply();
	}

	void maybeAutoCheck(){
		if ( !g_update_auto_check ) {
			return;
		}
		const qint64 now = QDateTime::currentSecsSinceEpoch();
		if ( g_update_last_check > 0 && now - g_update_last_check < k_update_check_interval_seconds ) {
			return;
		}
		QTimer::singleShot( 1500, this, [this](){ checkForUpdates( UpdateCheckMode::Automatic ); } );
	}

	void checkForUpdates( UpdateCheckMode mode ){
		checkForUpdatesInternal( mode, nullptr, nullptr );
	}

	void checkForUpdatesBlocking( UpdateCheckMode mode, QWidget* parent_override ){
		bool finished = false;
		QEventLoop loop;
		checkForUpdatesInternal( mode, parent_override, [&](){
			finished = true;
			loop.quit();
		} );
		if ( !finished ) {
			loop.exec();
		}
	}

	bool quitRequested() const {
		return m_quit_requested;
	}

private:
	QNetworkAccessManager *m_network = nullptr;
	QPointer<QProgressDialog> m_check_dialog;
	QPointer<QProgressDialog> m_download_dialog;
	QNetworkReply *m_reply = nullptr;
	QFile m_download_file;
	UpdateCheckMode m_mode = UpdateCheckMode::Automatic;
	bool m_check_in_progress = false;
	bool m_download_in_progress = false;
	QString m_download_path;
	QString m_download_dir;
	bool m_constructed = false;
	bool m_quit_requested = false;
	bool m_tried_fallback_manifest = false;
	QString m_release_notes_url;
	QPointer<QWidget> m_parent_override;
	std::function<void()> m_check_finished_callback;

	void ensure_network(){
		if ( !m_network ) {
			m_network = new QNetworkAccessManager( this );
		}
	}

	QWidget* parentWindow() const {
		return m_parent_override ? m_parent_override.data() : MainFrame_getWindow();
	}

	void finish_check(){
		if ( m_check_dialog ) {
			m_check_dialog->close();
			m_check_dialog = nullptr;
		}
		m_check_in_progress = false;
		auto callback = std::move( m_check_finished_callback );
		if ( callback ) {
			callback();
		}
	}

	void checkForUpdatesInternal( UpdateCheckMode mode, QWidget* parent_override, std::function<void()> finished ){
		if ( m_check_in_progress || m_download_in_progress ) {
			if ( finished ) {
				finished();
			}
			return;
		}
		if ( mode == UpdateCheckMode::Automatic && !g_update_auto_check ) {
			if ( finished ) {
				finished();
			}
			return;
		}

		const qint64 now = QDateTime::currentSecsSinceEpoch();
		if ( mode == UpdateCheckMode::Automatic && g_update_last_check > 0 &&
		     now - g_update_last_check < k_update_check_interval_seconds ) {
			if ( finished ) {
				finished();
			}
			return;
		}

		ensure_network();

		g_update_last_check = static_cast<int>( now );
		m_mode = mode;
		m_parent_override = parent_override;
		m_check_finished_callback = std::move( finished );
		m_release_notes_url.clear();
		m_tried_fallback_manifest = false;
		m_quit_requested = false;
		m_check_in_progress = true;

		if ( m_mode == UpdateCheckMode::Manual ) {
			m_check_dialog = new QProgressDialog( "Checking for updates...", "Cancel", 0, 0, parentWindow() );
			m_check_dialog->setWindowModality( Qt::WindowModal );
			m_check_dialog->setMinimumDuration( 0 );
			connect( m_check_dialog, &QProgressDialog::canceled, this, [this](){
				if ( m_reply ) {
					m_reply->abort();
				}
			} );
		}

		start_release_lookup();
	}

	void start_release_lookup(){
		const bool allow_prerelease = g_update_allow_prerelease;
		QUrl url;
		if ( allow_prerelease ) {
			url = QUrl( releases_api_url() );
		}
		else{
			url = QUrl( releases_api_url() + "/latest" );
		}

		QNetworkRequest request( url );
		request.setRawHeader( "Accept", "application/vnd.github+json" );
		request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork );
		configure_update_request( request );

		m_reply = m_network->get( request );
		connect( m_reply, &QNetworkReply::finished, this, [this](){ handle_release_finished(); } );
	}

	void start_manifest_request( const QString& manifest_url ){
		if ( manifest_url.isEmpty() ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				qt_MessageBox( parentWindow(), "Release metadata is missing update.json.", "Update", EMessageBoxType::Error );
			}
			finish_check();
			return;
		}

		QUrl url( manifest_url );
		QUrlQuery query( url );
		query.addQueryItem( "ts", QString::number( QDateTime::currentSecsSinceEpoch() ) );
		url.setQuery( query );

		QNetworkRequest request( url );
		request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork );
		configure_update_request( request );

		m_reply = m_network->get( request );
		connect( m_reply, &QNetworkReply::finished, this, [this](){ handle_manifest_finished(); } );
	}

	void handle_release_finished(){
		if ( !m_reply ) {
			finish_check();
			return;
		}

		const QNetworkReply::NetworkError net_error = m_reply->error();
		const QString error_string = m_reply->errorString();
		const QByteArray payload = m_reply->readAll();
		m_reply->deleteLater();
		m_reply = nullptr;

		if ( net_error == QNetworkReply::OperationCanceledError ) {
			finish_check();
			return;
		}

		if ( net_error != QNetworkReply::NoError ) {
			if ( !m_tried_fallback_manifest ) {
				m_tried_fallback_manifest = true;
				start_manifest_request( fallback_manifest_url() );
				return;
			}
			if ( m_mode == UpdateCheckMode::Manual ) {
				const auto msg = QString( "Update check failed: " ) + error_string;
				qt_MessageBox( parentWindow(), msg.toLatin1().constData(), "Update", EMessageBoxType::Error );
			}
			finish_check();
			return;
		}

		ReleaseMetadata release;
		QString error;
		if ( !parse_release_payload( payload, g_update_allow_prerelease, release, error ) ) {
			if ( !m_tried_fallback_manifest ) {
				m_tried_fallback_manifest = true;
				start_manifest_request( fallback_manifest_url() );
				return;
			}
			if ( m_mode == UpdateCheckMode::Manual ) {
				qt_MessageBox( parentWindow(), error.toLatin1().constData(), "Update", EMessageBoxType::Info );
			}
			finish_check();
			return;
		}

		m_release_notes_url = release.notes_url;
		start_manifest_request( release.manifest_url );
	}

	void handle_manifest_finished(){
		if ( !m_reply ) {
			finish_check();
			return;
		}
		if ( m_check_dialog ) {
			m_check_dialog->close();
			m_check_dialog = nullptr;
		}

		const QNetworkReply::NetworkError net_error = m_reply->error();
		const QString error_string = m_reply->errorString();
		const QByteArray payload = m_reply->readAll();
		m_reply->deleteLater();
		m_reply = nullptr;

		if ( net_error == QNetworkReply::OperationCanceledError ) {
			finish_check();
			return;
		}
		if ( net_error != QNetworkReply::NoError ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				const auto msg = QString( "Update check failed: " ) + error_string;
				qt_MessageBox( parentWindow(), msg.toLatin1().constData(), "Update", EMessageBoxType::Error );
			}
			finish_check();
			return;
		}

		QString error;
		UpdateManifest manifest;
		if ( !parse_manifest( payload, manifest, error ) ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				qt_MessageBox( parentWindow(), error.toLatin1().constData(), "Update", EMessageBoxType::Error );
			}
			finish_check();
			return;
		}
		if ( manifest.notes.isEmpty() && !m_release_notes_url.isEmpty() ) {
			manifest.notes = m_release_notes_url;
		}

		if ( !g_update_allow_prerelease && is_prerelease_version( manifest.version ) ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				const auto msg = StringStream( "Prerelease ", manifest.version.toLatin1().constData(), " is available.\nEnable prerelease updates to download it." );
				qt_MessageBox( parentWindow(), msg, "Update", EMessageBoxType::Info );
			}
			finish_check();
			return;
		}

		const QString platform = platform_key();
		if ( !manifest.assets.contains( platform ) ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				const auto msg = StringStream( "No update package found for platform ", platform.toLatin1().constData(), "." );
				qt_MessageBox( parentWindow(), msg, "Update", EMessageBoxType::Info );
			}
			finish_check();
			return;
		}

		const int cmp = compare_versions( current_version(), manifest.version );
		if ( cmp >= 0 ) {
			if ( m_mode == UpdateCheckMode::Manual ) {
				const auto msg = StringStream( "You are up to date (", current_version().toLatin1().constData(), ")." );
				qt_MessageBox( parentWindow(), msg, "Update", EMessageBoxType::Info );
			}
			finish_check();
			return;
		}

		const UpdateAsset asset = manifest.assets.value( platform );
		prompt_update( manifest, asset );
		finish_check();
	}

	void prompt_update( const UpdateManifest& manifest, const UpdateAsset& asset ){
		QWidget* parent = parentWindow();
		const bool splash_parent = parent && parent->windowFlags().testFlag( Qt::SplashScreen );
		QMessageBox dialog( splash_parent ? nullptr : parent );
		dialog.setWindowTitle( "VibeRadiant Update" );
		dialog.setText( QStringLiteral( "VibeRadiant %1 is available." ).arg( manifest.version ) );
		dialog.setInformativeText( QStringLiteral( "Current version: %1\nLatest version: %2" ).arg( current_version(), manifest.version ) );
		if ( splash_parent ) {
			dialog.setWindowFlag( Qt::WindowStaysOnTopHint, true );
		}

		QAbstractButton* download_button = dialog.addButton( "Download and Install", QMessageBox::AcceptRole );
		QAbstractButton* release_button = dialog.addButton( "View Release", QMessageBox::ActionRole );
		dialog.addButton( "Later", QMessageBox::RejectRole );
		dialog.exec();

		if ( dialog.clickedButton() == download_button ) {
			start_download( manifest, asset );
		}
		else if ( dialog.clickedButton() == release_button ) {
			if ( !manifest.notes.isEmpty() ) {
				OpenURL( manifest.notes.toLatin1().constData() );
			}
			else{
				OpenURL( releases_url().toLatin1().constData() );
			}
		}
	}

	void start_download( const UpdateManifest& manifest, const UpdateAsset& asset ){
		Q_UNUSED( manifest );

		const QString temp_root = QStandardPaths::writableLocation( QStandardPaths::TempLocation );
		if ( temp_root.isEmpty() ) {
			qt_MessageBox( parentWindow(), "No writable temp directory available.", "Update", EMessageBoxType::Error );
			return;
		}

		m_download_dir = QDir( temp_root ).filePath(
			QStringLiteral( "viberadiant-update-%1" ).arg( QDateTime::currentMSecsSinceEpoch() ) );
		QDir().mkpath( m_download_dir );

		m_download_path = QDir( m_download_dir ).filePath( asset.name.isEmpty() ? "update.bin" : asset.name );
		m_download_file.setFileName( m_download_path );
		if ( !m_download_file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
			qt_MessageBox( parentWindow(), "Failed to open download file.", "Update", EMessageBoxType::Error );
			return;
		}

		QNetworkRequest request( QUrl( asset.url ) );
		configure_update_request( request );

		m_download_in_progress = true;

		m_download_dialog = new QProgressDialog( "Downloading update...", "Cancel", 0, 100, parentWindow() );
		m_download_dialog->setWindowModality( Qt::WindowModal );
		m_download_dialog->setMinimumDuration( 0 );
		m_download_dialog->setValue( 0 );
		connect( m_download_dialog, &QProgressDialog::canceled, this, [this](){
			if ( m_reply ) {
				m_reply->abort();
			}
		} );

		ensure_network();
		m_reply = m_network->get( request );
		connect( m_reply, &QNetworkReply::readyRead, this, [this](){
			if ( m_reply ) {
				m_download_file.write( m_reply->readAll() );
			}
		} );
		connect( m_reply, &QNetworkReply::downloadProgress, this, [this]( qint64 received, qint64 total ){
			if ( m_download_dialog ) {
				if ( total > 0 ) {
					m_download_dialog->setValue( static_cast<int>( ( received * 100 ) / total ) );
				}
				else{
					m_download_dialog->setRange( 0, 0 );
				}
			}
		} );
		connect( m_reply, &QNetworkReply::finished, this, [this, asset](){ handle_download_finished( asset ); } );
	}

	void handle_download_finished( const UpdateAsset& asset ){
		if ( m_download_dialog ) {
			m_download_dialog->close();
			m_download_dialog = nullptr;
		}

		m_download_in_progress = false;

		if ( !m_reply ) {
			return;
		}

		const QNetworkReply::NetworkError net_error = m_reply->error();
		m_reply->deleteLater();
		m_reply = nullptr;
		m_download_file.flush();
		m_download_file.close();

		if ( net_error == QNetworkReply::OperationCanceledError ) {
			QFile::remove( m_download_path );
			return;
		}
		if ( net_error != QNetworkReply::NoError ) {
			QFile::remove( m_download_path );
			qt_MessageBox( parentWindow(), "Update download failed.", "Update", EMessageBoxType::Error );
			return;
		}

		QString error;
		if ( !asset.sha256.isEmpty() ) {
			const QString hash = sha256_file( m_download_path, error );
			if ( hash.isEmpty() || QString::compare( hash, asset.sha256, Qt::CaseInsensitive ) != 0 ) {
				QFile::remove( m_download_path );
				qt_MessageBox( parentWindow(), "Update verification failed.", "Update", EMessageBoxType::Error );
				return;
			}
		}

		if ( !install_update( asset, m_download_path ) ) {
			return;
		}
	}

	bool install_update( const UpdateAsset& asset, const QString& path ){
		if ( !ConfirmModified( "Install Update" ) ) {
			return false;
		}

#if defined( WIN32 )
		Q_UNUSED( asset );
		return install_update_windows( path );
#elif defined( __linux__ ) || defined( __FreeBSD__ )
		Q_UNUSED( asset );
		return install_update_linux( path );
#else
		Q_UNUSED( asset );
		Q_UNUSED( path );
		qt_MessageBox( parentWindow(), "Auto-update is not supported on this platform.", "Update", EMessageBoxType::Info );
		return false;
#endif
	}

	bool install_update_windows( const QString& path ){
		const QString install_dir = QDir::toNativeSeparators( QString::fromLatin1( AppPath_get() ) );
		const QString exe_path = QDir::toNativeSeparators( QString::fromLatin1( environment_get_app_filepath() ) );

		QString error;
		if ( !ensure_writable_directory( install_dir, error ) ) {
			qt_MessageBox( parentWindow(), error.toLatin1().constData(), "Update", EMessageBoxType::Error );
			return false;
		}

		const QString script_path = QDir( m_download_dir ).filePath( "apply-update.ps1" );
		const auto pid = QString::number( QCoreApplication::applicationPid() );
		const auto script = QString(
			"$ErrorActionPreference = 'Stop'\n"
			"$pid = %1\n"
			"while (Get-Process -Id $pid -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }\n"
			"Expand-Archive -Path %2 -DestinationPath %3 -Force\n"
			"Start-Process %4\n"
		).arg( pid,
		      escape_powershell_string( QDir::toNativeSeparators( path ) ),
		      escape_powershell_string( install_dir ),
		      escape_powershell_string( exe_path ) );

		QFile script_file( script_path );
		if ( !script_file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
			qt_MessageBox( parentWindow(), "Failed to write update script.", "Update", EMessageBoxType::Error );
			return false;
		}
		script_file.write( script.toLatin1() );
		script_file.close();

		if ( !QProcess::startDetached( "powershell", { "-ExecutionPolicy", "Bypass", "-File", script_path } ) ) {
			qt_MessageBox( parentWindow(), "Failed to launch updater.", "Update", EMessageBoxType::Error );
			return false;
		}

		m_quit_requested = true;
		QCoreApplication::quit();
		return true;
	}

	bool install_update_linux( const QString& path ){
		const QByteArray appimage_env = qgetenv( "APPIMAGE" );
		if ( appimage_env.isEmpty() ) {
			qt_MessageBox( parentWindow(), "Auto-update requires the AppImage build.", "Update", EMessageBoxType::Info );
			return false;
		}

		const QString appimage_path = QString::fromUtf8( appimage_env );
		QString error;
		if ( !ensure_writable_directory( QFileInfo( appimage_path ).absolutePath(), error ) ) {
			qt_MessageBox( parentWindow(), error.toLatin1().constData(), "Update", EMessageBoxType::Error );
			return false;
		}

		const QString script_path = QDir( m_download_dir ).filePath( "apply-update.sh" );
		const auto pid = QString::number( QCoreApplication::applicationPid() );
		const auto script = QString(
			"#!/bin/sh\n"
			"set -e\n"
			"pid=%1\n"
			"while kill -0 $pid 2>/dev/null; do sleep 0.2; done\n"
			"chmod +x %2\n"
			"mv %2 %3\n"
			"%3 &\n"
		).arg( pid,
		      QDir::toNativeSeparators( path ),
		      QDir::toNativeSeparators( appimage_path ) );

		QFile script_file( script_path );
		if ( !script_file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
			qt_MessageBox( parentWindow(), "Failed to write update script.", "Update", EMessageBoxType::Error );
			return false;
		}
		script_file.write( script.toLatin1() );
		script_file.close();

		QFile::setPermissions( script_path, QFile::permissions( script_path ) | QFileDevice::ExeUser );

		if ( !QProcess::startDetached( "/bin/sh", { script_path } ) ) {
			qt_MessageBox( parentWindow(), "Failed to launch updater.", "Update", EMessageBoxType::Error );
			return false;
		}

		m_quit_requested = true;
		QCoreApplication::quit();
		return true;
	}

	bool ensure_writable_directory( const QString& dir, QString& error ) const {
		QDir target( dir );
		if ( !target.exists() ) {
			error = StringStream( "Update directory does not exist: ", dir.toLatin1().constData() ).c_str();
			return false;
		}

		const QString test_path = target.filePath( ".update_write_test" );
		QFile test_file( test_path );
		if ( !test_file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
			error = StringStream( "Update directory is not writable: ", dir.toLatin1().constData() ).c_str();
			return false;
		}
		test_file.close();
		test_file.remove();
		return true;
	}

	void cancel_reply(){
		if ( m_reply ) {
			m_reply->abort();
			m_reply->deleteLater();
			m_reply = nullptr;
		}
		if ( m_download_file.isOpen() ) {
			m_download_file.close();
		}
		m_check_in_progress = false;
		m_download_in_progress = false;
		m_parent_override = nullptr;
		m_check_finished_callback = nullptr;
	}
};

UpdateManager *g_update_manager = nullptr;
} // namespace

void UpdateManager_Construct(){
	if ( !g_update_manager ) {
		g_update_manager = new UpdateManager();
	}
	g_update_manager->construct();
}

void UpdateManager_Destroy(){
	delete g_update_manager;
	g_update_manager = nullptr;
}

void UpdateManager_MaybeAutoCheck(){
	if ( g_update_manager ) {
		g_update_manager->maybeAutoCheck();
	}
}

void UpdateManager_CheckForUpdates( UpdateCheckMode mode ){
	if ( g_update_manager ) {
		g_update_manager->checkForUpdates( mode );
	}
}

void UpdateManager_CheckForUpdatesBlocking( UpdateCheckMode mode, QWidget* parent_override ){
	if ( g_update_manager ) {
		g_update_manager->checkForUpdatesBlocking( mode, parent_override );
	}
}

bool UpdateManager_QuitRequested(){
	return g_update_manager && g_update_manager->quitRequested();
}
