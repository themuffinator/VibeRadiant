/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

//
// User preferences
//
// Leonardo Zide (leo@lokigames.com)
//

#include "preferences.h"
#include "environment.h"

#include "debugging/debugging.h"

#include "generic/callback.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "os/file.h"
#include "os/path.h"
#include "os/dir.h"
#include "gtkutil/messagebox.h"
#include "commandlib.h"
#include "gtkutil/i18n.h"
#include "localization.h"

#include "error.h"
#include "xywindow.h"
#include "mainframe.h"
#include "gtkdlgs.h"
#include "theme.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QGroupBox>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QRadioButton>
#include <QAbstractItemModel>
#include <QItemSelectionModel>
#include <QStringList>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFrame>
#include <QPainter>
#include <QTimer>
#include <QEventLoop>
#include <QSignalBlocker>
#include <QUuid>
#include <QStyle>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include <vector>
#include <utility>


void Global_constructPreferences( PreferencesPage& page, bool applyImmediately ){
	Localization_constructPreferences( page, applyImmediately );
}

void Interface_constructPreferences( PreferencesPage& page ){
	page.appendPathEntry( "Shader Editor Command", g_TextEditor_editorCommand, false );
}

/*!
   =========================================================
   Games selection dialog
   =========================================================
 */

inline const char* xmlAttr_getName( xmlAttrPtr attr ){
	return reinterpret_cast<const char*>( attr->name );
}

inline const char* xmlAttr_getValue( xmlAttrPtr attr ){
	return reinterpret_cast<const char*>( attr->children->content );
}

CopiedString CGameDescription::normaliseKey( const char* key ){
	if ( key == nullptr ) {
		return "";
	}

	StringOutputStream token( 64 );
	for ( const unsigned char c : std::string_view( key ) )
	{
		if ( std::isalnum( c ) ) {
			token << static_cast<char>( std::tolower( c ) );
		}
	}
	return token.c_str();
}

const char* CGameDescription::getKeyValue( const char* key ) const {
	if ( key == nullptr || string_empty( key ) ) {
		return "";
	}

	if ( GameDescription::const_iterator i = m_gameDescription.find( key ); i != m_gameDescription.end() ) {
		return ( *i ).second.c_str();
	}

	const CopiedString normalised = normaliseKey( key );
	if ( !normalised.empty() ) {
		if ( GameDescription::const_iterator i = m_gameDescriptionNormalised.find( normalised ); i != m_gameDescriptionNormalised.end() ) {
			return ( *i ).second.c_str();
		}
	}

	return "";
}

const char* CGameDescription::getRequiredKeyValue( const char* key ) const {
	if ( key != nullptr && !string_empty( key ) ) {
		if ( GameDescription::const_iterator i = m_gameDescription.find( key ); i != m_gameDescription.end() ) {
			return ( *i ).second.c_str();
		}

		const CopiedString normalised = normaliseKey( key );
		if ( !normalised.empty() ) {
			if ( GameDescription::const_iterator i = m_gameDescriptionNormalised.find( normalised ); i != m_gameDescriptionNormalised.end() ) {
				return ( *i ).second.c_str();
			}
		}
	}
	ERROR_MESSAGE( "game attribute " << Quoted( key ) << " not found in " << Quoted( mGameFile ) );
	return "";
}

CGameDescription::CGameDescription( xmlDocPtr pDoc, const CopiedString& gameFile ){
	// read the user-friendly game name
	xmlNodePtr pNode = pDoc->children;

	while ( pNode != 0 && strcmp( (const char*)pNode->name, "game" ) )
	{
		pNode = pNode->next;
	}
	if ( !pNode ) {
		Error( "Didn't find 'game' node in the game description file '%s'\n", pDoc->URL );
	}

	for ( xmlAttrPtr attr = pNode->properties; attr != 0; attr = attr->next )
	{
		const CopiedString attrName = xmlAttr_getName( attr );
		const CopiedString attrValue = xmlAttr_getValue( attr );

		m_gameDescription.insert( GameDescription::value_type( attrName, attrValue ) );

		const CopiedString normalised = normaliseKey( attrName.c_str() );
		if ( !normalised.empty() ) {
			m_gameDescriptionNormalised.insert( GameDescription::value_type( normalised, attrValue ) );
		}
	}

	mGameToolsPath = StringStream( AppPath_get(), "gamepacks/", gameFile, '/' );

	ASSERT_MESSAGE( file_exists( mGameToolsPath.c_str() ), "game directory not found: " << Quoted( mGameToolsPath ) );

	mGameFile = gameFile;

	{
		const char* type = getKeyValue( "type" );
		if ( string_empty( type ) ) {
			globalWarningStream() << "Warning, 'type' attribute not found in " << SingleQuoted( reinterpret_cast<const char*>( pDoc->URL ) ) << '\n';
			// default
			mGameType = "q3";
		}
		else
		{
			mGameType = type;
		}
	}
}

void CGameDescription::Dump(){
	globalOutputStream() << "game description file: " << Quoted( mGameFile ) << '\n';
	for ( const auto& [ key, value ] : m_gameDescription )
	{
		globalOutputStream() << key << " = " << Quoted( value ) << '\n';
	}
}

CGameDescription *g_pGameDescription;


#include "stream/textfilestream.h"
#include "xml/xmlparser.h"
#include "xml/xmlwriter.h"

#include "preferencedictionary.h"
#include "stringio.h"

constexpr const char* PREFERENCES_VERSION = "1.0";

bool Preferences_Load( PreferenceDictionary& preferences, const char* filename, const char *cmdline_prefix ){
	bool ret = false;
	TextFileInputStream file( filename );
	if ( !file.failed() ) {
		XMLStreamParser parser( file );
		XMLPreferenceDictionaryImporter importer( preferences, PREFERENCES_VERSION );
		parser.exportXML( importer );
		ret = true;
	}
	// set config settings from the command line, e.g.   -global-gamefile q4.game   -q4.game-CamHeight 944
	const size_t len = strlen( cmdline_prefix );
	for ( int i = 1; i < g_argc - 1; ++i )
	{
		if ( g_argv[i][0] == '-' ) {
			if ( strncmp( g_argv[i] + 1, cmdline_prefix, len ) == 0 ) {
				if ( g_argv[i][len + 1] == '-' ) {
					preferences.importPref( g_argv[i] + len + 2, g_argv[i + 1] );
				}
			}
			++i;
		}
	}

	return ret;
}

bool Preferences_Save( PreferenceDictionary& preferences, const char* filename ){
	TextFileOutputStream file( filename );
	if ( !file.failed() ) {
		XMLStreamWriter writer( file );
		XMLPreferenceDictionaryExporter exporter( preferences, PREFERENCES_VERSION );
		exporter.exportXML( writer );
		return true;
	}
	return false;
}

bool Preferences_Save_Safe( PreferenceDictionary& preferences, const char* filename ){
	const auto tmpName = StringStream( filename, "TMP" );
	return Preferences_Save( preferences, tmpName ) && file_move( tmpName, filename );
}


void RegisterGlobalPreferences( PreferenceSystem& preferences ){
	preferences.registerPreference( "gamefile", makeCopiedStringStringImportCallback( LatchedAssignCaller( g_GamesDialog.m_sGameFile ) ), CopiedStringExportStringCaller( g_GamesDialog.m_sGameFile.m_latched ) );
	preferences.registerPreference( "gamePrompt", BoolImportStringCaller( g_GamesDialog.m_bGamePrompt ), BoolExportStringCaller( g_GamesDialog.m_bGamePrompt ) );
	theme_registerGlobalPreference( preferences );
	Localization_registerGlobalPreference( preferences );
}


PreferenceDictionary g_global_preferences;

void GlobalPreferences_Init(){
	RegisterGlobalPreferences( g_global_preferences );
}

constexpr const char* PREFS_GLOBAL_FILENAME = "global.pref";

namespace
{
constexpr const char* GAME_INSTALL_STATE_FILENAME = "game_installations.json";

struct GameInstallationEntry
{
	QString id;
	QString gameFile;
	QString name;
	QString path;
	QString engineExecutable;
};

struct GameInstallationState
{
	QVector<GameInstallationEntry> installations;
	QString selectedGameFile;
	QHash<QString, QString> selectedInstallByGame;
	QHash<QString, QString> selectedGameNameByInstall;
};

CopiedString g_startupGameInstallationPath;
CopiedString g_startupGameInstallationEngineExecutable;
CopiedString g_startupGameInstallationId;
CopiedString g_startupGameInstallationGameName;

struct SupportedGameModEntry
{
	QString dir;
	QString label;
};

QString normalise_game_name_token( const QString& value ){
	const QString trimmed = value.trimmed();
	if ( trimmed.isEmpty() ) {
		return {};
	}
	return QDir::cleanPath( trimmed );
}

QStringList descriptor_split_list( const char* value, const char* delimiters ){
	QStringList out;
	QString token;
	for ( const char c : std::string_view( value == nullptr ? "" : value ) )
	{
		if ( strchr( delimiters, c ) != nullptr ) {
			token = token.trimmed();
			if ( !token.isEmpty() ) {
				out.push_back( token );
			}
			token.clear();
			continue;
		}
		token += QLatin1Char( c );
	}
	token = token.trimmed();
	if ( !token.isEmpty() ) {
		out.push_back( token );
	}
	return out;
}

void append_supported_game_mod( QVector<SupportedGameModEntry>& mods, const QString& dir, const QString& label ){
	const QString cleanDir = normalise_game_name_token( dir );
	if ( cleanDir.isEmpty() ) {
		return;
	}

	for ( SupportedGameModEntry& existing : mods )
	{
		if ( existing.dir.compare( cleanDir, Qt::CaseInsensitive ) == 0 ) {
			if ( existing.label.trimmed().isEmpty() && !label.trimmed().isEmpty() ) {
				existing.label = label.trimmed();
			}
			return;
		}
	}

	SupportedGameModEntry entry;
	entry.dir = cleanDir;
	entry.label = label.trimmed().isEmpty() ? cleanDir : label.trimmed();
	mods.push_back( entry );
}

QString installation_game_directory_path( const QString& installPath, const QString& dirName ){
	const QString trimmedRootPath = installPath.trimmed();
	const QString rootPath = trimmedRootPath.isEmpty() ? QString() : QDir::cleanPath( trimmedRootPath );
	const QString cleanDir = normalise_game_name_token( dirName );
	if ( rootPath.isEmpty() || cleanDir.isEmpty() ) {
		return {};
	}

	const QDir rootDir( rootPath );
	if ( rootDir.dirName().compare( cleanDir, Qt::CaseInsensitive ) == 0 ) {
		return rootDir.absolutePath();
	}
	return QDir::cleanPath( rootDir.filePath( cleanDir ) );
}

bool installation_game_directory_exists( const QString& installPath, const QString& dirName ){
	const QFileInfo dirInfo( installation_game_directory_path( installPath, dirName ) );
	return dirInfo.exists() && dirInfo.isDir();
}

bool installation_game_directory_looks_like_mod( const CGameDescription& game, const QString& installPath, const QString& dirName ){
	const QString gamePath = installation_game_directory_path( installPath, dirName );
	if ( gamePath.isEmpty() ) {
		return false;
	}

	const QFileInfo dirInfo( gamePath );
	if ( !dirInfo.exists() || !dirInfo.isDir() ) {
		return false;
	}

	const QString cleanDir = normalise_game_name_token( dirName );
	const QString baseGame = QString::fromUtf8( game.getRequiredKeyValue( "basegame" ) ).trimmed();
	if ( cleanDir.compare( baseGame, Qt::CaseInsensitive ) == 0 ) {
		return true;
	}

	const QDir dir( gamePath );
	for ( const char* hint : { "maps", "scripts", "materials", "textures", "models", "sound", "shaders", "progs" } )
	{
		if ( QFileInfo( dir.filePath( hint ) ).isDir() ) {
			return true;
		}
	}
	for ( const char* fileHint : { "modinfo.txt", "description.txt", "gamex86.dll", "gamex64.dll", "game_x64.dll", "pak0.pk3", "pak0.pak", "pak0.pk4", "default.cfg", "progs.dat" } )
	{
		if ( QFileInfo( dir.filePath( fileHint ) ).exists() ) {
			return true;
		}
	}

	QStringList archiveTypes = descriptor_split_list( game.getKeyValue( "archivetypes" ), " ,;|\t\r\n" );
	for ( QString& archiveType : archiveTypes )
	{
		archiveType = archiveType.trimmed().toLower();
	}

	const QFileInfoList files = dir.entryInfoList( QDir::Files | QDir::NoSymLinks | QDir::Readable, QDir::Name | QDir::IgnoreCase );
	for ( const QFileInfo& fileInfo : files )
	{
		const QString ext = fileInfo.suffix().trimmed().toLower();
		if ( ext == QLatin1String( "pk3" ) || ext == QLatin1String( "pak" ) || ext == QLatin1String( "pk4" ) ) {
			return true;
		}
		if ( archiveTypes.contains( ext ) ) {
			return true;
		}
	}
	return false;
}

QVector<SupportedGameModEntry> detect_supported_game_mods_for_installation( const CGameDescription& game, const QString& installPath ){
	QVector<SupportedGameModEntry> candidates;
	const QString gameName = QString::fromUtf8( game.getRequiredKeyValue( "name" ) ).trimmed();
	const QString baseGame = QString::fromUtf8( game.getRequiredKeyValue( "basegame" ) ).trimmed();
	const QString baseGameName = QString::fromUtf8( game.getRequiredKeyValue( "basegamename" ) ).trimmed();
	append_supported_game_mod( candidates, baseGame, baseGameName );

	const QString knownGame = QString::fromUtf8( game.getKeyValue( "knowngame" ) ).trimmed();
	const QString knownGameName = QString::fromUtf8( game.getKeyValue( "knowngamename" ) ).trimmed();
	append_supported_game_mod( candidates, knownGame, knownGameName.isEmpty() ? knownGame : knownGameName );

	const QStringList knownMods = descriptor_split_list( game.getKeyValue( "knownmods" ), ",;|" );
	const QStringList knownModNames = descriptor_split_list( game.getKeyValue( "knownmodnames" ), ",;|" );
	for ( int i = 0; i < knownMods.size(); ++i )
	{
		const QString label = i < knownModNames.size() ? knownModNames[i] : knownMods[i];
		append_supported_game_mod( candidates, knownMods[i], label );
	}

	const QString defaultGameName = QString::fromUtf8( game.getKeyValue( "defaultgamename" ) ).trimmed();
	append_supported_game_mod( candidates, defaultGameName, gameName );

	QVector<SupportedGameModEntry> detected;
	for ( const SupportedGameModEntry& candidate : candidates )
	{
		if ( !installation_game_directory_exists( installPath, candidate.dir ) ) {
			continue;
		}
		if ( !installation_game_directory_looks_like_mod( game, installPath, candidate.dir ) ) {
			continue;
		}
		append_supported_game_mod( detected, candidate.dir, candidate.label );
	}

	if ( detected.isEmpty() && installation_game_directory_exists( installPath, baseGame ) ) {
		append_supported_game_mod( detected, baseGame, baseGameName );
	}

	return detected;
}

QString choose_supported_game_name_for_installation(
	const CGameDescription& game,
	const QString& installPath,
	const QString& preferredGameName ){
	const QVector<SupportedGameModEntry> supportedMods = detect_supported_game_mods_for_installation( game, installPath );
	const auto containsDir = [&supportedMods]( const QString& dir ){
		const QString cleanDir = normalise_game_name_token( dir );
		for ( const SupportedGameModEntry& entry : supportedMods )
		{
			if ( entry.dir.compare( cleanDir, Qt::CaseInsensitive ) == 0 ) {
				return true;
			}
		}
		return false;
	};

	const QString preferred = normalise_game_name_token( preferredGameName );
	if ( !preferred.isEmpty()
	  && installation_game_directory_exists( installPath, preferred )
	  && installation_game_directory_looks_like_mod( game, installPath, preferred ) ) {
		return preferred;
	}
	if ( !preferred.isEmpty() && containsDir( preferred ) ) {
		return preferred;
	}

	const QString defaultGameName = QString::fromUtf8( game.getKeyValue( "defaultgamename" ) ).trimmed();
	if ( !defaultGameName.isEmpty() && containsDir( defaultGameName ) ) {
		return normalise_game_name_token( defaultGameName );
	}

	const QString baseGame = QString::fromUtf8( game.getRequiredKeyValue( "basegame" ) ).trimmed();
	if ( containsDir( baseGame ) ) {
		return normalise_game_name_token( baseGame );
	}

	if ( !supportedMods.isEmpty() ) {
		return supportedMods.front().dir;
	}

	return normalise_game_name_token( defaultGameName.isEmpty() ? baseGame : defaultGameName );
}

QString supported_game_mod_label_for_installation(
	const CGameDescription& game,
	const QString& installPath,
	const QString& gameName ){
	const QString selected = normalise_game_name_token( gameName );
	const QVector<SupportedGameModEntry> supportedMods = detect_supported_game_mods_for_installation( game, installPath );
	for ( const SupportedGameModEntry& entry : supportedMods )
	{
		if ( entry.dir.compare( selected, Qt::CaseInsensitive ) == 0 ) {
			return entry.label;
		}
	}
	return selected;
}

const char* installation_engine_attribute(){
#if defined( WIN32 )
	return "engine_win32";
#elif defined( __APPLE__ )
	return "engine_macos";
#elif defined( __linux__ ) || defined( __FreeBSD__ )
	return "engine_linux";
#else
#error "unsupported platform"
#endif
}

QString game_install_state_file_path(){
	const QString root = QString::fromUtf8( g_Preferences.m_global_rc_path.c_str() );
	if ( root.isEmpty() ) {
		return {};
	}
	QDir dir( root );
	if ( !dir.exists() ) {
		dir.mkpath( "." );
	}
	return dir.filePath( GAME_INSTALL_STATE_FILENAME );
}

QString normalise_installation_path( const QString& path ){
	QString cleaned = QDir::cleanPath( path.trimmed() );
#if defined( WIN32 )
	cleaned = cleaned.toLower();
#endif
	return cleaned;
}

bool installation_path_equal( const QString& lhs, const QString& rhs ){
	return normalise_installation_path( lhs ) == normalise_installation_path( rhs );
}

const CGameDescription* find_game_by_file( const QVector<const CGameDescription*>& games, const QString& gameFile ){
	for ( const CGameDescription* game : games )
	{
		if ( game == nullptr ) {
			continue;
		}
		if ( QString::fromUtf8( game->mGameFile.c_str() ) == gameFile ) {
			return game;
		}
	}
	return nullptr;
}

QVector<int> installation_indexes_for_game( const GameInstallationState& state, const QString& gameFile ){
	QVector<int> indexes;
	for ( int i = 0; i < state.installations.size(); ++i )
	{
		if ( state.installations[i].gameFile == gameFile ) {
			indexes.push_back( i );
		}
	}
	return indexes;
}

int installation_count_for_game( const GameInstallationState& state, const QString& gameFile ){
	return installation_indexes_for_game( state, gameFile ).size();
}

const GameInstallationEntry* find_installation_by_id( const GameInstallationState& state, const QString& id ){
	if ( id.isEmpty() ) {
		return nullptr;
	}
	for ( const GameInstallationEntry& entry : state.installations )
	{
		if ( entry.id == id ) {
			return &entry;
		}
	}
	return nullptr;
}

GameInstallationEntry* find_installation_by_id( GameInstallationState& state, const QString& id ){
	if ( id.isEmpty() ) {
		return nullptr;
	}
	for ( GameInstallationEntry& entry : state.installations )
	{
		if ( entry.id == id ) {
			return &entry;
		}
	}
	return nullptr;
}

QString infer_installation_storefront( const QString& path, const QString& sourceHint ){
	const QString sourceLower = sourceHint.trimmed().toLower();
	if ( sourceLower.contains( "steam" ) ) {
		return "Steam";
	}
	if ( sourceLower.contains( "gog" ) ) {
		return "GOG.com";
	}
	if ( sourceLower.contains( "epic" ) ) {
		return "Epic Games";
	}
	if ( sourceLower.contains( "itch" ) ) {
		return "itch.io";
	}
	if ( sourceLower.contains( "microsoft" ) || sourceLower.contains( "xbox" ) ) {
		return "Microsoft Store";
	}

	const QString normalisedPath = QDir::fromNativeSeparators( QDir::cleanPath( path ) ).toLower();
	if ( normalisedPath.contains( "/steamapps/" ) ) {
		return "Steam";
	}
	if ( normalisedPath.contains( "/gog galaxy/" ) || normalisedPath.contains( "/gog games/" ) || normalisedPath.contains( "/gog.com/" ) ) {
		return "GOG.com";
	}
	if ( normalisedPath.contains( "/epic games/" ) || normalisedPath.contains( "/epicgameslauncher/" ) ) {
		return "Epic Games";
	}
	if ( normalisedPath.contains( "/itch.io/" ) ) {
		return "itch.io";
	}
	if ( normalisedPath.contains( "/xboxgames/" ) || normalisedPath.contains( "/windowsapps/" ) ) {
		return "Microsoft Store";
	}
	return {};
}

QString installation_name_base( const CGameDescription& game, const QString& path, const QString& sourceHint ){
	const QString gameName = QString::fromUtf8( game.getRequiredKeyValue( "name" ) );
	QString leaf = QFileInfo( path ).fileName().trimmed();
	if ( leaf.isEmpty() ) {
		leaf = gameName;
	}
	const bool leafUseful = leaf.compare( gameName, Qt::CaseInsensitive ) != 0;

	const QString storefront = infer_installation_storefront( path, sourceHint );
	if ( !storefront.isEmpty() && leafUseful ) {
		return QString( "%1 (%2 - %3)" ).arg( gameName, storefront, leaf );
	}
	if ( !storefront.isEmpty() ) {
		return QString( "%1 (%2)" ).arg( gameName, storefront );
	}
	return leafUseful ? QString( "%1 (%2)" ).arg( gameName, leaf ) : gameName;
}

QString installation_name_without_generated_suffix( const QString& name ){
	const QString trimmed = name.trimmed();
	const int marker = trimmed.lastIndexOf( " #" );
	if ( marker <= 0 ) {
		return trimmed;
	}
	for ( int i = marker + 2; i < trimmed.size(); ++i )
	{
		if ( !trimmed[i].isDigit() ) {
			return trimmed;
		}
	}
	return trimmed.left( marker ).trimmed();
}

bool installation_name_looks_autogenerated(
	const CGameDescription& game,
	const QString& name,
	const QString& path,
	const QString& sourceHint ){
	const QString base = installation_name_without_generated_suffix( name );
	const QString legacyBase = installation_name_base( game, path, QString() );
	if ( base.compare( legacyBase, Qt::CaseInsensitive ) == 0 ) {
		return true;
	}
	const QString storefrontBase = installation_name_base( game, path, sourceHint );
	return base.compare( storefrontBase, Qt::CaseInsensitive ) == 0;
}

QString choose_installation_name(
	const CGameDescription& game,
	const QString& path,
	const GameInstallationState& state,
	const QString& sourceHint = QString(),
	const QString& skipId = QString() ){
	const QString baseName = installation_name_base( game, path, sourceHint );

	auto nameTaken = [&state, &skipId]( const QString& candidate ){
		for ( const GameInstallationEntry& existing : state.installations )
		{
			if ( !skipId.isEmpty() && existing.id == skipId ) {
				continue;
			}
			if ( existing.name.compare( candidate, Qt::CaseInsensitive ) == 0 ) {
				return true;
			}
		}
		return false;
	};

	if ( !nameTaken( baseName ) ) {
		return baseName;
	}

	for ( int suffix = 2; suffix < 1000; ++suffix )
	{
		const QString candidate = QString( "%1 #%2" ).arg( baseName ).arg( suffix );
		if ( !nameTaken( candidate ) ) {
			return candidate;
		}
	}
	return baseName;
}

QString installation_detail_text( const GameInstallationEntry& entry, const QString& gameModLabel = QString() ){
	QString detail = QFileInfo( entry.path ).absoluteFilePath();
	if ( !entry.engineExecutable.trimmed().isEmpty() ) {
		detail += QString( "\nEngine: %1" ).arg( entry.engineExecutable.trimmed() );
	}
	if ( !gameModLabel.trimmed().isEmpty() ) {
		detail += QString( "\nMod: %1" ).arg( gameModLabel.trimmed() );
	}
	return detail;
}

QString engine_default_for_game( const CGameDescription& game ){
	return QString::fromUtf8( game.getKeyValue( installation_engine_attribute() ) );
}

const char* installation_mp_engine_attribute(){
#if defined( WIN32 )
	return "mp_engine_win32";
#elif defined( __APPLE__ )
	return "mp_engine_macos";
#elif defined( __linux__ ) || defined( __FreeBSD__ )
	return "mp_engine_linux";
#else
#error "unsupported platform"
#endif
}

QString engine_mp_default_for_game( const CGameDescription& game ){
	return QString::fromUtf8( game.getKeyValue( installation_mp_engine_attribute() ) );
}

int source_port_preference_score( const QString& lowerFileName ){
	struct TokenScore
	{
		const char* token;
		int score;
	};

	static const TokenScore kTokenScores[] = {
		{ "ironwail", 600 },
		{ "vkquake", 590 },
		{ "quakespasm", 580 },
		{ "quakespasm-spiked", 585 },
		{ "fteqw", 560 },
		{ "ezquake", 550 },
		{ "yquake2", 600 },
		{ "q2pro", 590 },
		{ "kmquake2", 585 },
		{ "q2vkpt", 580 },
		{ "q2rtx", 570 },
		{ "ioq3", 600 },
		{ "ioquake3", 600 },
		{ "quake3e", 595 },
		{ "etlegacy", 600 },
		{ "iowolfsp", 590 },
		{ "iowolfmp", 590 },
		{ "dhewm3", 600 },
		{ "rbdoom3bfg", 595 },
		{ "darkplaces", 580 },
		{ "daemon", 560 },
		{ "xonotic", 560 },
		{ "warsow", 550 },
		{ "warfork", 550 },
		{ "openarena", 540 }
	};

	int bestScore = 0;
	for ( const TokenScore& token : kTokenScores )
	{
		if ( lowerFileName.contains( QLatin1String( token.token ) ) && token.score > bestScore ) {
			bestScore = token.score;
		}
	}
	return bestScore;
}

QString detect_best_engine_executable_for_installation( const CGameDescription& game, const QString& installPath ){
	const QString installRoot = QDir::cleanPath( installPath.trimmed() );
	if ( installRoot.isEmpty() ) {
		return {};
	}

	const QDir rootDir( installRoot );
	if ( !rootDir.exists() ) {
		return {};
	}

	const QString defaultEngine = QDir::fromNativeSeparators( engine_default_for_game( game ).trimmed() );
	const QString mpEngine = QDir::fromNativeSeparators( engine_mp_default_for_game( game ).trimmed() );
	const QString defaultEngineFile = QFileInfo( defaultEngine ).fileName().toLower();
	const QString mpEngineFile = QFileInfo( mpEngine ).fileName().toLower();
	const QString baseGameToken = QString::fromUtf8( game.getKeyValue( "basegame" ) ).toLower();

	struct Candidate
	{
		QString executable;
		int score;
	};

	QVector<Candidate> candidates;
	QHash<QString, bool> seenCanonicalPaths;

	auto addCandidate = [&]( const QString& candidateInput ){
		QString candidate = QDir::fromNativeSeparators( candidateInput.trimmed() );
		if ( candidate.isEmpty() ) {
			return;
		}

		const QByteArray candidateUtf8 = candidate.toUtf8();
		const bool absolute = path_is_absolute( candidateUtf8.constData() );
		const QString absolutePath = absolute
			? QDir::cleanPath( candidate )
			: QDir::cleanPath( rootDir.filePath( candidate ) );

		const QFileInfo info( absolutePath );
		if ( !info.exists() || !info.isFile() ) {
			return;
		}

#if defined( WIN32 )
		if ( info.suffix().compare( "exe", Qt::CaseInsensitive ) != 0 ) {
			return;
		}
#else
		if ( !info.isExecutable() ) {
			return;
		}
#endif

		const QString lowerName = info.fileName().toLower();
		if ( lowerName.contains( "dedicated" )
		  || lowerName.contains( "server" )
		  || lowerName.contains( "headless" )
		  || lowerName.contains( "launcher" )
		  || lowerName.contains( "updater" )
		  || lowerName.contains( "patcher" ) ) {
			return;
		}

		const QString canonical = QDir::cleanPath( QDir::fromNativeSeparators( info.absoluteFilePath() ) ).toLower();
		if ( seenCanonicalPaths.value( canonical ) ) {
			return;
		}

		QString storedExecutable = absolute
			? QDir::cleanPath( QDir::fromNativeSeparators( info.absoluteFilePath() ) )
			: QDir::cleanPath( QDir::fromNativeSeparators( rootDir.relativeFilePath( info.absoluteFilePath() ) ) );

		int score = source_port_preference_score( lowerName );
		if ( !defaultEngineFile.isEmpty() && lowerName == defaultEngineFile ) {
			score += 140;
		}
		if ( !mpEngineFile.isEmpty() && lowerName == mpEngineFile ) {
			score += 110;
		}
		if ( !baseGameToken.isEmpty() && lowerName.contains( baseGameToken ) ) {
			score += 20;
		}
		if ( lowerName.contains( "64" ) || lowerName.contains( "x64" ) ) {
			score += 15;
		}
		if ( lowerName.contains( "sdl" ) ) {
			score += 8;
		}
		score -= storedExecutable.count( '/' ) * 2;

		candidates.push_back( { storedExecutable, score } );
		seenCanonicalPaths.insert( canonical, true );
	};

	addCandidate( defaultEngine );
	addCandidate( mpEngine );

	QStringList scanDirectories;
	scanDirectories << "." << "bin" << "bin64" << "bin32" << "x64" << "x86";

	if ( !defaultEngine.isEmpty() ) {
		const QByteArray defaultEngineUtf8 = defaultEngine.toUtf8();
		if ( !path_is_absolute( defaultEngineUtf8.constData() ) ) {
			const QString defaultParent = QDir::cleanPath( QFileInfo( defaultEngine ).path() );
			if ( !defaultParent.isEmpty()
			  && defaultParent != "."
			  && !scanDirectories.contains( defaultParent, Qt::CaseInsensitive ) ) {
				scanDirectories.push_back( defaultParent );
			}
		}
	}

	for ( const QString& relativeDirRaw : scanDirectories )
	{
		const QString relativeDir = QDir::cleanPath( relativeDirRaw.trimmed() );
		QDir dir( installRoot );
		if ( relativeDir != "." && relativeDir != "/" ) {
			if ( !dir.cd( relativeDir ) ) {
				continue;
			}
		}

#if defined( WIN32 )
		const QStringList filters = { "*.exe" };
#else
		const QStringList filters = { "*" };
#endif
		const QFileInfoList files = dir.entryInfoList(
			filters,
			QDir::Files | QDir::NoSymLinks | QDir::Readable,
			QDir::Name | QDir::IgnoreCase );

		for ( const QFileInfo& fileInfo : files )
		{
#if !defined( WIN32 )
			if ( !fileInfo.isExecutable() ) {
				continue;
			}
#endif
			addCandidate( rootDir.relativeFilePath( fileInfo.absoluteFilePath() ) );
		}
	}

	if ( candidates.isEmpty() ) {
		return {};
	}

	int bestIndex = 0;
	for ( int i = 1; i < candidates.size(); ++i )
	{
		const Candidate& current = candidates[i];
		const Candidate& best = candidates[bestIndex];
		if ( current.score > best.score ) {
			bestIndex = i;
			continue;
		}
		if ( current.score == best.score && current.executable.size() < best.executable.size() ) {
			bestIndex = i;
			continue;
		}
		if ( current.score == best.score
		  && current.executable.size() == best.executable.size()
		  && QString::compare( current.executable, best.executable, Qt::CaseInsensitive ) < 0 ) {
			bestIndex = i;
		}
	}

	return candidates[bestIndex].executable;
}

QPixmap game_icon_pixmap( const QString& gameName ){
	const int size = 34;
	QPixmap pixmap( size, size );
	pixmap.fill( Qt::transparent );

	uint seed = 0;
	for ( const QChar c : gameName )
	{
		seed = seed * 33u + static_cast<uint>( c.unicode() );
	}
	const int hue = static_cast<int>( seed % 360u );
	const QColor base = QColor::fromHsv( hue, 165, 180 );
	const QColor accent = QColor::fromHsv( ( hue + 30 ) % 360, 150, 230 );

	QPainter painter( &pixmap );
	painter.setRenderHint( QPainter::Antialiasing, true );
	QLinearGradient gradient( 0, 0, size, size );
	gradient.setColorAt( 0.0, accent );
	gradient.setColorAt( 1.0, base );
	painter.setPen( Qt::NoPen );
	painter.setBrush( gradient );
	painter.drawRoundedRect( QRectF( 0, 0, size, size ), 8.0, 8.0 );

	const QString initial = gameName.trimmed().isEmpty() ? QString( "?" ) : gameName.trimmed().left( 1 ).toUpper();
	QFont font = painter.font();
	font.setBold( true );
	font.setPointSize( 12 );
	painter.setFont( font );
	painter.setPen( Qt::white );
	painter.drawText( QRect( 0, 0, size, size ), Qt::AlignCenter, initial );
	return pixmap;
}

GameInstallationState load_game_installation_state( QString* error ){
	if ( error != nullptr ) {
		error->clear();
	}

	GameInstallationState state;
	const QString filePath = game_install_state_file_path();
	if ( filePath.isEmpty() ) {
		return state;
	}

	QFile file( filePath );
	if ( !file.exists() ) {
		return state;
	}
	if ( !file.open( QIODevice::ReadOnly ) ) {
		if ( error != nullptr ) {
			*error = QString( "Unable to open installation state: %1" ).arg( file.errorString() );
		}
		return state;
	}

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson( file.readAll(), &parseError );
	if ( parseError.error != QJsonParseError::NoError || !doc.isObject() ) {
		if ( error != nullptr ) {
			*error = QString( "Failed to parse installation state: %1" ).arg( parseError.errorString() );
		}
		return state;
	}

	const QJsonObject root = doc.object();
	state.selectedGameFile = root.value( "selected_game_file" ).toString();

	const QJsonObject selectedByGame = root.value( "selected_installation_by_game" ).toObject();
	for ( auto it = selectedByGame.begin(); it != selectedByGame.end(); ++it )
	{
		if ( it.value().isString() ) {
			state.selectedInstallByGame.insert( it.key(), it.value().toString() );
		}
	}

	const QJsonObject selectedGameNameByInstall = root.value( "selected_game_name_by_installation" ).toObject();
	for ( auto it = selectedGameNameByInstall.begin(); it != selectedGameNameByInstall.end(); ++it )
	{
		if ( it.value().isString() ) {
			state.selectedGameNameByInstall.insert( it.key(), normalise_game_name_token( it.value().toString() ) );
		}
	}

	const QJsonArray installations = root.value( "installations" ).toArray();
	state.installations.reserve( installations.size() );
	for ( const QJsonValue& value : installations )
	{
		if ( !value.isObject() ) {
			continue;
		}
		const QJsonObject object = value.toObject();
		GameInstallationEntry entry;
		entry.id = object.value( "id" ).toString();
		entry.gameFile = object.value( "game_file" ).toString();
		entry.name = object.value( "name" ).toString();
		entry.path = object.value( "path" ).toString();
		entry.engineExecutable = object.value( "engine_executable" ).toString();

		if ( entry.id.isEmpty() || entry.gameFile.isEmpty() || entry.path.trimmed().isEmpty() ) {
			continue;
		}
		entry.path = QDir::cleanPath( entry.path.trimmed() );
		entry.name = entry.name.trimmed();
		if ( entry.name.isEmpty() ) {
			entry.name = QFileInfo( entry.path ).fileName();
		}
		state.installations.push_back( entry );
	}

	return state;
}

bool save_game_installation_state( const GameInstallationState& state, QString* error ){
	if ( error != nullptr ) {
		error->clear();
	}

	const QString filePath = game_install_state_file_path();
	if ( filePath.isEmpty() ) {
		if ( error != nullptr ) {
			*error = "Global settings path is unavailable.";
		}
		return false;
	}

	QJsonObject root;
	root.insert( "version", 2 );
	root.insert( "selected_game_file", state.selectedGameFile );

	QJsonObject selectedByGame;
	for ( auto it = state.selectedInstallByGame.begin(); it != state.selectedInstallByGame.end(); ++it )
	{
		if ( !it.value().isEmpty() ) {
			selectedByGame.insert( it.key(), it.value() );
		}
	}
	root.insert( "selected_installation_by_game", selectedByGame );

	QJsonObject selectedGameNameByInstall;
	for ( auto it = state.selectedGameNameByInstall.begin(); it != state.selectedGameNameByInstall.end(); ++it )
	{
		const QString value = normalise_game_name_token( it.value() );
		if ( !it.key().isEmpty() && !value.isEmpty() ) {
			selectedGameNameByInstall.insert( it.key(), value );
		}
	}
	root.insert( "selected_game_name_by_installation", selectedGameNameByInstall );

	QJsonArray installations;
	for ( const GameInstallationEntry& entry : state.installations )
	{
		if ( entry.id.isEmpty() || entry.gameFile.isEmpty() || entry.path.trimmed().isEmpty() ) {
			continue;
		}
		QJsonObject object;
		object.insert( "id", entry.id );
		object.insert( "game_file", entry.gameFile );
		object.insert( "name", entry.name );
		object.insert( "path", QDir::cleanPath( entry.path ) );
		object.insert( "engine_executable", entry.engineExecutable.trimmed() );
		installations.push_back( object );
	}
	root.insert( "installations", installations );

	QSaveFile file( filePath );
	if ( !file.open( QIODevice::WriteOnly ) ) {
		if ( error != nullptr ) {
			*error = QString( "Unable to save installation state: %1" ).arg( file.errorString() );
		}
		return false;
	}
	const QByteArray contents = QJsonDocument( root ).toJson( QJsonDocument::Indented );
	if ( file.write( contents ) != contents.size() ) {
		if ( error != nullptr ) {
			*error = QString( "Unable to write installation state: %1" ).arg( file.errorString() );
		}
		file.cancelWriting();
		return false;
	}
	if ( !file.commit() ) {
		if ( error != nullptr ) {
			*error = QString( "Unable to commit installation state: %1" ).arg( file.errorString() );
		}
		return false;
	}
	return true;
}
} // namespace

const char* StartupGameInstallationPath_get(){
	return g_startupGameInstallationPath.c_str();
}

const char* StartupGameInstallationEngineExecutable_get(){
	return g_startupGameInstallationEngineExecutable.c_str();
}

const char* StartupGameInstallationId_get(){
	return g_startupGameInstallationId.c_str();
}

const char* StartupGameInstallationGameName_get(){
	return g_startupGameInstallationGameName.c_str();
}

void StartupGameInstallationSelectedGameName_set( const char* gamename ){
	const QString cleanGameName = normalise_game_name_token( QString::fromUtf8( gamename == nullptr ? "" : gamename ) );
	g_startupGameInstallationGameName = cleanGameName.toUtf8().constData();

	const QString installId = QString::fromUtf8( g_startupGameInstallationId.c_str() ).trimmed();
	if ( installId.isEmpty() ) {
		return;
	}

	QString loadError;
	GameInstallationState state = load_game_installation_state( &loadError );
	if ( !loadError.isEmpty() ) {
		globalWarningStream() << loadError.toUtf8().constData() << '\n';
		return;
	}

	if ( cleanGameName.isEmpty() ) {
		state.selectedGameNameByInstall.remove( installId );
	}
	else{
		state.selectedGameNameByInstall[installId] = cleanGameName;
	}

	QString saveError;
	if ( !save_game_installation_state( state, &saveError ) && !saveError.isEmpty() ) {
		globalWarningStream() << saveError.toUtf8().constData() << '\n';
	}
}

bool StartupGameInstallationConfigured(){
	return !g_startupGameInstallationPath.empty();
}

class GameInstallationEditorDialog : public QDialog
{
	const CGameDescription& m_game;
	GameInstallationEntry m_entry;
	QLineEdit* m_nameEdit = nullptr;
	QLineEdit* m_pathEdit = nullptr;
	QLineEdit* m_engineEdit = nullptr;

public:
	GameInstallationEditorDialog( const CGameDescription& game, const GameInstallationEntry& initial, QWidget* parent ) :
		QDialog( parent ),
		m_game( game ),
		m_entry( initial ){
		buildUi();
		loadState();
	}

	const GameInstallationEntry& result() const {
		return m_entry;
	}

private:
	void buildUi(){
		setModal( true );
		setWindowTitle( m_entry.id.isEmpty() ? "Add Installation" : "Edit Installation" );
		setMinimumWidth( 600 );

		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 18, 16, 18, 16 );
		root->setSpacing( 12 );

		auto* title = new QLabel( "Installation Settings", this );
		QFont titleFont = title->font();
		titleFont.setPointSize( titleFont.pointSize() + 3 );
		titleFont.setBold( true );
		title->setFont( titleFont );
		root->addWidget( title );

		auto* hint = new QLabel(
			"Set the game installation directory and optional engine executable override for Run Map.",
			this );
		hint->setWordWrap( true );
		root->addWidget( hint );

		auto* form = new QGridLayout;
		form->setColumnStretch( 0, 0 );
		form->setColumnStretch( 1, 1 );
		form->setHorizontalSpacing( 10 );
		form->setVerticalSpacing( 10 );

		form->addWidget( new QLabel( "Name", this ), 0, 0 );
		m_nameEdit = new QLineEdit( this );
		form->addWidget( m_nameEdit, 0, 1 );

		form->addWidget( new QLabel( "Installation Path", this ), 1, 0 );
		{
			auto* rowWidget = new QWidget( this );
			auto* row = new QHBoxLayout( rowWidget );
			row->setContentsMargins( 0, 0, 0, 0 );
			row->setSpacing( 8 );
			m_pathEdit = new QLineEdit( rowWidget );
			m_pathEdit->setPlaceholderText( "Directory containing game data and executables" );
			auto* browse = new QPushButton( "Browse...", rowWidget );
			row->addWidget( m_pathEdit, 1 );
			row->addWidget( browse, 0 );
			form->addWidget( rowWidget, 1, 1 );
			QObject::connect( browse, &QPushButton::clicked, this, [this](){
				const QString initial = m_pathEdit->text().trimmed();
				const QString picked = QFileDialog::getExistingDirectory(
					this,
					"Choose Installation Directory",
					initial.isEmpty() ? QString() : initial
				);
				if ( !picked.isEmpty() ) {
					m_pathEdit->setText( QDir::cleanPath( picked ) );
				}
			} );
		}

		form->addWidget( new QLabel( "Engine Override", this ), 2, 0 );
		{
			auto* rowWidget = new QWidget( this );
			auto* row = new QHBoxLayout( rowWidget );
			row->setContentsMargins( 0, 0, 0, 0 );
			row->setSpacing( 8 );
			m_engineEdit = new QLineEdit( rowWidget );
			m_engineEdit->setPlaceholderText( "Optional custom engine executable (absolute path or file name)" );
			auto* browse = new QPushButton( "Browse...", rowWidget );
			auto* clear = new QPushButton( "Use Default", rowWidget );
			row->addWidget( m_engineEdit, 1 );
			row->addWidget( browse, 0 );
			row->addWidget( clear, 0 );
			form->addWidget( rowWidget, 2, 1 );
			QObject::connect( browse, &QPushButton::clicked, this, [this](){
				const QString initial = m_engineEdit->text().trimmed();
				const QString startDir = QFileInfo( initial ).exists()
					? QFileInfo( initial ).absolutePath()
					: m_pathEdit->text().trimmed();
				const QString picked = QFileDialog::getOpenFileName(
					this,
					"Choose Engine Executable",
					startDir
				);
				if ( !picked.isEmpty() ) {
					m_engineEdit->setText( QDir::cleanPath( picked ) );
				}
			} );
			QObject::connect( clear, &QPushButton::clicked, this, [this](){
				m_engineEdit->clear();
			} );
		}

		root->addLayout( form );

		auto* defaults = new QLabel(
			QString( "Game default engine: %1" ).arg( engine_default_for_game( m_game ) ),
			this
		);
		defaults->setWordWrap( true );
		root->addWidget( defaults );

		auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
		root->addWidget( buttons );
		QObject::connect( buttons, &QDialogButtonBox::accepted, this, [this](){
			if ( validateAndStore() ) {
				accept();
			}
		} );
		QObject::connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
	}

	void loadState(){
		if ( m_nameEdit != nullptr ) {
			m_nameEdit->setText( m_entry.name.trimmed() );
		}
		if ( m_pathEdit != nullptr ) {
			m_pathEdit->setText( QDir::cleanPath( m_entry.path.trimmed() ) );
		}
		if ( m_engineEdit != nullptr ) {
			m_engineEdit->setText( m_entry.engineExecutable.trimmed() );
		}

		if ( m_nameEdit != nullptr && m_nameEdit->text().trimmed().isEmpty() ) {
			m_nameEdit->setText( QString::fromUtf8( m_game.getRequiredKeyValue( "name" ) ) );
		}
	}

	bool validateAndStore(){
		const QString name = m_nameEdit != nullptr ? m_nameEdit->text().trimmed() : QString();
		const QString path = m_pathEdit != nullptr ? m_pathEdit->text().trimmed() : QString();
		const QString engine = m_engineEdit != nullptr ? m_engineEdit->text().trimmed() : QString();

		if ( name.isEmpty() ) {
			QMessageBox::warning( this, "Installation", "Name cannot be empty." );
			return false;
		}
		if ( path.isEmpty() ) {
			QMessageBox::warning( this, "Installation", "Installation path cannot be empty." );
			return false;
		}
		const QFileInfo pathInfo( path );
		if ( !pathInfo.exists() || !pathInfo.isDir() ) {
			QMessageBox::warning( this, "Installation", QString( "Invalid installation directory:\n%1" ).arg( path ) );
			return false;
		}
		if ( !engine.isEmpty() && path_is_absolute( engine.toUtf8().constData() ) ) {
			const QFileInfo exeInfo( engine );
			if ( !exeInfo.exists() || !exeInfo.isFile() ) {
				QMessageBox::warning( this, "Installation", QString( "Invalid engine executable:\n%1" ).arg( engine ) );
				return false;
			}
		}

		m_entry.name = name;
		m_entry.path = QDir::cleanPath( path );
		m_entry.engineExecutable = engine;
		return true;
	}
};

class GameSetupManagerDialog : public QDialog
{
	QVector<const CGameDescription*> m_games;
	GameInstallationState m_state;
	QString m_selectedGameFile;
	QString m_selectedInstallationId;

	QStackedWidget* m_pages = nullptr;
	QListWidget* m_gameList = nullptr;
	QListWidget* m_installList = nullptr;
	QLabel* m_installPageTitle = nullptr;
	QPushButton* m_continueButton = nullptr;
	QPushButton* m_removeGameInstallButton = nullptr;
	QPushButton* m_finishButton = nullptr;
	QPushButton* m_editInstallButton = nullptr;
	QPushButton* m_removeInstallButton = nullptr;
	QPushButton* m_detectGameButton = nullptr;
	QPushButton* m_detectInstallButton = nullptr;
	QLabel* m_detectedModLabel = nullptr;
	QComboBox* m_detectedModCombo = nullptr;
	QHash<QString, QFrame*> m_gameRowFrames;
	QHash<QString, QLabel*> m_gameRowBadges;

public:
	GameSetupManagerDialog(
		QVector<const CGameDescription*> games,
		GameInstallationState initialState,
		const QString& initialGameFile,
		QWidget* parent = nullptr ) :
		QDialog( parent ),
		m_games( std::move( games ) ),
		m_state( std::move( initialState ) ),
		m_selectedGameFile( initialGameFile ){
		buildUi();
		refreshGameList();
		selectInitialGame();
		updateGamePageState();
	}

	const GameInstallationState& state() const {
		return m_state;
	}

	QString selectedGameFile() const {
		return m_selectedGameFile;
	}

	QString selectedInstallationId() const {
		return m_selectedInstallationId;
	}

private:
	void buildUi(){
		setModal( true );
		setWindowTitle( "Game Setup" );
		resize( 900, 620 );

		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 18, 16, 18, 16 );
		root->setSpacing( 12 );

		auto* title = new QLabel( "Game Setup", this );
		QFont titleFont = title->font();
		titleFont.setPointSize( titleFont.pointSize() + 6 );
		titleFont.setBold( true );
		title->setFont( titleFont );
		root->addWidget( title );

		auto* subtitle = new QLabel(
			"Choose a game, manage its installations, then continue to select which installation and supported mod VibeRadiant should use.",
			this
		);
		subtitle->setWordWrap( true );
		root->addWidget( subtitle );

		m_pages = new QStackedWidget( this );
		root->addWidget( m_pages, 1 );

		buildGamePage();
		buildInstallPage();
	}

	void buildGamePage(){
		auto* page = new QWidget( m_pages );
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 0, 0, 0 );
		layout->setSpacing( 10 );

		m_gameList = new QListWidget( page );
		m_gameList->setSelectionMode( QAbstractItemView::SingleSelection );
		m_gameList->setSpacing( 4 );
		layout->addWidget( m_gameList, 1 );

		auto* buttons = new QHBoxLayout;
		buttons->setSpacing( 8 );
		auto* addInstall = new QPushButton( "Add Installation...", page );
		m_removeGameInstallButton = new QPushButton( "Remove Installation...", page );
		m_detectGameButton = new QPushButton( "Auto-detect", page );
		m_continueButton = new QPushButton( "Continue", page );
		auto* cancelButton = new QPushButton( "Cancel", page );
		m_continueButton->setDefault( true );

		buttons->addWidget( addInstall );
		buttons->addWidget( m_removeGameInstallButton );
		buttons->addWidget( m_detectGameButton );
		buttons->addStretch( 1 );
		buttons->addWidget( m_continueButton );
		buttons->addWidget( cancelButton );
		layout->addLayout( buttons );

		m_pages->addWidget( page );

		QObject::connect( m_gameList, &QListWidget::itemSelectionChanged, this, [this](){
			updateGameSelectionState();
		} );
		QObject::connect( m_gameList, &QListWidget::itemDoubleClicked, this, [this]( QListWidgetItem* ){
			openInstallSelectionPage();
		} );
		QObject::connect( addInstall, &QPushButton::clicked, this, [this](){
			addInstallationForSelectedGame();
		} );
		QObject::connect( m_removeGameInstallButton, &QPushButton::clicked, this, [this](){
			removeInstallationFromSelectedGame();
		} );
		QObject::connect( m_detectGameButton, &QPushButton::clicked, this, [this](){
			autoDetectForSelectedGame();
		} );
		QObject::connect( m_continueButton, &QPushButton::clicked, this, [this](){
			openInstallSelectionPage();
		} );
		QObject::connect( cancelButton, &QPushButton::clicked, this, &QDialog::reject );
	}

	void buildInstallPage(){
		auto* page = new QWidget( m_pages );
		auto* layout = new QVBoxLayout( page );
		layout->setContentsMargins( 0, 0, 0, 0 );
		layout->setSpacing( 10 );

		m_installPageTitle = new QLabel( page );
		QFont titleFont = m_installPageTitle->font();
		titleFont.setPointSize( titleFont.pointSize() + 2 );
		titleFont.setBold( true );
		m_installPageTitle->setFont( titleFont );
		layout->addWidget( m_installPageTitle );

		{
			auto* modRow = new QHBoxLayout;
			modRow->setContentsMargins( 0, 0, 0, 0 );
			modRow->setSpacing( 8 );
			m_detectedModLabel = new QLabel( "Supported mod", page );
			m_detectedModCombo = new QComboBox( page );
			m_detectedModCombo->setEnabled( false );
			m_detectedModCombo->setToolTip( "Curated supported mods detected for the selected installation." );
			modRow->addWidget( m_detectedModLabel, 0 );
			modRow->addWidget( m_detectedModCombo, 1 );
			layout->addLayout( modRow );
		}

		m_installList = new QListWidget( page );
		m_installList->setSelectionMode( QAbstractItemView::SingleSelection );
		m_installList->setAlternatingRowColors( true );
		layout->addWidget( m_installList, 1 );

		auto* buttons = new QHBoxLayout;
		buttons->setSpacing( 8 );
		auto* addInstall = new QPushButton( "Add...", page );
		m_editInstallButton = new QPushButton( "Edit...", page );
		m_removeInstallButton = new QPushButton( "Remove", page );
		m_detectInstallButton = new QPushButton( "Auto-detect", page );
		auto* backButton = new QPushButton( "Back", page );
		m_finishButton = new QPushButton( "Start with Installation", page );
		m_finishButton->setDefault( true );

		buttons->addWidget( addInstall );
		buttons->addWidget( m_editInstallButton );
		buttons->addWidget( m_removeInstallButton );
		buttons->addWidget( m_detectInstallButton );
		buttons->addStretch( 1 );
		buttons->addWidget( backButton );
		buttons->addWidget( m_finishButton );
		layout->addLayout( buttons );

		m_pages->addWidget( page );

		QObject::connect( m_installList, &QListWidget::itemSelectionChanged, this, [this](){
			updateInstallPageState();
		} );
		QObject::connect( m_detectedModCombo, qOverload<int>( &QComboBox::currentIndexChanged ), this, [this]( int index ){
			if ( index < 0 ) {
				return;
			}
			const QString installId = selectedInstallIdFromList();
			const QString gameFile = selectedGameFileFromList();
			const CGameDescription* game = find_game_by_file( m_games, gameFile );
			const GameInstallationEntry* entry = find_installation_by_id( m_state, installId );
			if ( installId.isEmpty() || game == nullptr || entry == nullptr ) {
				return;
			}
			const QString gameName = normalise_game_name_token( m_detectedModCombo->itemData( index ).toString() );
			if ( gameName.isEmpty() ) {
				return;
			}
			m_state.selectedGameNameByInstall[installId] = gameName;
			if ( QListWidgetItem* item = m_installList->currentItem(); item != nullptr ) {
				item->setToolTip( installation_detail_text(
					*entry,
					supported_game_mod_label_for_installation( *game, entry->path, gameName ) ) );
			}
		} );
		QObject::connect( m_installList, &QListWidget::itemDoubleClicked, this, [this]( QListWidgetItem* ){
			acceptSelectedInstallation();
		} );
		QObject::connect( addInstall, &QPushButton::clicked, this, [this](){
			addInstallationForSelectedGame();
		} );
		QObject::connect( m_editInstallButton, &QPushButton::clicked, this, [this](){
			editSelectedInstallation();
		} );
		QObject::connect( m_removeInstallButton, &QPushButton::clicked, this, [this](){
			removeSelectedInstallation();
		} );
		QObject::connect( m_detectInstallButton, &QPushButton::clicked, this, [this](){
			autoDetectForSelectedGame();
		} );
		QObject::connect( backButton, &QPushButton::clicked, this, [this](){
			m_pages->setCurrentIndex( 0 );
			updateGamePageState();
		} );
		QObject::connect( m_finishButton, &QPushButton::clicked, this, [this](){
			acceptSelectedInstallation();
		} );
	}

	void refreshGameList(){
		m_gameRowFrames.clear();
		m_gameRowBadges.clear();
		m_gameList->clear();
		for ( const CGameDescription* game : m_games )
		{
			if ( game == nullptr ) {
				continue;
			}
			const QString gameFile = QString::fromUtf8( game->mGameFile.c_str() );
			const QString gameName = QString::fromUtf8( game->getRequiredKeyValue( "name" ) );
			const int count = installation_count_for_game( m_state, gameFile );
			if ( count <= 0 ) {
				continue;
			}

			auto* item = new QListWidgetItem;
			item->setData( Qt::UserRole, gameFile );
			item->setSizeHint( QSize( 0, 66 ) );
			m_gameList->addItem( item );

			auto* frame = new QFrame( m_gameList );
			auto* row = new QHBoxLayout( frame );
			row->setContentsMargins( 12, 8, 12, 8 );
			row->setSpacing( 10 );

			auto* iconLabel = new QLabel( frame );
			iconLabel->setPixmap( game_icon_pixmap( gameName ) );
			iconLabel->setFixedSize( 34, 34 );
			row->addWidget( iconLabel, 0, Qt::AlignVCenter );

			auto* titleLabel = new QLabel( gameName, frame );
			QFont nameFont = titleLabel->font();
			nameFont.setPointSize( nameFont.pointSize() + 1 );
			nameFont.setBold( true );
			titleLabel->setFont( nameFont );
			row->addWidget( titleLabel, 1, Qt::AlignVCenter );

			auto* badge = new QLabel(
				count == 1
					? QStringLiteral( "1 installation" )
					: QString( "%1 installations" ).arg( count ),
				frame );
			badge->setStyleSheet(
				"QLabel { "
				"background: palette(highlight); "
				"color: palette(highlighted-text); "
				"border: 1px solid palette(mid); "
				"border-radius: 10px; "
				"padding: 3px 10px; "
				"font-weight: 600; "
				"}"
			);
			row->addWidget( badge, 0, Qt::AlignVCenter );

			frame->setToolTip( QString::fromUtf8( game->mGameFile.c_str() ) );
			m_gameList->setItemWidget( item, frame );
			m_gameRowFrames.insert( gameFile, frame );
			m_gameRowBadges.insert( gameFile, badge );
		}
		updateGameSelectionState();
	}

	void refreshInstallList(){
		m_installList->clear();
		const QString gameFile = selectedGameFileFromList();
		const CGameDescription* game = find_game_by_file( m_games, gameFile );
		if ( gameFile.isEmpty() || game == nullptr ) {
			refreshSupportedModsForSelectedInstallation();
			updateInstallPageState();
			return;
		}

		const QVector<int> indexes = installation_indexes_for_game( m_state, gameFile );
		for ( const int index : indexes )
		{
			const GameInstallationEntry& entry = m_state.installations[index];
			auto* item = new QListWidgetItem( entry.name, m_installList );
			item->setData( Qt::UserRole, entry.id );
			const QString gameName = choose_supported_game_name_for_installation(
				*game,
				entry.path,
				m_state.selectedGameNameByInstall.value( entry.id ) );
			item->setToolTip( installation_detail_text(
				entry,
				supported_game_mod_label_for_installation( *game, entry.path, gameName ) ) );
			item->setIcon( style()->standardIcon( QStyle::SP_DirOpenIcon ) );
			item->setText( QString( "%1\n%2" ).arg( entry.name, QFileInfo( entry.path ).absoluteFilePath() ) );
		}

		QString preferredId = m_state.selectedInstallByGame.value( gameFile );
		if ( preferredId.isEmpty() && !indexes.isEmpty() ) {
			preferredId = m_state.installations[indexes.first()].id;
		}

		for ( int i = 0; i < m_installList->count(); ++i )
		{
			QListWidgetItem* item = m_installList->item( i );
			if ( item != nullptr && item->data( Qt::UserRole ).toString() == preferredId ) {
				m_installList->setCurrentItem( item );
				break;
			}
		}
		if ( m_installList->currentItem() == nullptr && m_installList->count() > 0 ) {
			m_installList->setCurrentRow( 0 );
		}
		refreshSupportedModsForSelectedInstallation();
		updateInstallPageState();
	}

	void selectInitialGame(){
		QString initialGame = m_selectedGameFile;
		if ( initialGame.isEmpty() ) {
			initialGame = m_state.selectedGameFile;
		}
		if ( initialGame.isEmpty() && !m_games.isEmpty() ) {
			initialGame = QString::fromUtf8( m_games.front()->mGameFile.c_str() );
		}

		for ( int i = 0; i < m_gameList->count(); ++i )
		{
			QListWidgetItem* item = m_gameList->item( i );
			if ( item != nullptr && item->data( Qt::UserRole ).toString() == initialGame ) {
				m_gameList->setCurrentItem( item );
				break;
			}
		}
		if ( m_gameList->currentItem() == nullptr && m_gameList->count() > 0 ) {
			m_gameList->setCurrentRow( 0 );
		}
		updateGameSelectionState();
	}

	QString selectedGameFileFromList() const {
		const QListWidgetItem* item = m_gameList->currentItem();
		if ( item == nullptr ) {
			return {};
		}
		return item->data( Qt::UserRole ).toString();
	}

	QString selectedInstallIdFromList() const {
		const QListWidgetItem* item = m_installList->currentItem();
		if ( item == nullptr ) {
			return {};
		}
		return item->data( Qt::UserRole ).toString();
	}

	void updateGameSelectionState(){
		m_selectedGameFile = selectedGameFileFromList();
		for ( int i = 0; i < m_gameList->count(); ++i )
		{
			QListWidgetItem* item = m_gameList->item( i );
			if ( item == nullptr ) {
				continue;
			}
			const QString gameFile = item->data( Qt::UserRole ).toString();
			if ( QFrame* frame = m_gameRowFrames.value( gameFile, nullptr ) ) {
				const bool selected = ( gameFile == m_selectedGameFile );
				frame->setStyleSheet(
					selected
					? "QFrame { border: 1px solid palette(highlight); border-radius: 10px; background: palette(alternate-base); }"
					: "QFrame { border: 1px solid palette(midlight); border-radius: 10px; background: palette(base); }"
				);
			}
			if ( QLabel* badge = m_gameRowBadges.value( gameFile, nullptr ) ) {
				const int count = installation_count_for_game( m_state, gameFile );
				badge->setText(
					count == 1
						? QStringLiteral( "1 installation" )
						: QString( "%1 installations" ).arg( count ) );
			}
		}
		updateGamePageState();
		if ( m_pages->currentIndex() == 1 ) {
			refreshInstallList();
		}
	}

	const CGameDescription* promptGameForInstallation() {
		QStringList labels;
		QVector<const CGameDescription*> choices;
		labels.reserve( m_games.size() );
		choices.reserve( m_games.size() );
		for ( const CGameDescription* game : m_games )
		{
			if ( game == nullptr ) {
				continue;
			}
			const QString gameName = QString::fromUtf8( game->getRequiredKeyValue( "name" ) );
			const QString gameFile = QString::fromUtf8( game->mGameFile.c_str() );
			labels.push_back( QString( "%1 (%2)" ).arg( gameName, gameFile ) );
			choices.push_back( game );
		}
		if ( choices.isEmpty() ) {
			return nullptr;
		}
		bool ok = false;
		const QString chosen = QInputDialog::getItem(
			this,
			"Choose Game",
			"Game",
			labels,
			0,
			false,
			&ok
		);
		if ( !ok || chosen.isEmpty() ) {
			return nullptr;
		}
		const int index = labels.indexOf( chosen );
		if ( index < 0 || index >= choices.size() ) {
			return nullptr;
		}
		return choices[index];
	}

	void updateGamePageState(){
		const bool hasGame = !selectedGameFileFromList().isEmpty();
		const int count = installation_count_for_game( m_state, selectedGameFileFromList() );
		m_continueButton->setEnabled( hasGame && count > 0 );
		m_removeGameInstallButton->setEnabled( hasGame && count > 0 );
		m_detectGameButton->setEnabled( true );
	}

	void updateInstallPageState(){
		const bool hasSelection = !selectedInstallIdFromList().isEmpty();
		m_editInstallButton->setEnabled( hasSelection );
		m_removeInstallButton->setEnabled( hasSelection );
		m_finishButton->setEnabled( hasSelection );
		m_finishButton->setText( "Start with Installation" );
		m_detectInstallButton->setEnabled( !m_games.isEmpty() );
		if ( m_detectedModLabel != nullptr ) {
			m_detectedModLabel->setEnabled( hasSelection );
		}
		if ( m_detectedModCombo != nullptr ) {
			refreshSupportedModsForSelectedInstallation();
		}
	}

	void refreshSupportedModsForSelectedInstallation(){
		if ( m_detectedModCombo == nullptr ) {
			return;
		}

		QSignalBlocker blocker( m_detectedModCombo );
		m_detectedModCombo->clear();
		m_detectedModCombo->setEnabled( false );
		m_finishButton->setText( "Start with Installation" );

		const QString gameFile = selectedGameFileFromList();
		const QString installId = selectedInstallIdFromList();
		const CGameDescription* game = find_game_by_file( m_games, gameFile );
		const GameInstallationEntry* entry = find_installation_by_id( m_state, installId );
		if ( game == nullptr || entry == nullptr ) {
			m_detectedModCombo->addItem( "No installation selected" );
			return;
		}

		QVector<SupportedGameModEntry> supportedMods = detect_supported_game_mods_for_installation( *game, entry->path );
		const QString selectedGameName = choose_supported_game_name_for_installation(
			*game,
			entry->path,
			m_state.selectedGameNameByInstall.value( installId ) );
		if ( !selectedGameName.isEmpty() ) {
			m_state.selectedGameNameByInstall[installId] = selectedGameName;
		}
		bool selectedListed = false;
		for ( const SupportedGameModEntry& mod : supportedMods )
		{
			if ( mod.dir.compare( selectedGameName, Qt::CaseInsensitive ) == 0 ) {
				selectedListed = true;
				break;
			}
		}
		if ( !selectedListed
		  && !selectedGameName.isEmpty()
		  && installation_game_directory_exists( entry->path, selectedGameName )
		  && installation_game_directory_looks_like_mod( *game, entry->path, selectedGameName ) ) {
			append_supported_game_mod(
				supportedMods,
				selectedGameName,
				QString( "%1 (Custom)" ).arg( selectedGameName ) );
		}
		if ( supportedMods.isEmpty() ) {
			m_detectedModCombo->addItem( "No supported mods detected" );
			return;
		}

		for ( const SupportedGameModEntry& mod : supportedMods )
		{
			m_detectedModCombo->addItem( mod.label, mod.dir );
		}

		for ( int i = 0; i < m_detectedModCombo->count(); ++i )
		{
			if ( normalise_game_name_token( m_detectedModCombo->itemData( i ).toString() )
			  == normalise_game_name_token( selectedGameName ) ) {
				m_detectedModCombo->setCurrentIndex( i );
				break;
			}
		}
		if ( m_detectedModCombo->currentIndex() < 0 ) {
			m_detectedModCombo->setCurrentIndex( 0 );
		}

		const bool multipleMods = m_detectedModCombo->count() > 1;
		m_detectedModCombo->setEnabled( true );
		if ( multipleMods ) {
			m_finishButton->setText( "Choose Mod and Start" );
		}
	}

	bool confirmSupportedModSelectionForSelectedInstallation(){
		const QString gameFile = selectedGameFileFromList();
		const QString installId = selectedInstallIdFromList();
		const CGameDescription* game = find_game_by_file( m_games, gameFile );
		const GameInstallationEntry* entry = find_installation_by_id( m_state, installId );
		if ( game == nullptr || entry == nullptr ) {
			return false;
		}

		QVector<SupportedGameModEntry> supportedMods = detect_supported_game_mods_for_installation( *game, entry->path );
		const QString fallbackGameName = choose_supported_game_name_for_installation(
			*game,
			entry->path,
			m_state.selectedGameNameByInstall.value( installId ) );

		if ( supportedMods.isEmpty() ) {
			if ( !fallbackGameName.isEmpty() ) {
				m_state.selectedGameNameByInstall[installId] = fallbackGameName;
			}
			return true;
		}
		if ( supportedMods.size() == 1 ) {
			m_state.selectedGameNameByInstall[installId] = supportedMods.front().dir;
			return true;
		}

		QStringList labels;
		labels.reserve( supportedMods.size() );
		int currentIndex = 0;
		for ( int i = 0; i < supportedMods.size(); ++i )
		{
			const SupportedGameModEntry& mod = supportedMods[i];
			labels.push_back(
				mod.label.compare( mod.dir, Qt::CaseInsensitive ) == 0
					? mod.label
					: QString( "%1 (%2)" ).arg( mod.label, mod.dir ) );
			if ( mod.dir.compare( fallbackGameName, Qt::CaseInsensitive ) == 0 ) {
				currentIndex = i;
			}
		}

		bool ok = false;
		const QString chosen = QInputDialog::getItem(
			this,
			"Choose Supported Mod",
			QString( "Select the supported mod VibeRadiant should use for \"%1\"." ).arg( entry->name ),
			labels,
			currentIndex,
			false,
			&ok );
		if ( !ok || chosen.isEmpty() ) {
			return false;
		}

		const int chosenIndex = labels.indexOf( chosen );
		if ( chosenIndex < 0 || chosenIndex >= supportedMods.size() ) {
			return false;
		}

		const QString chosenGameName = supportedMods[chosenIndex].dir;
		m_state.selectedGameNameByInstall[installId] = chosenGameName;
		if ( m_detectedModCombo != nullptr ) {
			for ( int i = 0; i < m_detectedModCombo->count(); ++i )
			{
				if ( normalise_game_name_token( m_detectedModCombo->itemData( i ).toString() ) == chosenGameName ) {
					QSignalBlocker blocker( m_detectedModCombo );
					m_detectedModCombo->setCurrentIndex( i );
					break;
				}
			}
		}
		return true;
	}

	void openInstallSelectionPage(){
		const QString gameFile = selectedGameFileFromList();
		const CGameDescription* game = find_game_by_file( m_games, gameFile );
		if ( game == nullptr ) {
			return;
		}
		if ( installation_count_for_game( m_state, gameFile ) == 0 ) {
			QMessageBox::information( this, "Game Setup", "Add or detect an installation before continuing." );
			return;
		}
		m_installPageTitle->setText( QString( "Installations for %1" ).arg( game->getRequiredKeyValue( "name" ) ) );
		refreshInstallList();
		m_pages->setCurrentIndex( 1 );
	}

	bool gameHasInstallationPath( const QString& gameFile, const QString& path, const QString& skipId = QString() ) const {
		for ( const GameInstallationEntry& entry : m_state.installations )
		{
			if ( entry.gameFile != gameFile ) {
				continue;
			}
			if ( !skipId.isEmpty() && entry.id == skipId ) {
				continue;
			}
			if ( installation_path_equal( entry.path, path ) ) {
				return true;
			}
		}
		return false;
	}

	GameInstallationEntry* findInstallationForGamePath( const QString& gameFile, const QString& path ){
		for ( GameInstallationEntry& entry : m_state.installations )
		{
			if ( entry.gameFile != gameFile ) {
				continue;
			}
			if ( installation_path_equal( entry.path, path ) ) {
				return &entry;
			}
		}
		return nullptr;
	}

	void addInstallationForSelectedGame(){
		if ( m_games.isEmpty() ) {
			QMessageBox::warning( this, "Installation", "No game profiles are available. Install gamepacks first." );
			return;
		}

		QString gameFile = selectedGameFileFromList();
		const CGameDescription* game = find_game_by_file( m_games, gameFile );
		if ( game == nullptr ) {
			game = promptGameForInstallation();
			if ( game != nullptr ) {
				gameFile = QString::fromUtf8( game->mGameFile.c_str() );
			}
		}
		if ( game == nullptr ) {
			return;
		}

		GameInstallationEntry entry;
		entry.id = QUuid::createUuid().toString( QUuid::WithoutBraces );
		entry.gameFile = gameFile;
		entry.name = QString::fromUtf8( game->getRequiredKeyValue( "name" ) );

		GameInstallationEditorDialog editor( *game, entry, this );
		if ( editor.exec() != QDialog::Accepted ) {
			return;
		}

		entry = editor.result();
		if ( gameHasInstallationPath( gameFile, entry.path ) ) {
			QMessageBox::warning( this, "Installation", "That installation path is already configured for this game." );
			return;
		}
		if ( entry.name.trimmed().isEmpty() ) {
			entry.name = choose_installation_name( *game, entry.path, m_state );
		}
		m_state.installations.push_back( entry );
		m_state.selectedGameNameByInstall[entry.id] = choose_supported_game_name_for_installation( *game, entry.path, QString() );
		m_state.selectedGameFile = gameFile;
		m_state.selectedInstallByGame[gameFile] = entry.id;
		m_selectedInstallationId = entry.id;
		refreshGameList();
		selectGameInList( gameFile );
		if ( m_pages->currentIndex() == 1 ) {
			refreshInstallList();
		}
	}

	void removeInstallationFromSelectedGame(){
		const QString gameFile = selectedGameFileFromList();
		if ( gameFile.isEmpty() ) {
			return;
		}
		const QVector<int> indexes = installation_indexes_for_game( m_state, gameFile );
		if ( indexes.isEmpty() ) {
			return;
		}

		int removeIndex = indexes.first();
		if ( indexes.size() > 1 ) {
			QStringList labels;
			labels.reserve( indexes.size() );
			for ( const int i : indexes )
			{
				const GameInstallationEntry& entry = m_state.installations[i];
				labels.push_back( QString( "%1 - %2" ).arg( entry.name, QFileInfo( entry.path ).absoluteFilePath() ) );
			}
			bool ok = false;
			const QString selected = QInputDialog::getItem(
				this,
				"Remove Installation",
				"Choose installation to remove:",
				labels,
				0,
				false,
				&ok
			);
			if ( !ok || selected.isEmpty() ) {
				return;
			}
			const int listIndex = labels.indexOf( selected );
			if ( listIndex < 0 || listIndex >= indexes.size() ) {
				return;
			}
			removeIndex = indexes[listIndex];
		}

		const GameInstallationEntry toRemove = m_state.installations[removeIndex];
		if ( QMessageBox::question(
				this,
				"Remove Installation",
				QString( "Remove \"%1\"?" ).arg( toRemove.name ) ) != QMessageBox::Yes ) {
			return;
		}

		m_state.installations.removeAt( removeIndex );
		if ( m_state.selectedInstallByGame.value( gameFile ) == toRemove.id ) {
			m_state.selectedInstallByGame.remove( gameFile );
		}
		m_state.selectedGameNameByInstall.remove( toRemove.id );
		if ( m_selectedInstallationId == toRemove.id ) {
			m_selectedInstallationId.clear();
		}
		refreshGameList();
		selectGameInList( gameFile );
		if ( m_pages->currentIndex() == 1 ) {
			refreshInstallList();
		}
	}

	void autoDetectForSelectedGame(){
		if ( m_games.isEmpty() ) {
			QMessageBox::warning( this, "Auto-detect", "No game profiles are available. Install gamepacks first." );
			return;
		}

		const QString previousSelectedGame = selectedGameFileFromList();
		int scannedGames = 0;
		int detectedPaths = 0;
		int added = 0;
		int engineAssignedForNew = 0;
		int engineUpdatedExisting = 0;
		int supportedModUpdated = 0;
		QString firstTouchedGame;

		for ( const CGameDescription* game : m_games )
		{
			if ( game == nullptr ) {
				continue;
			}
			++scannedGames;

			const QString gameFile = QString::fromUtf8( game->mGameFile.c_str() );
			const GameInstallationEntry* selectedEntry = find_installation_by_id( m_state, m_state.selectedInstallByGame.value( gameFile ) );
			const QByteArray currentPath = ( selectedEntry != nullptr )
				? selectedEntry->path.toUtf8()
				: QByteArray();
			const std::vector<DetectedGameInstallPath> detected = EnginePath_detectInstallationsForGame(
				*game,
				currentPath.isEmpty() ? nullptr : currentPath.constData()
			);

			for ( const DetectedGameInstallPath& install : detected )
			{
				const QString path = QDir::cleanPath( QString::fromUtf8( install.path.c_str() ).trimmed() );
				if ( path.isEmpty() ) {
					continue;
				}
				++detectedPaths;

				const QString sourceHint = QString::fromUtf8( install.source.c_str() ).trimmed();
				const QString bestEngine = detect_best_engine_executable_for_installation( *game, path ).trimmed();
				if ( GameInstallationEntry* existing = findInstallationForGamePath( gameFile, path ); existing != nullptr ) {
					if ( existing->name.trimmed().isEmpty()
					  || installation_name_looks_autogenerated( *game, existing->name, existing->path, sourceHint ) ) {
						existing->name = choose_installation_name( *game, existing->path, m_state, sourceHint, existing->id );
					}
					if ( !bestEngine.isEmpty() && existing->engineExecutable.trimmed() != bestEngine ) {
						existing->engineExecutable = bestEngine;
						++engineUpdatedExisting;
						if ( firstTouchedGame.isEmpty() ) {
							firstTouchedGame = gameFile;
						}
					}
					const QString bestGameName = choose_supported_game_name_for_installation(
						*game,
						path,
						m_state.selectedGameNameByInstall.value( existing->id ) );
					if ( !bestGameName.isEmpty()
					  && m_state.selectedGameNameByInstall.value( existing->id ) != bestGameName ) {
						m_state.selectedGameNameByInstall[existing->id] = bestGameName;
						++supportedModUpdated;
						if ( firstTouchedGame.isEmpty() ) {
							firstTouchedGame = gameFile;
						}
					}
					if ( m_state.selectedInstallByGame.value( gameFile ).isEmpty() ) {
						m_state.selectedInstallByGame[gameFile] = existing->id;
					}
					continue;
				}

				GameInstallationEntry entry;
				entry.id = QUuid::createUuid().toString( QUuid::WithoutBraces );
				entry.gameFile = gameFile;
				entry.path = path;
				entry.name = choose_installation_name( *game, entry.path, m_state, sourceHint );
				entry.engineExecutable = bestEngine;

				m_state.installations.push_back( entry );
				m_state.selectedGameNameByInstall[entry.id] = choose_supported_game_name_for_installation( *game, entry.path, QString() );
				if ( m_state.selectedInstallByGame.value( gameFile ).isEmpty() ) {
					m_state.selectedInstallByGame[gameFile] = entry.id;
				}
				if ( firstTouchedGame.isEmpty() ) {
					firstTouchedGame = gameFile;
				}
				if ( !bestEngine.isEmpty() ) {
					++engineAssignedForNew;
				}
				++added;
			}
		}

		for ( const CGameDescription* game : m_games )
		{
			if ( game == nullptr ) {
				continue;
			}
			const QString gameFile = QString::fromUtf8( game->mGameFile.c_str() );
			if ( !m_state.selectedInstallByGame.value( gameFile ).isEmpty() ) {
				continue;
			}
			const QVector<int> indexes = installation_indexes_for_game( m_state, gameFile );
			if ( !indexes.isEmpty() ) {
				m_state.selectedInstallByGame[gameFile] = m_state.installations[indexes.front()].id;
			}
		}

		if ( added == 0 && engineUpdatedExisting == 0 && supportedModUpdated == 0 ) {
			QMessageBox::information( this, "Auto-detect", "No new installations, supported mods, or engine updates were detected." );
			return;
		}

		refreshGameList();
		QString focusGame = previousSelectedGame;
		if ( installation_count_for_game( m_state, focusGame ) == 0 ) {
			focusGame = firstTouchedGame;
		}
		if ( installation_count_for_game( m_state, focusGame ) == 0 && !m_state.selectedGameFile.isEmpty() ) {
			focusGame = m_state.selectedGameFile;
		}
		if ( installation_count_for_game( m_state, focusGame ) == 0 && !m_state.installations.isEmpty() ) {
			focusGame = m_state.installations.front().gameFile;
		}
		if ( !focusGame.isEmpty() ) {
			m_state.selectedGameFile = focusGame;
			selectGameInList( focusGame );
			m_selectedInstallationId = m_state.selectedInstallByGame.value( focusGame );
		}
		if ( m_pages->currentIndex() == 1 ) {
			refreshInstallList();
		}
		QMessageBox::information(
			this,
			"Auto-detect",
			QString(
				"Scanned %1 game profile(s).\n"
				"Detected %2 installation path(s).\n"
				"Added %3 installation(s).\n"
				"Assigned engine for %4 new installation(s).\n"
				"Updated engine for %5 existing installation(s).\n"
				"Updated supported mod selection for %6 installation(s)." )
				.arg( scannedGames )
				.arg( detectedPaths )
				.arg( added )
				.arg( engineAssignedForNew )
				.arg( engineUpdatedExisting )
				.arg( supportedModUpdated ) );
	}

	void editSelectedInstallation(){
		const QString id = selectedInstallIdFromList();
		GameInstallationEntry* entry = find_installation_by_id( m_state, id );
		if ( entry == nullptr ) {
			return;
		}
		const CGameDescription* game = find_game_by_file( m_games, entry->gameFile );
		if ( game == nullptr ) {
			return;
		}

		GameInstallationEditorDialog editor( *game, *entry, this );
		if ( editor.exec() != QDialog::Accepted ) {
			return;
		}

		GameInstallationEntry updated = editor.result();
		if ( gameHasInstallationPath( entry->gameFile, updated.path, entry->id ) ) {
			QMessageBox::warning( this, "Installation", "That installation path is already configured for this game." );
			return;
		}
		*entry = updated;
		m_state.selectedGameNameByInstall[entry->id] = choose_supported_game_name_for_installation(
			*game,
			entry->path,
			m_state.selectedGameNameByInstall.value( entry->id ) );
		m_state.selectedInstallByGame[entry->gameFile] = entry->id;
		m_selectedInstallationId = entry->id;
		refreshGameList();
		selectGameInList( entry->gameFile );
		refreshInstallList();
	}

	void removeSelectedInstallation(){
		const QString id = selectedInstallIdFromList();
		if ( id.isEmpty() ) {
			return;
		}
		GameInstallationEntry* entry = find_installation_by_id( m_state, id );
		if ( entry == nullptr ) {
			return;
		}
		const QString gameFile = entry->gameFile;
		if ( QMessageBox::question(
				this,
				"Remove Installation",
				QString( "Remove \"%1\"?" ).arg( entry->name ) ) != QMessageBox::Yes ) {
			return;
		}

		for ( int i = 0; i < m_state.installations.size(); ++i )
		{
			if ( m_state.installations[i].id == id ) {
				m_state.installations.removeAt( i );
				break;
			}
		}
		if ( m_state.selectedInstallByGame.value( gameFile ) == id ) {
			m_state.selectedInstallByGame.remove( gameFile );
		}
		m_state.selectedGameNameByInstall.remove( id );
		if ( m_selectedInstallationId == id ) {
			m_selectedInstallationId.clear();
		}
		refreshGameList();
		selectGameInList( gameFile );
		refreshInstallList();
	}

	void acceptSelectedInstallation(){
		const QString gameFile = selectedGameFileFromList();
		const QString installId = selectedInstallIdFromList();
		if ( gameFile.isEmpty() || installId.isEmpty() ) {
			return;
		}
		if ( !confirmSupportedModSelectionForSelectedInstallation() ) {
			return;
		}
		m_selectedGameFile = gameFile;
		m_selectedInstallationId = installId;
		m_state.selectedGameFile = gameFile;
		m_state.selectedInstallByGame[gameFile] = installId;
		if ( const CGameDescription* game = find_game_by_file( m_games, gameFile ); game != nullptr ) {
			if ( const GameInstallationEntry* install = find_installation_by_id( m_state, installId ); install != nullptr ) {
				m_state.selectedGameNameByInstall[installId] = choose_supported_game_name_for_installation(
					*game,
					install->path,
					m_state.selectedGameNameByInstall.value( installId ) );
			}
		}
		accept();
	}

	void selectGameInList( const QString& gameFile ){
		for ( int i = 0; i < m_gameList->count(); ++i )
		{
			QListWidgetItem* item = m_gameList->item( i );
			if ( item != nullptr && item->data( Qt::UserRole ).toString() == gameFile ) {
				m_gameList->setCurrentItem( item );
				return;
			}
		}
	}
};

void CGameDialog::LoadPrefs(){
	// load global .pref file
	const auto strGlobalPref = StringStream( g_Preferences.m_global_rc_path, PREFS_GLOBAL_FILENAME );

	globalOutputStream() << "loading global preferences from " << Quoted( strGlobalPref ) << '\n';

	if ( !Preferences_Load( g_global_preferences, strGlobalPref, "global" ) ) {
		globalOutputStream() << "failed to load global preferences from " << strGlobalPref << '\n';
	}
}

void CGameDialog::SavePrefs(){
	const auto strGlobalPref = StringStream( g_Preferences.m_global_rc_path, PREFS_GLOBAL_FILENAME );

	globalOutputStream() << "saving global preferences to " << strGlobalPref << '\n';

	if ( !Preferences_Save_Safe( g_global_preferences, strGlobalPref ) ) {
		globalOutputStream() << "failed to save global preferences to " << strGlobalPref << '\n';
	}
}

void CGameDialog::DoGameDialog(){
	// show the UI
	DoModal();

	// we save the prefs file
	SavePrefs();
}

CGameDescription* CGameDialog::GameDescriptionForComboItem(){
	return ( m_nComboSelect >= 0 && m_nComboSelect < static_cast<int>( mGames.size() ) )?
	       *std::next( mGames.begin(), m_nComboSelect )
	       : 0; // not found
}

void CGameDialog::GameFileAssign( int value ){
	m_nComboSelect = value;
	// use value to set m_sGameFile
	if( CGameDescription* iGame = GameDescriptionForComboItem() )
		m_sGameFile.assign( iGame->mGameFile );
}

void CGameDialog::GameFileImport( int value ){
	m_nComboSelect = value;
	// use value to set m_sGameFile
	if( CGameDescription* iGame = GameDescriptionForComboItem() )
		m_sGameFile.import( iGame->mGameFile );
}

void CGameDialog::GameFileExport( const IntImportCallback& importCallback ) const {
	// use m_sGameFile to set value
	if( const auto found = std::ranges::find( mGames, m_sGameFile.m_latched, &CGameDescription::mGameFile ); found != mGames.cend() )
		m_nComboSelect = std::distance( mGames.cbegin(), found );
	importCallback( m_nComboSelect );
}

void CGameDialog::CreateGlobalFrame( PreferencesPage& page, bool global ){
	std::vector<const char*> games;
	games.reserve( mGames.size() );
	for ( const auto *game : mGames )
	{
		games.push_back( game->getRequiredKeyValue( "name" ) );
	}
	page.appendCombo(
	    "Select the game",
	    StringArrayRange( games ),
	    global?
	    IntImportCallback( MemberCaller<CGameDialog, void(int), &CGameDialog::GameFileAssign>( *this ) ):
	    IntImportCallback( MemberCaller<CGameDialog, void(int), &CGameDialog::GameFileImport>( *this ) ),
	    ConstMemberCaller<CGameDialog, void(const IntImportCallback&), &CGameDialog::GameFileExport>( *this )
	);
	page.appendCheckBox( "Startup", "Show Global Preferences", m_bGamePrompt );
	QPushButton* manageInstallations = page.appendButton( "Installations", "Manage Installations..." );
	QObject::connect( manageInstallations, &QPushButton::clicked, [this, global](){
		QVector<const CGameDescription*> gamesVector;
		gamesVector.reserve( static_cast<int>( mGames.size() ) );
		for ( const CGameDescription* game : mGames )
		{
			gamesVector.push_back( game );
		}

		QString stateError;
		GameInstallationState installState = load_game_installation_state( &stateError );
		if ( !stateError.isEmpty() ) {
			QMessageBox::warning( g_Preferences.GetWidget(), "Installations", stateError );
			return;
		}

		GameSetupManagerDialog dialog(
			gamesVector,
			installState,
			QString::fromUtf8( m_sGameFile.m_latched.c_str() ),
			g_Preferences.GetWidget() );
		if ( dialog.exec() != QDialog::Accepted ) {
			return;
		}

		installState = dialog.state();
		const QString selectedGameFile = dialog.selectedGameFile();
		const QString selectedInstallId = dialog.selectedInstallationId();
		if ( !selectedGameFile.isEmpty() ) {
			installState.selectedGameFile = selectedGameFile;
		}
		if ( !selectedInstallId.isEmpty() && !selectedGameFile.isEmpty() ) {
			installState.selectedInstallByGame[selectedGameFile] = selectedInstallId;
		}

		QString saveError;
		if ( !save_game_installation_state( installState, &saveError ) && !saveError.isEmpty() ) {
			QMessageBox::warning( g_Preferences.GetWidget(), "Installations", saveError );
		}

		if ( !selectedGameFile.isEmpty() ) {
			const QByteArray gameFileUtf8 = selectedGameFile.toUtf8();
			if ( global ) {
				m_sGameFile.assign( gameFileUtf8.constData() );
			}
			else{
				m_sGameFile.import( gameFileUtf8.constData() );
			}
		}
	} );
}

void CGameDialog::BuildDialog(){
	GetWidget()->setWindowTitle( i18n::tr( "Global Preferences" ) );

	auto *vbox = new QVBoxLayout( GetWidget() );
	vbox->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
	{
		auto *frame = new QGroupBox( i18n::tr( "Game Settings" ) );
		vbox->addWidget( frame );

		auto *grid = new QGridLayout( frame );
		grid->setAlignment( Qt::AlignmentFlag::AlignTop );
		grid->setColumnStretch( 0, 111 );
		grid->setColumnStretch( 1, 333 );
		{
			PreferencesPage preferencesPage( *this, grid );
			Global_constructPreferences( preferencesPage, true );
			CreateGlobalFrame( preferencesPage, true );
		}
	}
	{
		auto *buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok );
		vbox->addWidget( buttons );
		QObject::connect( buttons, &QDialogButtonBox::accepted, GetWidget(), &QDialog::accept );
	}
}

void CGameDialog::ScanForGames(){
	const auto path = StringStream( AppPath_get(), "gamepacks/games/" );

	globalOutputStream() << "Scanning for game description files: " << path << '\n';

	if ( !file_is_directory( path.c_str() ) ) {
		globalWarningStream() << "Game descriptions directory not found: " << path << '\n';
		return;
	}

	/*!
	   \todo FIXME LINUX:
	   do we put game description files below AppPath, or in ~/.radiant
	   i.e. read only or read/write?
	   my guess .. readonly cause it's an install
	   we will probably want to add ~/.radiant/<version>/games/ scanning on top of that for developers
	   (if that's really needed)
	 */

	Directory_forEach( path, matchFileExtension( "game", [&]( const char *name ){
		const auto strPath = StringStream( path, name );
		globalOutputStream() << strPath << '\n';

		xmlDocPtr pDoc = xmlParseFile( strPath );
		if ( pDoc ) {
			mGames.push_back( new CGameDescription( pDoc, name ) );
			xmlFreeDoc( pDoc );
		}
		else
		{
			globalErrorStream() << "XML parser failed on " << SingleQuoted( strPath ) << '\n';
		}
	}));
}

void CGameDialog::InitGlobalPrefPath(){
	g_Preferences.m_global_rc_path = SettingsPath_get();
}

void CGameDialog::Reset(){
	if ( g_Preferences.m_global_rc_path.empty() ) {
		InitGlobalPrefPath();
	}

	file_remove( StringStream( g_Preferences.m_global_rc_path, PREFS_GLOBAL_FILENAME ) );
	const QString statePath = game_install_state_file_path();
	if ( !statePath.isEmpty() ) {
		const QByteArray statePathUtf8 = statePath.toUtf8();
		file_remove( statePathUtf8.constData() );
	}
}

void CGameDialog::Init(){
	InitGlobalPrefPath();
	LoadPrefs();
	theme_construct(); // after global prefs, b4 any normal windows
	ScanForGames();
	if ( mGames.empty() ) {
		const auto path = StringStream( AppPath_get(), "gamepacks/games/" );
		Error( "Didn't find any valid game file descriptions in %s\n"
		       "Install runtime data and gamepacks into %sgamepacks/ (for example via make install-data)\n",
		       path.c_str(), AppPath_get() );
	}
	else
	{
		mGames.sort( []( const CGameDescription* one, const CGameDescription* another ){
			return one->mGameFile < another->mGameFile;
		} );
	}

	QVector<const CGameDescription*> games;
	games.reserve( static_cast<int>( mGames.size() ) );
	for ( const CGameDescription* game : mGames )
	{
		games.push_back( game );
	}

	QString stateError;
	GameInstallationState installState = load_game_installation_state( &stateError );
	if ( !stateError.isEmpty() ) {
		globalWarningStream() << stateError.toUtf8().constData() << '\n';
	}

	const auto gameExists = [&games]( const QString& gameFile ){
		return find_game_by_file( games, gameFile ) != nullptr;
	};

	// Drop stale installation entries whose game no longer exists or whose path vanished.
	for ( int i = installState.installations.size() - 1; i >= 0; --i )
	{
		const GameInstallationEntry& entry = installState.installations[i];
		if ( !gameExists( entry.gameFile ) ) {
			installState.installations.removeAt( i );
			continue;
		}
		const QFileInfo pathInfo( entry.path );
		if ( !pathInfo.exists() || !pathInfo.isDir() ) {
			installState.installations.removeAt( i );
		}
	}
	for ( auto it = installState.selectedInstallByGame.begin(); it != installState.selectedInstallByGame.end(); )
	{
		const GameInstallationEntry* selected = find_installation_by_id( installState, it.value() );
		if ( selected == nullptr || selected->gameFile != it.key() ) {
			it = installState.selectedInstallByGame.erase( it );
			continue;
		}
		++it;
	}
	for ( auto it = installState.selectedGameNameByInstall.begin(); it != installState.selectedGameNameByInstall.end(); )
	{
		if ( find_installation_by_id( installState, it.key() ) == nullptr ) {
			it = installState.selectedGameNameByInstall.erase( it );
			continue;
		}
		it.value() = normalise_game_name_token( it.value() );
		++it;
	}

	const QString prefGameFile = QString::fromUtf8( m_sGameFile.m_value.c_str() );
	const auto resolveSelection =
		[&]( const QString& preferredGameFile,
		     const QString& preferredInstallId,
		     QString* outGameFile,
		     QString* outInstallId ) -> bool {
			const auto tryGame = [&]( const QString& gameFile, const QString& installId )->bool {
				if ( gameFile.isEmpty() || !gameExists( gameFile ) ) {
					return false;
				}
				const QVector<int> indexes = installation_indexes_for_game( installState, gameFile );
				if ( indexes.isEmpty() ) {
					return false;
				}

				QString selectedId = installId;
				if ( selectedId.isEmpty() ) {
					selectedId = installState.selectedInstallByGame.value( gameFile );
				}
				const GameInstallationEntry* selected = find_installation_by_id( installState, selectedId );
				if ( selected == nullptr || selected->gameFile != gameFile ) {
					selected = &installState.installations[indexes.first()];
				}

				if ( outGameFile != nullptr ) {
					*outGameFile = gameFile;
				}
				if ( outInstallId != nullptr ) {
					*outInstallId = selected->id;
				}
				return true;
			};

			if ( tryGame( preferredGameFile, preferredInstallId ) ) {
				return true;
			}
			if ( tryGame( installState.selectedGameFile, installState.selectedInstallByGame.value( installState.selectedGameFile ) ) ) {
				return true;
			}
			if ( tryGame( prefGameFile, installState.selectedInstallByGame.value( prefGameFile ) ) ) {
				return true;
			}
			for ( const GameInstallationEntry& entry : installState.installations )
			{
				if ( tryGame( entry.gameFile, entry.id ) ) {
					return true;
				}
			}
			return false;
		};

	QString selectedGameFile = prefGameFile;
	QString selectedInstallId = installState.selectedInstallByGame.value( selectedGameFile );
	bool hasSelection = resolveSelection( selectedGameFile, selectedInstallId, &selectedGameFile, &selectedInstallId );
	bool userCancelledSetup = false;

	if ( m_bGamePrompt || !hasSelection ) {
		while ( true )
		{
			GameSetupManagerDialog dialog( games, installState, selectedGameFile );
			const int code = dialog.exec();
			if ( code == QDialog::Accepted ) {
				installState = dialog.state();
				selectedGameFile = dialog.selectedGameFile();
				selectedInstallId = dialog.selectedInstallationId();
				hasSelection = resolveSelection( selectedGameFile, selectedInstallId, &selectedGameFile, &selectedInstallId );
				if ( hasSelection ) {
					break;
				}
			}
			else{
				userCancelledSetup = true;
				break;
			}
		}
	}

	if ( userCancelledSetup ) {
		std::exit( EXIT_SUCCESS );
	}

	if ( !hasSelection ) {
		Error( "No game installation configured. Configure at least one installation to continue.\n" );
	}

	const CGameDescription* selectedGame = find_game_by_file( games, selectedGameFile );
	if ( selectedGame == nullptr ) {
		Error( "Selected game profile '%s' is no longer available.\n", selectedGameFile.toUtf8().constData() );
	}
	const GameInstallationEntry* selectedInstall = find_installation_by_id( installState, selectedInstallId );
	if ( selectedInstall == nullptr || selectedInstall->gameFile != selectedGameFile ) {
		const QVector<int> indexes = installation_indexes_for_game( installState, selectedGameFile );
		if ( indexes.isEmpty() ) {
			Error( "No installation configured for selected game profile '%s'.\n", selectedGameFile.toUtf8().constData() );
		}
		selectedInstall = &installState.installations[indexes.first()];
		selectedInstallId = selectedInstall->id;
	}

	installState.selectedGameFile = selectedGameFile;
	installState.selectedInstallByGame[selectedGameFile] = selectedInstallId;
	const QString selectedGameName = choose_supported_game_name_for_installation(
		*selectedGame,
		selectedInstall->path,
		installState.selectedGameNameByInstall.value( selectedInstallId ) );
	if ( !selectedGameName.isEmpty() ) {
		installState.selectedGameNameByInstall[selectedInstallId] = selectedGameName;
	}

	QString saveStateError;
	if ( !save_game_installation_state( installState, &saveStateError ) && !saveStateError.isEmpty() ) {
		globalWarningStream() << saveStateError.toUtf8().constData() << '\n';
	}

	m_sGameFile.assign( selectedGameFile.toUtf8().constData() );
	SavePrefs();

	g_startupGameInstallationPath = selectedInstall->path.toUtf8().constData();
	g_startupGameInstallationEngineExecutable = selectedInstall->engineExecutable.toUtf8().constData();
	g_startupGameInstallationId = selectedInstall->id.toUtf8().constData();
	g_startupGameInstallationGameName = selectedGameName.toUtf8().constData();

	g_pGameDescription = const_cast<CGameDescription*>( selectedGame );
	g_pGameDescription->Dump();
}

CGameDialog::~CGameDialog(){
	// free all the game descriptions
	for ( auto& game : mGames )
	{
		delete std::exchange( game, nullptr );
	}
	if ( GetWidget() != 0 ) {
		Destroy();
	}
}

inline const char* GameDescription_getIdentifier( const CGameDescription& gameDescription ){
	const char* identifier = gameDescription.getKeyValue( "index" );
	if ( string_empty( identifier ) ) {
		identifier = "1";
	}
	return identifier;
}

void CGameDialog::AddPacksURL( StringOutputStream &URL ){
	// add the URLs for the list of game packs installed
	// FIXME: this is kinda hardcoded for now..
	for ( const CGameDescription *iGame : mGames )
	{
		URL << "&Games_dlup%5B%5D=" << GameDescription_getIdentifier( *iGame );
	}
}

CGameDialog g_GamesDialog;


// =============================================================================
// Widget callbacks for PrefsDlg

static void OnButtonClean( PrefsDlg *dlg ){
	// make sure this is what the user wants
	if ( qt_MessageBox( g_Preferences.GetWidget(),
	                     "This will close VibeRadiant and clean the corresponding registry entries.\n"
	                     "Next time you start VibeRadiant it will be good as new. Do you wish to continue?",
	                     "Reset Registry", EMessageBoxType::Warning, eIDYES | eIDNO ) == eIDYES ) {
		dlg->EndModal( QDialog::DialogCode::Rejected );

		g_preferences_globals.disable_ini = true;
		Preferences_Reset();
		QCoreApplication::quit();
	}
}

// =============================================================================
// PrefsDlg class

/*
   ========

   very first prefs init deals with selecting the game and the game tools path
   then we can load .ini stuff

   using prefs / ini settings:
   those are per-game

   look in ~/.radiant/<version>/gamename
   ========
 */

constexpr const char* PREFS_LOCAL_FILENAME = "local.pref";

void PrefsDlg::Init(){
	// m_global_rc_path has been set above
	// m_rc_path is for game specific preferences
	// takes the form: global-pref-path/gamename/prefs-file

	// this is common to win32 and Linux init now
	// game sub-dir
	m_rc_path = StringStream( m_global_rc_path, g_pGameDescription->mGameFile.c_str(), '/' );
	Q_mkdir( m_rc_path.c_str() );

	// then the ini file
	m_inipath = StringStream( m_rc_path, PREFS_LOCAL_FILENAME );
}


typedef std::list<PreferenceGroupCallback> PreferenceGroupCallbacks;

inline void PreferenceGroupCallbacks_constructGroup( const PreferenceGroupCallbacks& callbacks, PreferenceGroup& group ){
	for ( const auto& cb : callbacks )
	{
		cb( group );
	}
}


inline void PreferenceGroupCallbacks_pushBack( PreferenceGroupCallbacks& callbacks, const PreferenceGroupCallback& callback ){
	callbacks.push_back( callback );
}

typedef std::list<PreferencesPageCallback> PreferencesPageCallbacks;

inline void PreferencesPageCallbacks_constructPage( const PreferencesPageCallbacks& callbacks, PreferencesPage& page ){
	for ( const auto& cb : callbacks )
	{
		cb( page );
	}
}

inline void PreferencesPageCallbacks_pushBack( PreferencesPageCallbacks& callbacks, const PreferencesPageCallback& callback ){
	callbacks.push_back( callback );
}

PreferencesPageCallbacks g_gamePreferences;
void PreferencesDialog_addGamePreferences( const PreferencesPageCallback& callback ){
	PreferencesPageCallbacks_pushBack( g_gamePreferences, callback );
}
PreferenceGroupCallbacks g_gameCallbacks;
void PreferencesDialog_addGamePage( const PreferenceGroupCallback& callback ){
	PreferenceGroupCallbacks_pushBack( g_gameCallbacks, callback );
}

PreferencesPageCallbacks g_interfacePreferences;
void PreferencesDialog_addInterfacePreferences( const PreferencesPageCallback& callback ){
	PreferencesPageCallbacks_pushBack( g_interfacePreferences, callback );
}
PreferenceGroupCallbacks g_interfaceCallbacks;
void PreferencesDialog_addInterfacePage( const PreferenceGroupCallback& callback ){
	PreferenceGroupCallbacks_pushBack( g_interfaceCallbacks, callback );
}

PreferencesPageCallbacks g_displayPreferences;
void PreferencesDialog_addDisplayPreferences( const PreferencesPageCallback& callback ){
	PreferencesPageCallbacks_pushBack( g_displayPreferences, callback );
}
PreferenceGroupCallbacks g_displayCallbacks;
void PreferencesDialog_addDisplayPage( const PreferenceGroupCallback& callback ){
	PreferenceGroupCallbacks_pushBack( g_displayCallbacks, callback );
}

PreferencesPageCallbacks g_settingsPreferences;
void PreferencesDialog_addSettingsPreferences( const PreferencesPageCallback& callback ){
	PreferencesPageCallbacks_pushBack( g_settingsPreferences, callback );
}
PreferenceGroupCallbacks g_settingsCallbacks;
void PreferencesDialog_addSettingsPage( const PreferenceGroupCallback& callback ){
	PreferenceGroupCallbacks_pushBack( g_settingsCallbacks, callback );
}

PreferencesPageCallbacks g_genAIPreferences;
void PreferencesDialog_addGenAIPreferences( const PreferencesPageCallback& callback ){
	PreferencesPageCallbacks_pushBack( g_genAIPreferences, callback );
}
PreferenceGroupCallbacks g_genAICallbacks;
void PreferencesDialog_addGenAIPage( const PreferenceGroupCallback& callback ){
	PreferenceGroupCallbacks_pushBack( g_genAICallbacks, callback );
}

void Widget_connectToggleDependency( QWidget* self, QCheckBox* toggleButton ){
	class EnabledTracker : public QObject
	{
		QCheckBox *const m_checkbox;
		QWidget *const m_dependent;
	public:
		EnabledTracker( QCheckBox *checkbox, QWidget *dependent ) : QObject( checkbox ), m_checkbox( checkbox ), m_dependent( dependent ){
			m_checkbox->installEventFilter( this );
		}
	protected:
		bool eventFilter( QObject *obj, QEvent *event ) override {
			if( event->type() == QEvent::EnabledChange ) {
				m_dependent->setEnabled( m_checkbox->checkState() && m_checkbox->isEnabled() );
			}
			return QObject::eventFilter( obj, event ); // standard event processing
		}
	};
	new EnabledTracker( toggleButton, self ); // track graying out for chained dependencies
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
	QObject::connect( toggleButton, &QCheckBox::checkStateChanged, [self, toggleButton]( Qt::CheckState state ){ // track being checked
		self->setEnabled( state != Qt::CheckState::Unchecked && toggleButton->isEnabled() );
	} );
#else
	QObject::connect( toggleButton, &QCheckBox::stateChanged, [self, toggleButton]( int state ){ // track being checked
		self->setEnabled( state && toggleButton->isEnabled() );
	} );
#endif
	self->setEnabled( toggleButton->checkState() && toggleButton->isEnabled() ); // apply dependency effect right away
}
void Widget_connectToggleDependency( QCheckBox* self, QCheckBox* toggleButton ){
	Widget_connectToggleDependency( static_cast<QWidget*>( self ), toggleButton );
}


QStandardItem* PreferenceTree_appendPage( QStandardItemModel* model, QStandardItem* parent, const char* name, int pageIndex ){
	auto *item = new QStandardItem( i18n::tr( name ) );
	item->setData( pageIndex, Qt::ItemDataRole::UserRole );
	parent->appendRow( item );
	return item;
}

auto PreferencePages_addPage( QStackedWidget* notebook, const char* name ){
	auto *scroll = new QScrollArea( notebook );
	scroll->setWidgetResizable( true );
	scroll->setFrameShape( QFrame::NoFrame );
	scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );
	scroll->setVerticalScrollBarPolicy( Qt::ScrollBarAsNeeded );

	auto *frame = new QGroupBox( i18n::tr( name ) );
	frame->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
	auto *grid = new QGridLayout( frame );
	grid->setAlignment( Qt::AlignmentFlag::AlignTop );
	grid->setColumnStretch( 0, 111 );
	grid->setColumnStretch( 1, 333 );

	scroll->setWidget( frame );
	return std::pair( notebook->addWidget( scroll ), grid );
}

struct PreferenceSearchEntry
{
	int pageIndex;
	QString pagePath;
	QString settingLabel;
	QString haystack;
};

void PreferenceTree_collectPagePaths( const QStandardItem* item, const QString& prefix, std::map<int, QString>& pagePaths ){
	for ( int row = 0; row < item->rowCount(); ++row )
	{
		const QStandardItem *const child = item->child( row, 0 );
		if ( child == nullptr ) {
			continue;
		}

		const QString name = child->text();
		const QString path = prefix.isEmpty() ? name : QString( "%1 > %2" ).arg( prefix, name );
		pagePaths[child->data( Qt::ItemDataRole::UserRole ).toInt()] = path;
		PreferenceTree_collectPagePaths( child, path, pagePaths );
	}
}

void PreferenceSearch_collectTexts( QWidget* page, std::map<QString, QString>& labels ){
	for ( auto *label : page->findChildren<QLabel*>() )
	{
		const QString text = label->text().trimmed();
		if ( !text.isEmpty() )
			labels[text.toLower()] = text;
	}
	for ( auto *checkbox : page->findChildren<QCheckBox*>() )
	{
		const QString text = checkbox->text().trimmed();
		if ( !text.isEmpty() )
			labels[text.toLower()] = text;
	}
	for ( auto *button : page->findChildren<QPushButton*>() )
	{
		const QString text = button->text().trimmed();
		if ( !text.isEmpty() )
			labels[text.toLower()] = text;
	}
	for ( auto *radioButton : page->findChildren<QRadioButton*>() )
	{
		const QString text = radioButton->text().trimmed();
		if ( !text.isEmpty() )
			labels[text.toLower()] = text;
	}
	for ( auto *combo : page->findChildren<QComboBox*>() )
	{
		for ( int i = 0; i < combo->count(); ++i )
		{
			const QString text = combo->itemText( i ).trimmed();
			if ( !text.isEmpty() )
				labels[text.toLower()] = text;
		}
	}
}

std::vector<PreferenceSearchEntry> PreferenceSearch_buildEntries( QStackedWidget* notebook, const std::map<int, QString>& pagePaths ){
	std::vector<PreferenceSearchEntry> entries;
	entries.reserve( notebook->count() * 8 );
	for ( int pageIndex = 0; pageIndex < notebook->count(); ++pageIndex )
	{
		QWidget *const page = notebook->widget( pageIndex );
		const auto pagePath = pagePaths.find( pageIndex );
		if ( page == nullptr || pagePath == pagePaths.end() ) {
			continue;
		}

		std::map<QString, QString> labels;
		PreferenceSearch_collectTexts( page, labels );
		for ( const auto& it : labels )
		{
			const QString& label = it.second;
			PreferenceSearchEntry entry{
				pageIndex,
				pagePath->second,
				label,
				( pagePath->second + ' ' + label ).toLower()
			};
			entries.push_back( std::move( entry ) );
		}
	}
	return entries;
}

QModelIndex PreferenceTree_findPageIndex( const QAbstractItemModel* model, int pageIndex, const QModelIndex& parent = QModelIndex() ){
	for ( int row = 0; row < model->rowCount( parent ); ++row )
	{
		const QModelIndex index = model->index( row, 0, parent );
		if ( !index.isValid() ) {
			continue;
		}
		if ( index.data( Qt::ItemDataRole::UserRole ).toInt() == pageIndex )
			return index;

		const QModelIndex childResult = PreferenceTree_findPageIndex( model, pageIndex, index );
		if ( childResult.isValid() )
			return childResult;
	}
	return QModelIndex();
}

QStringList PreferenceSearch_terms( const QString& raw ){
	QStringList terms;
	for ( const QString& token : raw.simplified().toLower().split( ' ', Qt::SplitBehaviorFlags::SkipEmptyParts ) )
	{
		terms.push_back( token );
	}
	return terms;
}

bool PreferenceSearch_matches( const QString& haystack, const QStringList& terms ){
	for ( const auto& term : terms )
	{
		if ( !haystack.contains( term ) )
			return false;
	}
	return true;
}

class PreferenceTreeGroup : public PreferenceGroup
{
	Dialog& m_dialog;
	QStackedWidget* m_notebook;
	QStandardItemModel* m_model;
	QStandardItem *m_group;
public:
	PreferenceTreeGroup( Dialog& dialog, QStackedWidget* notebook, QStandardItemModel* model, QStandardItem *group ) :
		m_dialog( dialog ),
		m_notebook( notebook ),
		m_model( model ),
		m_group( group ){
	}
	PreferencesPage createPage( const char* treeName, const char* frameName ) override {
		const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, frameName );
		PreferenceTree_appendPage( m_model, m_group, treeName, pageIndex );
		return PreferencesPage( m_dialog, layout );
	}
};

void PrefsDlg::BuildDialog(){
	static bool preferencesCallbacksRegistered = false;
	if ( !preferencesCallbacksRegistered ) {
		preferencesCallbacksRegistered = true;
		PreferencesDialog_addInterfacePreferences( makeCallbackF( Interface_constructPreferences ) );
		PreferencesDialog_addInterfacePreferences( makeCallbackF( theme_construct_preferences ) );
	}

	GetWidget()->setWindowTitle( i18n::tr( "VibeRadiant Preferences" ) );

	{
		auto *grid = new QGridLayout( GetWidget() );
		grid->setSizeConstraint( QLayout::SizeConstraint::SetMinimumSize );
		grid->setContentsMargins( 12, 12, 12, 12 );
		grid->setHorizontalSpacing( 12 );
		grid->setVerticalSpacing( 10 );
		{
			m_searchEdit = new QLineEdit;
			m_searchEdit->setPlaceholderText( i18n::tr( "Search settings..." ) );
			m_searchEdit->setClearButtonEnabled( true );
			grid->addWidget( m_searchEdit, 0, 0 );
		}

		auto *splitter = new QSplitter( Qt::Orientation::Horizontal );
		splitter->setChildrenCollapsible( false );
		grid->addWidget( splitter, 1, 0 );

		auto *sidebar = new QWidget;
		auto *sidebarLayout = new QVBoxLayout( sidebar );
		sidebarLayout->setContentsMargins( 0, 0, 0, 0 );
		sidebarLayout->setSpacing( 8 );
		splitter->addWidget( sidebar );

		m_treeview = new QTreeView;
		m_treeview->setHeaderHidden( true );
		m_treeview->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
		m_treeview->setUniformRowHeights( true );
		m_treeview->setAlternatingRowColors( true );
		m_treeview->setHorizontalScrollBarPolicy( Qt::ScrollBarPolicy::ScrollBarAlwaysOff );
		m_treeview->setSizeAdjustPolicy( QAbstractScrollArea::SizeAdjustPolicy::AdjustToContents );
		m_treeview->header()->setStretchLastSection( false );
		m_treeview->header()->setSectionResizeMode( QHeaderView::ResizeMode::ResizeToContents );
		sidebarLayout->addWidget( m_treeview );

		m_searchResults = new QListWidget;
		m_searchResults->setVisible( false );
		m_searchResults->setAlternatingRowColors( true );
		m_searchResults->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
		sidebarLayout->addWidget( m_searchResults );

		m_notebook = new QStackedWidget;
		splitter->addWidget( m_notebook );
		splitter->setStretchFactor( 0, 0 );
		splitter->setStretchFactor( 1, 1 );
		splitter->setSizes( { 260, 760 } );

		{
			auto *buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
			grid->addWidget( buttons, 2, 0 );
			QObject::connect( buttons, &QDialogButtonBox::accepted, GetWidget(), &QDialog::accept );
			QObject::connect( buttons, &QDialogButtonBox::rejected, GetWidget(), &QDialog::reject );
			QObject::connect( buttons->addButton( i18n::tr( "Clean" ), QDialogButtonBox::ButtonRole::ResetRole ), &QPushButton::clicked, [this](){ OnButtonClean( this ); } );
		}

		// store display name in column #0 and page index in data( Qt::ItemDataRole::UserRole )
		m_treeModel = new QStandardItemModel( m_treeview );
		m_treeview->setModel( m_treeModel );

		{
			/********************************************************************/
			/* Add preference tree options                                      */
			/********************************************************************/
			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "Global Preferences" );
				{
					PreferencesPage preferencesPage( *this, layout );
					Global_constructPreferences( preferencesPage, false );
				}
				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "Global", pageIndex );
				{
					const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "Game" );
					PreferencesPage preferencesPage( *this, layout );
					g_GamesDialog.CreateGlobalFrame( preferencesPage, false );

					PreferenceTree_appendPage( m_treeModel, group, "Game", pageIndex );
				}
			}

			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "Game Settings" );
				{
					PreferencesPage preferencesPage( *this, layout );
					Game_constructPreferences( preferencesPage );
					PreferencesPageCallbacks_constructPage( g_gamePreferences, preferencesPage );
				}

				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "Game", pageIndex );
				PreferenceTreeGroup preferenceGroup( *this, m_notebook, m_treeModel, group );

				PreferenceGroupCallbacks_constructGroup( g_gameCallbacks, preferenceGroup );
			}

			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "Interface Preferences" );
				{
					PreferencesPage preferencesPage( *this, layout );
					PreferencesPageCallbacks_constructPage( g_interfacePreferences, preferencesPage );
				}

				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "Interface", pageIndex );
				PreferenceTreeGroup preferenceGroup( *this, m_notebook, m_treeModel, group );

				PreferenceGroupCallbacks_constructGroup( g_interfaceCallbacks, preferenceGroup );
			}

			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "Display Preferences" );
				{
					PreferencesPage preferencesPage( *this, layout );
					PreferencesPageCallbacks_constructPage( g_displayPreferences, preferencesPage );
				}
				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "Display", pageIndex );
				PreferenceTreeGroup preferenceGroup( *this, m_notebook, m_treeModel, group );

				PreferenceGroupCallbacks_constructGroup( g_displayCallbacks, preferenceGroup );
			}

			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "General Settings" );
				{
					PreferencesPage preferencesPage( *this, layout );
					PreferencesPageCallbacks_constructPage( g_settingsPreferences, preferencesPage );
				}

				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "Settings", pageIndex );
				PreferenceTreeGroup preferenceGroup( *this, m_notebook, m_treeModel, group );

				PreferenceGroupCallbacks_constructGroup( g_settingsCallbacks, preferenceGroup );
			}

			{
				const auto [ pageIndex, layout ] = PreferencePages_addPage( m_notebook, "GenAI Settings" );
				{
					PreferencesPage preferencesPage( *this, layout );
					PreferencesPageCallbacks_constructPage( g_genAIPreferences, preferencesPage );
				}

				QStandardItem *group = PreferenceTree_appendPage( m_treeModel, m_treeModel->invisibleRootItem(), "GenAI", pageIndex );
				PreferenceTreeGroup preferenceGroup( *this, m_notebook, m_treeModel, group );

				PreferenceGroupCallbacks_constructGroup( g_genAICallbacks, preferenceGroup );
			}
		}

		QObject::connect( m_treeview->selectionModel(), &QItemSelectionModel::currentChanged, [this]( const QModelIndex& current ){
			if ( !current.isValid() ) {
				return;
			}
			m_notebook->setCurrentIndex( current.data( Qt::ItemDataRole::UserRole ).toInt() );
		} );

		m_treeview->expandAll();
		m_treeview->setCurrentIndex( m_treeview->model()->index( 0, 0 ) );

		std::map<int, QString> pagePaths;
		PreferenceTree_collectPagePaths( m_treeModel->invisibleRootItem(), QString(), pagePaths );
		auto searchEntries = PreferenceSearch_buildEntries( m_notebook, pagePaths );

		auto updateSearchResults = [this, searchEntries = std::move( searchEntries )]( const QString& text ){
			const QString trimmed = text.trimmed();
			if ( trimmed.isEmpty() ) {
				m_searchResults->clear();
				m_searchResults->setVisible( false );
				m_treeview->setVisible( true );
				return;
			}

			const auto terms = PreferenceSearch_terms( trimmed );
			m_searchResults->clear();
			m_treeview->setVisible( false );
			m_searchResults->setVisible( true );

			int matches = 0;
			for ( const auto& entry : searchEntries )
			{
				if ( !PreferenceSearch_matches( entry.haystack, terms ) ) {
					continue;
				}
				auto *item = new QListWidgetItem( QString( "%1: %2" ).arg( entry.pagePath, entry.settingLabel ) );
				item->setData( Qt::ItemDataRole::UserRole, entry.pageIndex );
				m_searchResults->addItem( item );
				++matches;
			}

			if ( matches == 0 ) {
				auto *empty = new QListWidgetItem( i18n::tr( "No matching settings" ) );
				empty->setFlags( empty->flags() & ~Qt::ItemFlag::ItemIsSelectable );
				m_searchResults->addItem( empty );
			}
		};

		auto activateResult = [this]( QListWidgetItem* item ){
			if ( item == nullptr ) {
				return;
			}
			const QVariant data = item->data( Qt::ItemDataRole::UserRole );
			if ( !data.isValid() ) {
				return;
			}

			const int pageIndex = data.toInt();
			m_notebook->setCurrentIndex( pageIndex );
			const QModelIndex index = PreferenceTree_findPageIndex( m_treeModel, pageIndex );
			if ( index.isValid() ) {
				m_treeview->selectionModel()->setCurrentIndex( index, QItemSelectionModel::SelectionFlag::ClearAndSelect );
				m_treeview->scrollTo( index, QAbstractItemView::ScrollHint::PositionAtCenter );
			}
		};

		QObject::connect( m_searchEdit, &QLineEdit::textChanged, updateSearchResults );
		QObject::connect( m_searchResults, &QListWidget::itemActivated, activateResult );
		QObject::connect( m_searchEdit, &QLineEdit::returnPressed, [this, activateResult](){
			if ( m_searchResults->isVisible() && m_searchResults->count() > 0 ) {
				if ( QListWidgetItem* item = m_searchResults->item( 0 ) )
					activateResult( item );
			}
		} );
	}

	QScreen* screen = GetWidget()->screen();
	if ( screen == nullptr ) {
		screen = QGuiApplication::screenAt( QCursor::pos() );
	}
	if ( screen == nullptr ) {
		screen = QGuiApplication::primaryScreen();
	}

	const QRect available = ( screen != nullptr )
		? screen->availableGeometry()
		: QRect( 0, 0, 1280, 800 );
	const int maxWidth = std::max( 640, static_cast<int>( available.width() * 0.95 ) );
	const int maxHeight = std::max( 480, static_cast<int>( available.height() * 0.95 ) );
	const int minWidth = std::min( 720, maxWidth );
	const int minHeight = std::min( 540, maxHeight );

	GetWidget()->setMinimumSize( minWidth, minHeight );
	GetWidget()->setMaximumSize( maxWidth, maxHeight );
	GetWidget()->resize( std::min( 900, maxWidth ), std::min( 700, maxHeight ) );
}

preferences_globals_t g_preferences_globals;

PrefsDlg g_Preferences;               // global prefs instance


void PreferencesDialog_constructWindow( QWidget* main_window ){
	g_Preferences.Create( main_window );
}
void PreferencesDialog_destroyWindow(){
	g_Preferences.Destroy();
}


PreferenceDictionary g_preferences;
static bool g_startupWelcomeShowOnStartup = true;
static bool g_startupModernJourneyEnabled = true;
static bool g_startupShowLoadingScreen = true;

PreferenceSystem& GetPreferenceSystem(){
	return g_preferences;
}

class PreferenceSystemAPI
{
	PreferenceSystem* m_preferencesystem;
public:
	typedef PreferenceSystem Type;
	STRING_CONSTANT( Name, "*" );

	PreferenceSystemAPI() : m_preferencesystem( &GetPreferenceSystem() ){
	}
	PreferenceSystem* getTable(){
		return m_preferencesystem;
	}
};

#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

typedef SingletonModule<PreferenceSystemAPI> PreferenceSystemModule;
typedef Static<PreferenceSystemModule> StaticPreferenceSystemModule;
StaticRegisterModule staticRegisterPreferenceSystem( StaticPreferenceSystemModule::instance() );

void Preferences_Load(){
	g_GamesDialog.LoadPrefs();

	globalOutputStream() << "loading local preferences from " << g_Preferences.m_inipath << '\n';

	if ( !Preferences_Load( g_preferences, g_Preferences.m_inipath.c_str(), g_GamesDialog.m_sGameFile.m_value.c_str() ) ) {
		globalWarningStream() << "failed to load local preferences from " << g_Preferences.m_inipath << '\n';
	}
}

void Preferences_Save(){
	if ( g_preferences_globals.disable_ini ) {
		return;
	}

	g_GamesDialog.SavePrefs();

	globalOutputStream() << "saving local preferences to " << g_Preferences.m_inipath << '\n';

	if ( !Preferences_Save_Safe( g_preferences, g_Preferences.m_inipath.c_str() ) ) {
		globalWarningStream() << "failed to save local preferences to " << g_Preferences.m_inipath << '\n';
	}
}

void Preferences_Reset(){
	file_remove( g_Preferences.m_inipath.c_str() );
}


void PrefsDlg::PostModal( QDialog::DialogCode code ){
	if ( code == QDialog::DialogCode::Accepted ) {
		class PreferencesApplySpinnerGlyph : public QWidget
		{
			QTimer m_timer;
			int m_angle = 0;
		public:
			explicit PreferencesApplySpinnerGlyph( QWidget* parent = nullptr ) : QWidget( parent ){
				setFixedSize( 24, 24 );
				m_timer.setInterval( 60 );
				QObject::connect( &m_timer, &QTimer::timeout, this, [this](){
					m_angle = ( m_angle + 30 ) % 360;
					update();
				} );
				m_timer.start();
			}
		protected:
			void paintEvent( QPaintEvent* event ) override {
				QWidget::paintEvent( event );
				QPainter painter( this );
				painter.setRenderHint( QPainter::Antialiasing, true );
				painter.translate( width() * 0.5, height() * 0.5 );
				painter.rotate( static_cast<qreal>( m_angle ) );

				const QRectF arcRect( -8.5, -8.5, 17.0, 17.0 );
				QPen ringPen( palette().color( QPalette::Mid ), 2.0, Qt::SolidLine, Qt::RoundCap );
				painter.setPen( ringPen );
				painter.drawArc( arcRect, 0, 360 * 16 );

				QPen arcPen( palette().color( QPalette::Highlight ), 2.5, Qt::SolidLine, Qt::RoundCap );
				painter.setPen( arcPen );
				painter.drawArc( arcRect, 35 * 16, 250 * 16 );
			}
		};

		class PreferencesApplyOverlay
		{
			QPointer<QDialog> m_dialog;
		public:
			explicit PreferencesApplyOverlay( QWidget* parent ){
				auto* dialog = new QDialog( parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::CustomizeWindowHint );
				dialog->setModal( false );
				dialog->setAttribute( Qt::WA_ShowWithoutActivating );
				dialog->setAttribute( Qt::WA_TranslucentBackground );

				auto* frame = new QFrame( dialog );
				frame->setObjectName( "PreferencesApplyOverlayFrame" );
				frame->setStyleSheet( "QFrame#PreferencesApplyOverlayFrame { border: 1px solid palette(mid); border-radius: 10px; background-color: palette(window); }" );

				auto* frameLayout = new QVBoxLayout( frame );
				frameLayout->setContentsMargins( 12, 10, 12, 10 );
				frameLayout->setSpacing( 8 );
				frameLayout->addWidget( new PreferencesApplySpinnerGlyph( frame ), 0, Qt::AlignHCenter );

				auto* label = new QLabel( i18n::tr( "Applying settings..." ), frame );
				label->setAlignment( Qt::AlignCenter );
				frameLayout->addWidget( label );

				auto* layout = new QVBoxLayout( dialog );
				layout->setContentsMargins( 0, 0, 0, 0 );
				layout->addWidget( frame );

				dialog->adjustSize();

				QRect targetGeometry;
				if ( parent != nullptr ) {
					targetGeometry = parent->frameGeometry();
				}
				else if ( QScreen* screen = QGuiApplication::screenAt( QCursor::pos() ) ) {
					targetGeometry = screen->availableGeometry();
				}
				else if ( !QGuiApplication::screens().empty() ) {
					targetGeometry = QGuiApplication::screens().front()->availableGeometry();
				}
				if ( targetGeometry.isValid() ) {
					dialog->move( targetGeometry.center() - QPoint( dialog->width() / 2, dialog->height() / 2 ) );
				}

				dialog->show();
				dialog->raise();
				QCoreApplication::processEvents( QEventLoop::ProcessEventsFlag::ExcludeUserInputEvents );
				m_dialog = dialog;
			}

			~PreferencesApplyOverlay(){
				if ( !m_dialog.isNull() ) {
					m_dialog->hide();
					m_dialog->deleteLater();
					QCoreApplication::processEvents( QEventLoop::ProcessEventsFlag::ExcludeUserInputEvents );
				}
			}
		} applyingOverlay( MainFrame_getWindow() );

		Preferences_Save();
		UpdateAllWindows();
	}
}

std::vector<const char*> g_restart_required;

void PreferencesDialog_restartRequired( const char* staticName ){
	g_restart_required.push_back( staticName );
}

void PreferencesDialog_showDialog(){
	//if ( ConfirmModified( "Edit Preferences" ) && g_Preferences.DoModal() == eIDOK ) {
	if ( g_Preferences.m_searchEdit != nullptr ) {
		g_Preferences.m_searchEdit->setFocus();
	}
	if ( g_Preferences.m_treeview != nullptr && g_Preferences.m_treeview->model() != nullptr ) {
		g_Preferences.m_treeview->setCurrentIndex( g_Preferences.m_treeview->model()->index( 0, 0 ) );
	}
	if ( g_Preferences.m_searchEdit != nullptr ) {
		g_Preferences.m_searchEdit->clear();
	}
	if ( g_Preferences.DoModal() == QDialog::DialogCode::Accepted ) {
		if ( !g_restart_required.empty() ) {
			auto message = StringStream( "Preference changes require a restart:\n\n" );
			for ( const auto *i : g_restart_required )
				message << i << '\n';
			g_restart_required.clear();
			message << "\nRestart now?";

			if( qt_MessageBox( MainFrame_getWindow(), message, "Restart is required", EMessageBoxType::Question ) == eIDYES )
				Radiant_Restart();
		}
	}
}

void PreferencesDialog_showDialogForQuery( const char* query ){
	if ( g_Preferences.m_searchEdit != nullptr && query != nullptr && !string_empty( query ) ) {
		g_Preferences.m_searchEdit->setText( query );
	}
	if ( g_Preferences.m_searchEdit != nullptr ) {
		g_Preferences.m_searchEdit->setFocus();
	}
	if ( g_Preferences.DoModal() == QDialog::DialogCode::Accepted ) {
		if ( !g_restart_required.empty() ) {
			auto message = StringStream( "Preference changes require a restart:\n\n" );
			for ( const auto *i : g_restart_required )
				message << i << '\n';
			g_restart_required.clear();
			message << "\nRestart now?";

			if( qt_MessageBox( MainFrame_getWindow(), message, "Restart is required", EMessageBoxType::Question ) == eIDYES )
				Radiant_Restart();
		}
	}
	if ( g_Preferences.m_searchEdit != nullptr ) {
		g_Preferences.m_searchEdit->clear();
	}
}





void GameName_importString( const char* value ){
	if ( const char* startupGameName = StartupGameInstallationGameName_get(); !string_empty( startupGameName ) ) {
		gamename_set( startupGameName );
		return;
	}
	gamename_set( value );
}
typedef FreeCaller<void(const char*), GameName_importString> GameNameImportStringCaller;
void GameName_exportString( const StringImportCallback& importer ){
	importer( gamename_get() );
}
typedef FreeCaller<void(const StringImportCallback&), GameName_exportString> GameNameExportStringCaller;

void GameMode_importString( const char* value ){
	gamemode_set( value );
}
typedef FreeCaller<void(const char*), GameMode_importString> GameModeImportStringCaller;
void GameMode_exportString( const StringImportCallback& importer ){
	importer( gamemode_get() );
}
typedef FreeCaller<void(const StringImportCallback&), GameMode_exportString> GameModeExportStringCaller;

void StartupWelcome_constructPreferences( PreferencesPage& page ){
	page.appendCheckBox( "Startup", i18n::tr( "Show welcome screen on startup" ).toUtf8().constData(), g_startupWelcomeShowOnStartup );
	page.appendCheckBox( "", i18n::tr( "Use modern startup journey" ).toUtf8().constData(), g_startupModernJourneyEnabled );
	page.appendCheckBox( "", i18n::tr( "Show loading screen during startup" ).toUtf8().constData(), g_startupShowLoadingScreen );
}


void RegisterPreferences( PreferenceSystem& preferences ){
	preferences.registerPreference( "CustomShaderEditorCommand", CopiedStringImportStringCaller( g_TextEditor_editorCommand ), CopiedStringExportStringCaller( g_TextEditor_editorCommand ) );

	preferences.registerPreference( "GameName", GameNameImportStringCaller(), GameNameExportStringCaller() );
	preferences.registerPreference( "GameMode", GameModeImportStringCaller(), GameModeExportStringCaller() );
	preferences.registerPreference( "StartupShowWelcome", BoolImportStringCaller( g_startupWelcomeShowOnStartup ), BoolExportStringCaller( g_startupWelcomeShowOnStartup ) );
	preferences.registerPreference( "ShowStartupWelcome", BoolImportStringCaller( g_startupWelcomeShowOnStartup ), BoolExportStringCaller( g_startupWelcomeShowOnStartup ) ); // legacy compatibility
	preferences.registerPreference( "StartupModernJourney", BoolImportStringCaller( g_startupModernJourneyEnabled ), BoolExportStringCaller( g_startupModernJourneyEnabled ) );
	preferences.registerPreference( "StartupShowLoadingScreen", BoolImportStringCaller( g_startupShowLoadingScreen ), BoolExportStringCaller( g_startupShowLoadingScreen ) );
}

void Preferences_Init(){
	PreferencesDialog_addSettingsPreferences( makeCallbackF( StartupWelcome_constructPreferences ) );
	RegisterPreferences( GetPreferenceSystem() );
}

bool StartupWelcome_ShowOnStartup(){
	return g_startupWelcomeShowOnStartup;
}

void StartupWelcome_SetShowOnStartup( bool show ){
	g_startupWelcomeShowOnStartup = show;
}

bool StartupJourney_ModernEnabled(){
	return g_startupModernJourneyEnabled;
}

bool StartupJourney_ShowLoadingScreen(){
	return g_startupShowLoadingScreen;
}
