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
// Main Window for Q3Radiant
//
// Leonardo Zide (leo@lokigames.com)
//

#include "mainframe.h"

#include "debugging/debugging.h"
#include "version.h"

#include "ifilesystem.h"
#include "ientity.h"
#include "ishaders.h"
#include "ieclass.h"
#include "irender.h"
#include "igl.h"
#include "moduleobserver.h"
#include "environment.h"

#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <vector>
#include <array>
#include <string>
#include <string_view>
#include <set>
#include <cctype>

#include <QWidget>
#include <QAbstractButton>
#include <QSplashScreen>
#include <QCoreApplication>
#include <QMainWindow>
#include <QLabel>
#include <QSplitter>
#include <QMenuBar>
#include <QApplication>
#include <QToolBar>
#include <QStatusBar>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QSettings>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QPointer>
#include <QTimer>
#include <QStyle>

#include "commandlib.h"
#include "scenelib.h"
#include "stream/stringstream.h"
#include "signal/isignal.h"
#include "os/path.h"
#include "os/file.h"
#include <glib.h>
#include "moduleobservers.h"

#include "gtkutil/glfont.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/image.h"
#include "gtkutil/i18n.h"
#include "gtkutil/messagebox.h"
#include "gtkutil/menu.h"
#include "gtkutil/guisettings.h"
#include "gtkutil/widget.h"

#include "autosave.h"
#include "build.h"
#include "brushmanip.h"
#include "camwindow.h"
#include "csg.h"
#include "commands.h"
#include "console.h"
#include "entity.h"
#include "entityinspector.h"
#include "entitylist.h"
#include "filters.h"
#include "findtexturedialog.h"
#include "grid.h"
#include "groupdialog.h"
#include "gtkdlgs.h"
#include "gtkmisc.h"
#include "help.h"
#include "map.h"
#include "mru.h"
#include "error.h"
#include "patchmanip.h"
#include "plugin.h"
#include "pluginmanager.h"
#include "pluginmenu.h"
#include "plugintoolbar.h"
#include "preferences.h"
#include "update.h"
#include "qe3.h"
#include "qgl.h"
#include "select.h"
#include "selection.h"
#include "server.h"
#include "surfacedialog.h"
#include "textures.h"
#include "assetbrowser.h"
#include "entitybrowser.h"
#include "texwindow.h"
#include "modelwindow.h"
#include "layerswindow.h"
#include "soundbrowser.h"
#include "url.h"
#include "xywindow.h"
#include "zwindow.h"
#include "windowobservers.h"
#include "renderstate.h"
#include "feedback.h"
#include "referencecache.h"
#include "issuebrowser.h"
#include "uvview.h"

#include "colors.h"
#include "tools.h"
#include "filterbar.h"
#include "genai.h"


// VFS
class VFSModuleObserver : public ModuleObserver
{
	std::size_t m_unrealised;
public:
	VFSModuleObserver() : m_unrealised( 1 ){
	}
	void realise() override {
		if ( --m_unrealised == 0 ) {
			QE_InitVFS();
			GlobalFileSystem().initialise();
		}
	}
	void unrealise() override {
		if ( ++m_unrealised == 1 ) {
			GlobalFileSystem().shutdown();
		}
	}
};

VFSModuleObserver g_VFSModuleObserver;

void VFS_Construct(){
	Radiant_attachHomePathsObserver( g_VFSModuleObserver );
}
void VFS_Destroy(){
	Radiant_detachHomePathsObserver( g_VFSModuleObserver );
}

// Home Paths

#ifdef WIN32
#include <shlobj.h>
#include <objbase.h>
const GUID qFOLDERID_SavedGames = {0x4C5C32FF, 0xBB9D, 0x43b0, {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}};
#define qREFKNOWNFOLDERID GUID
#define qKF_FLAG_CREATE 0x8000
#define qKF_FLAG_NO_ALIAS 0x1000
typedef HRESULT ( WINAPI qSHGetKnownFolderPath_t )( qREFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR *ppszPath );
static qSHGetKnownFolderPath_t *qSHGetKnownFolderPath;
#endif
void HomePaths_Realise(){
	do
	{
		const char* prefix = g_pGameDescription->getKeyValue( "prefix" );
		if ( !string_empty( prefix ) ) {
			StringOutputStream path( 256 );

#if defined( __APPLE__ )
			path( DirectoryCleaned( g_get_home_dir() ), "Library/Application Support", ( prefix + 1 ), '/' );
			if ( file_is_directory( path ) ) {
				g_qeglobals.m_userEnginePath = path;
				break;
			}
			path( DirectoryCleaned( g_get_home_dir() ), prefix, '/' );
#endif

#if defined( WIN32 )
			TCHAR mydocsdir[MAX_PATH + 1];
			wchar_t *mydocsdirw;
			HMODULE shfolder = LoadLibrary( "shfolder.dll" );
			if ( shfolder ) {
#if defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
				qSHGetKnownFolderPath = reinterpret_cast<qSHGetKnownFolderPath_t *>( GetProcAddress( shfolder, "SHGetKnownFolderPath" ) );
#if defined( __GNUC__ )
#pragma GCC diagnostic pop
#endif
			}
			else{
				qSHGetKnownFolderPath = nullptr;
			}
			CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
			if ( qSHGetKnownFolderPath && qSHGetKnownFolderPath( qFOLDERID_SavedGames, qKF_FLAG_CREATE | qKF_FLAG_NO_ALIAS, nullptr, &mydocsdirw ) == S_OK ) {
				memset( mydocsdir, 0, sizeof( mydocsdir ) );
				wcstombs( mydocsdir, mydocsdirw, sizeof( mydocsdir ) - 1 );
				CoTaskMemFree( mydocsdirw );
				path( DirectoryCleaned( mydocsdir ), ( prefix + 1 ), '/' );
				if ( file_is_directory( path ) ) {
					g_qeglobals.m_userEnginePath = path;
					CoUninitialize();
					FreeLibrary( shfolder );
					break;
				}
			}
			CoUninitialize();
			if ( shfolder ) {
				FreeLibrary( shfolder );
			}
			if ( SUCCEEDED( SHGetFolderPath( nullptr, CSIDL_PERSONAL, nullptr, 0, mydocsdir ) ) ) {
				path( DirectoryCleaned( mydocsdir ), "My Games/", ( prefix + 1 ), '/' );
				// win32: only add it if it already exists
				if ( file_is_directory( path ) ) {
					g_qeglobals.m_userEnginePath = path;
					break;
				}
			}
#endif

#if defined( POSIX )
			path( DirectoryCleaned( g_get_home_dir() ), prefix, '/' );
			g_qeglobals.m_userEnginePath = path;
			break;
#endif
		}

		g_qeglobals.m_userEnginePath = EnginePath_get();
	}
	while ( false );

	Q_mkdir( g_qeglobals.m_userEnginePath.c_str() );

	g_qeglobals.m_userGamePath = StringStream( g_qeglobals.m_userEnginePath, gamename_get(), '/' );
	ASSERT_MESSAGE( !g_qeglobals.m_userGamePath.empty(), "HomePaths_Realise: user-game-path is empty" );
	Q_mkdir( g_qeglobals.m_userGamePath.c_str() );
}

ModuleObservers g_homePathObservers;

void Radiant_attachHomePathsObserver( ModuleObserver& observer ){
	g_homePathObservers.attach( observer );
}

void Radiant_detachHomePathsObserver( ModuleObserver& observer ){
	g_homePathObservers.detach( observer );
}

class HomePathsModuleObserver : public ModuleObserver
{
	std::size_t m_unrealised;
public:
	HomePathsModuleObserver() : m_unrealised( 1 ){
	}
	void realise() override {
		if ( --m_unrealised == 0 ) {
			HomePaths_Realise();
			g_homePathObservers.realise();
		}
	}
	void unrealise() override {
		if ( ++m_unrealised == 1 ) {
			g_homePathObservers.unrealise();
		}
	}
};

HomePathsModuleObserver g_HomePathsModuleObserver;

void HomePaths_Construct(){
	Radiant_attachEnginePathObserver( g_HomePathsModuleObserver );
}
void HomePaths_Destroy(){
	Radiant_detachEnginePathObserver( g_HomePathsModuleObserver );
}


// Engine Path

CopiedString g_strEnginePath;
ModuleObservers g_enginePathObservers;
std::size_t g_enginepath_unrealised = 1;

void Radiant_attachEnginePathObserver( ModuleObserver& observer ){
	g_enginePathObservers.attach( observer );
}

void Radiant_detachEnginePathObserver( ModuleObserver& observer ){
	g_enginePathObservers.detach( observer );
}


void EnginePath_Realise(){
	if ( --g_enginepath_unrealised == 0 ) {
		g_enginePathObservers.realise();
	}
}


const char* EnginePath_get(){
	ASSERT_MESSAGE( g_enginepath_unrealised == 0, "EnginePath_get: engine path not realised" );
	return g_strEnginePath.c_str();
}

void EnginePath_Unrealise(){
	if ( ++g_enginepath_unrealised == 1 ) {
		g_enginePathObservers.unrealise();
	}
}

static CopiedString g_installedDevFilesPath; // track last engine path, where dev files installation occured, to prompt again when changed

static void installDevFiles(){
	if( !path_equal( g_strEnginePath.c_str(), g_installedDevFilesPath.c_str() ) ){
		ASSERT_MESSAGE( g_enginepath_unrealised != 0, "installDevFiles: engine path realised" );
		DoInstallDevFilesDlg( g_strEnginePath.c_str() );
		g_installedDevFilesPath = g_strEnginePath;
	}
}

namespace
{
struct GameInstallRule
{
	CopiedString gameFile;
	CopiedString gameName;
	CopiedString basegameName;
	CopiedString basegame;
	CopiedString engineExecutable;
	std::vector<CopiedString> archiveTypes;
	std::vector<CopiedString> requiredFiles;
	std::vector<CopiedString> aliases;
};

struct DetectedEngineInstall
{
	CopiedString path;
	CopiedString source;
};

std::vector<DetectedEngineInstall> g_detectedEngineInstalls;
int g_detectedEngineInstallInitialSelection = -1;

std::string EnginePath_toLower( const std::string& value ){
	std::string lower;
	lower.reserve( value.size() );
	for ( const unsigned char c : value )
	{
		lower.push_back( static_cast<char>( std::tolower( c ) ) );
	}
	return lower;
}

std::string EnginePath_normaliseToken( const char* value ){
	std::string token;
	if ( value == nullptr ) {
		return token;
	}
	for ( const unsigned char c : std::string_view( value ) )
	{
		if ( std::isalnum( c ) ) {
			token.push_back( static_cast<char>( std::tolower( c ) ) );
		}
	}
	return token;
}

std::string EnginePath_trim( std::string value ){
	while ( !value.empty() && std::isspace( static_cast<unsigned char>( value.front() ) ) )
		value.erase( value.begin() );
	while ( !value.empty() && std::isspace( static_cast<unsigned char>( value.back() ) ) )
		value.pop_back();
	return value;
}

std::vector<CopiedString> EnginePath_splitList( const char* value, const char* delimiters ){
	std::vector<CopiedString> tokens;
	if ( value == nullptr || string_empty( value ) ) {
		return tokens;
	}

	std::string token;
	for ( const char c : std::string_view( value ) )
	{
		if ( strchr( delimiters, c ) != nullptr ) {
			token = EnginePath_trim( token );
			if ( !token.empty() ) {
				tokens.emplace_back( token.c_str() );
			}
			token.clear();
			continue;
		}
		token.push_back( c );
	}
	token = EnginePath_trim( token );
	if ( !token.empty() ) {
		tokens.emplace_back( token.c_str() );
	}
	return tokens;
}

void EnginePath_appendUniqueAlias( std::vector<CopiedString>& aliases, const char* alias ){
	if ( alias == nullptr || string_empty( alias ) ) {
		return;
	}

	const std::string trimmed = EnginePath_trim( alias );
	const std::string normalised = EnginePath_normaliseToken( trimmed.c_str() );
	if ( normalised.empty() ) {
		return;
	}
	for ( const auto& existing : aliases )
	{
		if ( EnginePath_normaliseToken( existing.c_str() ) == normalised ) {
			return;
		}
	}
	aliases.emplace_back( trimmed.c_str() );
}

void EnginePath_appendUniquePath( std::vector<CopiedString>& values, const char* value ){
	if ( value == nullptr || string_empty( value ) ) {
		return;
	}
	for ( const auto& existing : values )
	{
		if ( path_equal( existing.c_str(), value ) ) {
			return;
		}
	}
	values.emplace_back( value );
}

const char* EnginePath_platformEngineAttribute(){
#if defined( WIN32 )
	return "engine_win32";
#elif defined( __APPLE__ )
	return "engine_macos";
#elif defined( __linux__ ) || defined ( __FreeBSD__ )
	return "engine_linux";
#else
#error "unsupported platform"
#endif
}

GameInstallRule EnginePath_buildInstallRule(){
	GameInstallRule rule;
	rule.gameFile = g_pGameDescription->mGameFile;
	rule.gameName = g_pGameDescription->getKeyValue( "name" );
	rule.basegameName = g_pGameDescription->getKeyValue( "basegamename" );
	rule.basegame = g_pGameDescription->getRequiredKeyValue( "basegame" );
	rule.engineExecutable = g_pGameDescription->getKeyValue( EnginePath_platformEngineAttribute() );

	for ( const auto& token : EnginePath_splitList( g_pGameDescription->getKeyValue( "archivetypes" ), " ,;|\t\r\n" ) )
	{
		EnginePath_appendUniquePath( rule.archiveTypes, EnginePath_toLower( token.c_str() ).c_str() );
	}

	for ( const char* key : { "detect_file1", "detect_file2" } )
	{
		const char* value = g_pGameDescription->getKeyValue( key );
		if ( !string_empty( value ) ) {
			rule.requiredFiles.emplace_back( value );
		}
	}
	for ( const auto& token : EnginePath_splitList( g_pGameDescription->getKeyValue( "detect_files" ), ",;|" ) )
	{
		EnginePath_appendUniquePath( rule.requiredFiles, token.c_str() );
	}

	EnginePath_appendUniqueAlias( rule.aliases, rule.basegame.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, rule.basegameName.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, g_pGameDescription->mGameType.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, rule.gameName.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, g_pGameDescription->mGameFile.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, g_pGameDescription->getKeyValue( "knowngame" ) );
	EnginePath_appendUniqueAlias( rule.aliases, g_pGameDescription->getKeyValue( "knowngamename" ) );
	EnginePath_appendUniqueAlias( rule.aliases, g_pGameDescription->getKeyValue( "unknowngamename" ) );
	for ( const auto& token : EnginePath_splitList( g_pGameDescription->getKeyValue( "knownmods" ), ",;|" ) )
	{
		EnginePath_appendUniqueAlias( rule.aliases, token.c_str() );
	}
	for ( const auto& token : EnginePath_splitList( g_pGameDescription->getKeyValue( "knownmodnames" ), ",;|" ) )
	{
		EnginePath_appendUniqueAlias( rule.aliases, token.c_str() );
	}
	for ( const auto& token : EnginePath_splitList( g_pGameDescription->getKeyValue( "install_aliases" ), ",;|" ) )
	{
		EnginePath_appendUniqueAlias( rule.aliases, token.c_str() );
	}

	const std::filesystem::path gameFilePath( rule.gameFile.c_str() );
	EnginePath_appendUniqueAlias( rule.aliases, gameFilePath.stem().string().c_str() );
	if ( !rule.engineExecutable.empty() ) {
		const std::filesystem::path enginePath( rule.engineExecutable.c_str() );
		EnginePath_appendUniqueAlias( rule.aliases, enginePath.stem().string().c_str() );
	}

	return rule;
}

void EnginePath_applyLegacyHints( GameInstallRule& rule ){
	const auto gameFileLower = EnginePath_toLower( rule.gameFile.c_str() );
	const auto addRequired = [&rule]( const char* value ){
		EnginePath_appendUniquePath( rule.requiredFiles, value );
	};
	const auto addAlias = [&rule]( const char* value ){
		EnginePath_appendUniqueAlias( rule.aliases, value );
	};

	if ( gameFileLower == "q2re.game" ) {
		addRequired( "rerelease" );
		addAlias( "Quake II Rerelease" );
		addAlias( "rerelease" );
	}
	else if ( gameFileLower == "q2.game" ) {
		addAlias( "Quake 2" );
		addAlias( "Quake II" );
	}
	else if ( gameFileLower == "q1.game" ) {
		addAlias( "Quake 1" );
		addAlias( "Quake" );
	}
	else if ( gameFileLower == "q3.game" ) {
		addRequired( "pak0.pk3" );
		addAlias( "Quake 3" );
		addAlias( "Quake III Arena" );
	}
	else if ( gameFileLower == "quakelive.game" ) {
		addRequired( "pak00.pk3" );
		addAlias( "Quake Live" );
		addAlias( "QuakeLive" );
	}
	else if ( gameFileLower == "heretic2.game" ) {
		addAlias( "Heretic 2" );
		addAlias( "Heretic II" );
	}
	else if ( gameFileLower == "kingpin.game" ) {
		addAlias( "Kingpin Life of Crime" );
	}
	else if ( gameFileLower == "nexuiz.game" ) {
		addRequired( "data/common-spog.pk3" );
		addAlias( "Nexuiz" );
	}
	else if ( gameFileLower == "doom3-demo.game" ) {
		addAlias( "Doom 3 Demo" );
		addAlias( "Doom3 Demo" );
	}
	else if ( gameFileLower == "xreal.game" ) {
		addAlias( "XreaL" );
		addAlias( "Xreal" );
	}
	else if ( gameFileLower == "warsow.game" ) {
		addRequired( "basewsw/dedicated_autoexec.cfg" );
		addAlias( "Warsow" );
	}
}

bool EnginePath_isWeakAlias( const std::string& token ){
	static const std::set<std::string> weak = {
		"base", "data", "main", "default", "pkg", "id1", "mod", "game"
	};
	return token.size() < 4 || weak.find( token ) != weak.end();
}

bool EnginePath_textMatchesAliases( const std::string& textNormalised, const GameInstallRule& rule ){
	for ( const auto& alias : rule.aliases )
	{
		const std::string token = EnginePath_normaliseToken( alias.c_str() );
		if ( EnginePath_isWeakAlias( token ) ) {
			continue;
		}
		if ( textNormalised.find( token ) != std::string::npos ) {
			return true;
		}
	}
	return false;
}

const char* EnginePath_defaultPathAttribute(){
#if defined( WIN32 )
	return "enginepath_win32";
#elif defined( __APPLE__ )
	return "enginepath_macos";
#elif defined( __linux__ ) || defined ( __FreeBSD__ )
	return "enginepath_linux";
#else
#error "unsupported platform"
#endif
}

bool EnginePath_hasGameDataAt( const char* installRoot, const GameInstallRule& rule ){
	if ( installRoot == nullptr || string_empty( installRoot ) ) {
		return false;
	}

	const auto cleanedRoot = StringStream( DirectoryCleaned( installRoot ) );
	const std::filesystem::path root{ cleanedRoot.c_str() };
	const std::string cleaned = root.string();
	if ( !file_is_directory( cleaned.c_str() ) ) {
		return false;
	}

	std::vector<std::filesystem::path> baseCandidates;
	if ( !rule.basegame.empty() ) {
		baseCandidates.emplace_back( root / rule.basegame.c_str() );
	}
	if ( !rule.basegame.empty() ) {
		const std::string rootLeaf = EnginePath_toLower( root.filename().string() );
		const std::string baseLeaf = EnginePath_toLower( rule.basegame.c_str() );
		if ( rootLeaf == baseLeaf ) {
			baseCandidates.emplace_back( root );
		}
	}

	const bool hasBaseDir = std::ranges::any_of( baseCandidates, []( const std::filesystem::path& path ){
		return file_is_directory( path.string().c_str() );
	} );

	bool hasEngine = false;
	if ( !rule.engineExecutable.empty() ) {
		hasEngine = file_exists( ( root / rule.engineExecutable.c_str() ).string().c_str() );
	}

	const auto hasArchiveType = [&rule]( const std::filesystem::path& directory ){
		if ( rule.archiveTypes.empty() ) {
			return false;
		}
		std::error_code err;
		for ( const auto& entry : std::filesystem::directory_iterator( directory, std::filesystem::directory_options::skip_permission_denied, err ) )
		{
			if ( !entry.is_regular_file( err ) ) {
				continue;
			}
			auto ext = entry.path().extension().string();
			if ( !ext.empty() && ext[0] == '.' ) {
				ext.erase( ext.begin() );
			}
			ext = EnginePath_toLower( ext );
			for ( const auto& wanted : rule.archiveTypes )
			{
				if ( string_equal( ext.c_str(), wanted.c_str() ) ) {
					return true;
				}
			}
		}
		return false;
	};

	const auto hasContentHints = []( const std::filesystem::path& directory ){
		for ( const char* hint : { "maps", "scripts", "materials", "textures", "models", "sound", "shaders" } )
		{
			if ( file_is_directory( ( directory / hint ).string().c_str() ) ) {
				return true;
			}
		}
		for ( const char* fileHint : { "pak0.pk3", "pak0.pak", "pak0.pk4", "gamex86.dll", "gamex64.dll" } )
		{
			if ( file_exists( ( directory / fileHint ).string().c_str() ) ) {
				return true;
			}
		}
		return false;
	};

	bool hasArchives = false;
	bool hasContent = false;
	for ( const auto& path : baseCandidates )
	{
		if ( !file_is_directory( path.string().c_str() ) ) {
			continue;
		}
		hasArchives = hasArchives || hasArchiveType( path );
		hasContent = hasContent || hasContentHints( path );
	}

	if ( !rule.requiredFiles.empty() ) {
		for ( const auto& required : rule.requiredFiles )
		{
			const std::filesystem::path rel( required.c_str() );
			if ( file_exists( ( root / rel ).string().c_str() ) ) {
				continue;
			}
			bool foundInBase = false;
			for ( const auto& base : baseCandidates )
			{
				if ( file_exists( ( base / rel ).string().c_str() ) ) {
					foundInBase = true;
					break;
				}
			}
			if ( !foundInBase ) {
				return false;
			}
		}
		return hasBaseDir;
	}

	return hasBaseDir && ( hasEngine || hasArchives || hasContent );
}

bool EnginePath_hasDetectedPath( const std::vector<DetectedEngineInstall>& installs, const char* path ){
	for ( const auto& install : installs )
	{
		if ( path_equal( install.path.c_str(), path ) ) {
			return true;
		}
	}
	return false;
}

CopiedString EnginePath_normaliseInstallRoot( const char* candidatePath, const GameInstallRule& rule ){
	if ( candidatePath == nullptr || string_empty( candidatePath ) ) {
		return "";
	}

	const auto cleanedCandidate = StringStream( DirectoryCleaned( candidatePath ) );
	std::filesystem::path path{ cleanedCandidate.c_str() };
	if ( path.empty() ) {
		return "";
	}

	if ( !rule.basegame.empty() ) {
		const auto leaf = EnginePath_toLower( path.filename().string() );
		const auto base = EnginePath_toLower( rule.basegame.c_str() );
		if ( leaf == base ) {
			const auto parent = path.parent_path();
			if ( !parent.empty()
			  && file_is_directory( parent.string().c_str() )
			  && file_is_directory( ( parent / rule.basegame.c_str() ).string().c_str() ) ) {
				path = parent;
			}
		}
	}

	const auto cleaned = StringStream( DirectoryCleaned( path.string().c_str() ) );
	return cleaned.c_str();
}

void EnginePath_addDetectedDirectory( std::vector<DetectedEngineInstall>& installs, const char* candidatePath, const char* source ){
	if ( candidatePath == nullptr || string_empty( candidatePath ) ) {
		return;
	}
	const auto cleaned = StringStream( DirectoryCleaned( candidatePath ) );
	if ( !file_is_directory( cleaned.c_str() ) ) {
		return;
	}
	if ( EnginePath_hasDetectedPath( installs, cleaned.c_str() ) ) {
		return;
	}
	installs.push_back( { cleaned.c_str(), source } );
}

void EnginePath_addDetectedInstall( std::vector<DetectedEngineInstall>& installs, const char* candidatePath, const char* source, const GameInstallRule& rule ){
	if ( candidatePath == nullptr || string_empty( candidatePath ) ) {
		return;
	}
	const auto cleaned = EnginePath_normaliseInstallRoot( candidatePath, rule );
	if ( !EnginePath_hasGameDataAt( cleaned.c_str(), rule ) ) {
		return;
	}
	if ( EnginePath_hasDetectedPath( installs, cleaned.c_str() ) ) {
		return;
	}
	installs.push_back( { cleaned.c_str(), source } );
}

void EnginePath_addUniqueDirectory( std::vector<CopiedString>& paths, const char* path ){
	if ( path == nullptr || string_empty( path ) ) {
		return;
	}
	const auto cleaned = StringStream( DirectoryCleaned( path ) );
	if ( !file_is_directory( cleaned.c_str() ) ) {
		return;
	}
	for ( const auto& existing : paths )
	{
		if ( path_equal( existing.c_str(), cleaned.c_str() ) ) {
			return;
		}
	}
	paths.push_back( cleaned.c_str() );
}

void EnginePath_detectNearbyInstalls( std::vector<DetectedEngineInstall>& installs, const GameInstallRule& rule ){
	auto current = std::filesystem::path( environment_get_app_path() );
	while ( !current.empty() )
	{
		EnginePath_addDetectedInstall( installs, current.string().c_str(), "Local install", rule );
		const auto parent = current.parent_path();
		if ( parent == current ) {
			break;
		}
		current = parent;
	}
}

#if defined( WIN32 )
void EnginePath_collectSteamRoots( std::vector<CopiedString>& steamRoots ){
	if ( const auto* steamDir = getenv( "STEAMDIR" ); steamDir != nullptr && !string_empty( steamDir ) ) {
		EnginePath_addUniqueDirectory( steamRoots, steamDir );
	}
	if ( const auto* programFilesX86 = getenv( "PROGRAMFILES(X86)" ); programFilesX86 != nullptr && !string_empty( programFilesX86 ) ) {
		EnginePath_addUniqueDirectory( steamRoots, StringStream( DirectoryCleaned( programFilesX86 ), "Steam/" ) );
	}
	if ( const auto* programFiles = getenv( "PROGRAMFILES" ); programFiles != nullptr && !string_empty( programFiles ) ) {
		EnginePath_addUniqueDirectory( steamRoots, StringStream( DirectoryCleaned( programFiles ), "Steam/" ) );
	}

	auto addSteamFromRegistry = [&steamRoots]( HKEY hkey, const char* subkey, const char* valueName ){
		char value[MAX_PATH * 4];
		DWORD size = sizeof( value );
		if ( RegGetValueA( hkey, subkey, valueName, RRF_RT_REG_SZ, nullptr, value, &size ) == ERROR_SUCCESS ) {
			EnginePath_addUniqueDirectory( steamRoots, value );
		}
	};
	addSteamFromRegistry( HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath" );
	addSteamFromRegistry( HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath" );
	addSteamFromRegistry( HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" );
}

void EnginePath_parseSteamLibraryFolders( const char* path, std::vector<CopiedString>& libraryRoots ){
	std::ifstream file( path, std::ios::in );
	if ( !file.is_open() ) {
		return;
	}

	std::string line;
	while ( std::getline( file, line ) )
	{
		const auto pathToken = line.find( "\"path\"" );
		if ( pathToken == std::string::npos ) {
			continue;
		}

		auto valueStart = line.find( '"', pathToken + 6 );
		if ( valueStart == std::string::npos ) {
			continue;
		}

		++valueStart;
		std::string decoded;
		for ( auto i = valueStart; i < line.size(); ++i )
		{
			const auto c = line[i];
			if ( c == '"' ) {
				break;
			}
			if ( c == '\\' && i + 1 < line.size() && ( line[i + 1] == '\\' || line[i + 1] == '"' ) ) {
				decoded.push_back( line[i + 1] );
				++i;
				continue;
			}
			decoded.push_back( c );
		}

		EnginePath_addUniqueDirectory( libraryRoots, decoded.c_str() );
	}
}

void EnginePath_collectSteamCommonRoots( std::vector<CopiedString>& commonRoots ){
	std::vector<CopiedString> steamRoots;
	EnginePath_collectSteamRoots( steamRoots );

	for ( const auto& steamRoot : steamRoots )
	{
		std::vector<CopiedString> libraries;
		EnginePath_addUniqueDirectory( libraries, steamRoot.c_str() );
		EnginePath_parseSteamLibraryFolders( StringStream( steamRoot, "steamapps/libraryfolders.vdf" ), libraries );
		for ( const auto& library : libraries )
		{
			EnginePath_addUniqueDirectory( commonRoots, StringStream( library, "steamapps/common/" ) );
		}
	}
}

void EnginePath_collectGogRoots( std::vector<CopiedString>& roots ){
	EnginePath_addUniqueDirectory( roots, "C:/GOG Games/" );
	if ( const auto* programFilesX86 = getenv( "PROGRAMFILES(X86)" ); programFilesX86 != nullptr && !string_empty( programFilesX86 ) ) {
		EnginePath_addUniqueDirectory( roots, StringStream( DirectoryCleaned( programFilesX86 ), "GOG Galaxy/Games/" ) );
	}
	if ( const auto* programFiles = getenv( "PROGRAMFILES" ); programFiles != nullptr && !string_empty( programFiles ) ) {
		EnginePath_addUniqueDirectory( roots, StringStream( DirectoryCleaned( programFiles ), "GOG Galaxy/Games/" ) );
	}
}

void EnginePath_collectWindowsUninstallRoots( std::vector<CopiedString>& roots, const GameInstallRule& rule ){
	const std::array<std::pair<HKEY, const char*>, 6> uninstallRoots{{
		{ HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
		{ HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
		{ HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
		{ HKEY_CURRENT_USER, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
		{ HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games" },
		{ HKEY_CURRENT_USER, "SOFTWARE\\GOG.com\\Games" },
	}};

	for ( const auto& [hive, keyPath] : uninstallRoots )
	{
		HKEY uninstallKey = nullptr;
		if ( RegOpenKeyExA( hive, keyPath, 0, KEY_READ, &uninstallKey ) != ERROR_SUCCESS ) {
			continue;
		}

		for ( DWORD index = 0;; ++index )
		{
			char subKeyName[512];
			DWORD subKeySize = static_cast<DWORD>( std::size( subKeyName ) );
			if ( RegEnumKeyExA( uninstallKey, index, subKeyName, &subKeySize, nullptr, nullptr, nullptr, nullptr ) != ERROR_SUCCESS ) {
				break;
			}

			const auto subPath = StringStream( keyPath, "\\", subKeyName );

			char displayName[2048]{};
			DWORD displayNameSize = sizeof( displayName );
			const bool hasDisplayName = RegGetValueA( hive, subPath.c_str(), "DisplayName", RRF_RT_REG_SZ, nullptr, displayName, &displayNameSize ) == ERROR_SUCCESS;
			if ( hasDisplayName ) {
				const auto normalised = EnginePath_normaliseToken( displayName );
				if ( !EnginePath_textMatchesAliases( normalised, rule ) ) {
					continue;
				}
			}

			char installLocation[4096]{};
			DWORD installLocationSize = sizeof( installLocation );
			if ( RegGetValueA( hive, subPath.c_str(), "InstallLocation", RRF_RT_REG_SZ, nullptr, installLocation, &installLocationSize ) == ERROR_SUCCESS ) {
				EnginePath_addUniqueDirectory( roots, installLocation );
			}
			for ( const char* valueName : { "Path", "path", "InstallDir", "Install Dir" } )
			{
				char candidate[4096]{};
				DWORD candidateSize = sizeof( candidate );
				if ( RegGetValueA( hive, subPath.c_str(), valueName, RRF_RT_REG_SZ, nullptr, candidate, &candidateSize ) == ERROR_SUCCESS ) {
					EnginePath_addUniqueDirectory( roots, candidate );
				}
			}
		}

		RegCloseKey( uninstallKey );
	}
}
#endif

void EnginePath_collectSystemRoots( std::vector<CopiedString>& roots ){
	for ( const char* envVar : { "RADIANT_GAME_PATHS", "VIBERADIANT_GAME_PATHS", "GAME_INSTALL_PATHS" } )
	{
		if ( const auto* value = getenv( envVar ); value != nullptr && !string_empty( value ) ) {
#if defined( WIN32 )
			for ( const auto& token : EnginePath_splitList( value, ";" ) )
#else
			for ( const auto& token : EnginePath_splitList( value, ":;" ) )
#endif
			{
				EnginePath_addUniqueDirectory( roots, token.c_str() );
			}
		}
	}

	EnginePath_addUniqueDirectory( roots, g_strEnginePath.c_str() );
	EnginePath_addUniqueDirectory( roots, g_pGameDescription->getKeyValue( EnginePath_defaultPathAttribute() ) );
	EnginePath_addUniqueDirectory( roots, g_qeglobals.m_userEnginePath.c_str() );
	EnginePath_addUniqueDirectory( roots, environment_get_app_path() );

#if defined( WIN32 )
	for ( const char* defaultRoot : { "C:/Games/", "D:/Games/", "E:/Games/", "C:/", "D:/" } )
	{
		EnginePath_addUniqueDirectory( roots, defaultRoot );
	}
	if ( const auto* programFilesX86 = getenv( "PROGRAMFILES(X86)" ); programFilesX86 != nullptr && !string_empty( programFilesX86 ) ) {
		EnginePath_addUniqueDirectory( roots, programFilesX86 );
	}
	if ( const auto* programFiles = getenv( "PROGRAMFILES" ); programFiles != nullptr && !string_empty( programFiles ) ) {
		EnginePath_addUniqueDirectory( roots, programFiles );
	}
#elif defined( __APPLE__ )
	for ( const char* defaultRoot : { "/Applications/", "/Applications/Games/", "/Users/Shared/" } )
	{
		EnginePath_addUniqueDirectory( roots, defaultRoot );
	}
#else
	for ( const char* defaultRoot : { "/usr/local/games/", "/usr/games/", "/opt/", "/opt/games/" } )
	{
		EnginePath_addUniqueDirectory( roots, defaultRoot );
	}
	EnginePath_addUniqueDirectory( roots, StringStream( DirectoryCleaned( g_get_home_dir() ), ".steam/steam/steamapps/common/" ) );
	EnginePath_addUniqueDirectory( roots, StringStream( DirectoryCleaned( g_get_home_dir() ), ".local/share/Steam/steamapps/common/" ) );
#endif
}

void EnginePath_detectFromRoots( std::vector<DetectedEngineInstall>& installs, const std::vector<CopiedString>& roots, const char* source, const GameInstallRule& rule ){
	for ( const auto& root : roots )
	{
		EnginePath_addDetectedInstall( installs, root.c_str(), source, rule );

		for ( const auto& alias : rule.aliases )
		{
			EnginePath_addDetectedInstall( installs, StringStream( root, alias, '/' ), source, rule );
		}

		std::error_code err;
		for ( const auto& entry : std::filesystem::directory_iterator( root.c_str(), std::filesystem::directory_options::skip_permission_denied, err ) )
		{
			if ( !entry.is_directory( err ) ) {
				continue;
			}
			const auto childName = EnginePath_normaliseToken( entry.path().filename().string().c_str() );
			if ( childName.empty() || !EnginePath_textMatchesAliases( childName, rule ) ) {
				continue;
			}
			EnginePath_addDetectedInstall( installs, entry.path().string().c_str(), source, rule );
		}
	}
}

CopiedString EnginePath_detectedInstallLabel( const DetectedEngineInstall& install ){
	const auto label = StringStream( install.source, " - ", install.path );
	return label.c_str();
}

void EnginePath_refreshDetectedInstalls(){
	g_detectedEngineInstalls.clear();
	g_detectedEngineInstallInitialSelection = -1;

	const auto rule = EnginePath_buildInstallRule();
	auto effectiveRule = rule;
	EnginePath_applyLegacyHints( effectiveRule );
	if ( effectiveRule.basegame.empty() ) {
		return;
	}

	EnginePath_addDetectedDirectory( g_detectedEngineInstalls, g_strEnginePath.c_str(), "Current setting" );
	EnginePath_addDetectedInstall(
		g_detectedEngineInstalls,
		g_pGameDescription->getKeyValue( EnginePath_defaultPathAttribute() ),
		"Gamepack default",
		effectiveRule
	);

	EnginePath_detectNearbyInstalls( g_detectedEngineInstalls, effectiveRule );

	std::vector<CopiedString> systemRoots;
	EnginePath_collectSystemRoots( systemRoots );
	EnginePath_detectFromRoots( g_detectedEngineInstalls, systemRoots, "System", effectiveRule );

#if defined( WIN32 )
	std::vector<CopiedString> steamCommonRoots;
	EnginePath_collectSteamCommonRoots( steamCommonRoots );
	EnginePath_detectFromRoots( g_detectedEngineInstalls, steamCommonRoots, "Steam", effectiveRule );

	std::vector<CopiedString> gogRoots;
	EnginePath_collectGogRoots( gogRoots );
	EnginePath_detectFromRoots( g_detectedEngineInstalls, gogRoots, "GOG", effectiveRule );

	std::vector<CopiedString> registryRoots;
	EnginePath_collectWindowsUninstallRoots( registryRoots, effectiveRule );
	EnginePath_detectFromRoots( g_detectedEngineInstalls, registryRoots, "Windows Registry", effectiveRule );
#endif
}
} // namespace

std::vector<DetectedGameInstallPath> EnginePath_detectInstallationsForGame( const CGameDescription& gameDescription, const char* currentPath ){
	CGameDescription* const previousGameDescription = g_pGameDescription;
	const CopiedString previousEnginePath = g_strEnginePath;

	g_pGameDescription = const_cast<CGameDescription*>( &gameDescription );
	g_strEnginePath = ( currentPath != nullptr && !string_empty( currentPath ) )
		? StringStream( DirectoryCleaned( currentPath ) )
		: "";

	EnginePath_refreshDetectedInstalls();

	const auto rule = EnginePath_buildInstallRule();
	auto effectiveRule = rule;
	EnginePath_applyLegacyHints( effectiveRule );

	std::vector<DetectedGameInstallPath> installs;
	installs.reserve( g_detectedEngineInstalls.size() );
	for ( const auto& install : g_detectedEngineInstalls )
	{
		if ( !EnginePath_hasGameDataAt( install.path.c_str(), effectiveRule ) ) {
			continue;
		}
		bool duplicate = false;
		for ( const auto& existing : installs )
		{
			if ( path_equal( existing.path.c_str(), install.path.c_str() ) ) {
				duplicate = true;
				break;
			}
		}
		if ( duplicate ) {
			continue;
		}
		installs.push_back( { install.path, install.source } );
	}

	g_strEnginePath = previousEnginePath;
	g_pGameDescription = previousGameDescription;
	if ( g_pGameDescription != nullptr ) {
		EnginePath_refreshDetectedInstalls();
	}
	else{
		g_detectedEngineInstalls.clear();
		g_detectedEngineInstallInitialSelection = -1;
	}

	return installs;
}

void setEnginePath( CopiedString& self, const char* value ){
	const auto buffer = StringStream( DirectoryCleaned( value ) );
	if ( !path_equal( buffer, self.c_str() ) ) {
#if 0
		while ( !ConfirmModified( "Paths Changed" ) )
		{
			if ( Map_Unnamed( g_map ) ) {
				Map_SaveAs();
			}
			else
			{
				Map_Save();
			}
		}
		Map_RegionOff();
#endif

		ScopeDisableScreenUpdates disableScreenUpdates( "Processing...", "Changing Engine Path" );

		EnginePath_Unrealise();

		self = buffer;

		installDevFiles();

		EnginePath_Realise();
	}
}
typedef ReferenceCaller<CopiedString, void(const char*), setEnginePath> EnginePathImportCaller;

void EnginePathDetectedInstall_import( int value ){
	if ( value == g_detectedEngineInstallInitialSelection ) {
		return;
	}
	if ( value >= 0 && value < static_cast<int>( g_detectedEngineInstalls.size() ) ) {
		setEnginePath( g_strEnginePath, g_detectedEngineInstalls[value].path.c_str() );
	}
}
typedef FreeCaller<void(int), EnginePathDetectedInstall_import> EnginePathDetectedInstallImportCaller;

void EnginePathDetectedInstall_export( const IntImportCallback& importCallback ){
	int selected = -1;
	for ( size_t i = 0; i < g_detectedEngineInstalls.size(); ++i )
	{
		if ( path_equal( g_detectedEngineInstalls[i].path.c_str(), g_strEnginePath.c_str() ) ) {
			selected = static_cast<int>( i );
			break;
		}
	}
	g_detectedEngineInstallInitialSelection = selected;
	importCallback( selected < 0? 0 : selected );
}
typedef FreeCaller<void(const IntImportCallback&), EnginePathDetectedInstall_export> EnginePathDetectedInstallExportCaller;


// Extra Resource Path

std::array<CopiedString, 5> g_strExtraResourcePaths;

const std::array<CopiedString, 5>& ExtraResourcePaths_get(){
	return g_strExtraResourcePaths;
}


// App Path

CopiedString g_strAppPath;                 ///< holds the full path of the executable

const char* AppPath_get(){
	return g_strAppPath.c_str();
}

/// the path to the local rc-dir
const char* LocalRcPath_get(){
	static CopiedString rc_path;
	if ( rc_path.empty() ) {
		rc_path = StringStream( GlobalRadiant().getSettingsPath(), g_pGameDescription->mGameFile, '/' );
	}
	return rc_path.c_str();
}

/// directory for temp files
/// NOTE: on *nix this is were we check for .pid
CopiedString g_strSettingsPath;
const char* SettingsPath_get(){
	return g_strSettingsPath.c_str();
}


/*!
   points to the game tools directory, for instance
   C:/Program Files/Quake III Arena/GtkRadiant
   (or other games)
   this is one of the main variables that are configured by the game selection on startup
   [GameToolsPath]/plugins
   [GameToolsPath]/modules
   and also q3map, bspc
 */
CopiedString g_strGameToolsPath;           ///< this is set by g_GamesDialog

const char* GameToolsPath_get(){
	return g_strGameToolsPath.c_str();
}


void Paths_constructPreferences( PreferencesPage& page ){
	page.appendPathEntry( "Engine Path", true,
	                      StringImportCallback( EnginePathImportCaller( g_strEnginePath ) ),
	                      StringExportCallback( StringExportCaller( g_strEnginePath ) )
	                    );

	EnginePath_refreshDetectedInstalls();
	if ( !g_detectedEngineInstalls.empty() ) {
		std::vector<CopiedString> labels;
		std::vector<const char*> entries;
		labels.reserve( g_detectedEngineInstalls.size() );
		entries.reserve( g_detectedEngineInstalls.size() );
		for ( const auto& install : g_detectedEngineInstalls )
		{
			labels.push_back( EnginePath_detectedInstallLabel( install ) );
			entries.push_back( labels.back().c_str() );
		}
		page.appendCombo(
			"Detected Installations",
			StringArrayRange( entries ),
			IntImportCallback( EnginePathDetectedInstallImportCaller() ),
			IntExportCallback( EnginePathDetectedInstallExportCaller() )
		);
	}
}
void Paths_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Paths", "Path Settings" ) );
	Paths_constructPreferences( page );
	for( auto& extraPath : g_strExtraResourcePaths )
		page.appendPathEntry( "Extra Resource Path", true,
		                      StringImportCallback( EnginePathImportCaller( extraPath ) ),
		                      StringExportCallback( StringExportCaller( extraPath ) )
		                    );
}
void Paths_registerPreferencesPage(){
	PreferencesDialog_addGamePage( makeCallbackF( Paths_constructPage ) );
}


class PathsDialog : public Dialog
{
public:
	void BuildDialog() override {
		GetWidget()->setWindowTitle( i18n::tr( "Engine Path Configuration" ) );

		auto *vbox = new QVBoxLayout( GetWidget() );
		{
		auto *frame = new QGroupBox( i18n::tr( "Path Settings" ) );
			vbox->addWidget( frame );

			auto *grid = new QGridLayout( frame );
			grid->setAlignment( Qt::AlignmentFlag::AlignTop );
			grid->setColumnStretch( 0, 111 );
			grid->setColumnStretch( 1, 333 );
			{
				const char* engine;
#if defined( WIN32 )
				engine = g_pGameDescription->getRequiredKeyValue( "engine_win32" );
#elif defined( __linux__ ) || defined ( __FreeBSD__ )
				engine = g_pGameDescription->getRequiredKeyValue( "engine_linux" );
#elif defined( __APPLE__ )
				engine = g_pGameDescription->getRequiredKeyValue( "engine_macos" );
#else
#error "unsupported platform"
#endif
				const auto text = StringStream( "Select directory, where game executable sits (typically ", Quoted( engine ), ")\n" );
				grid->addWidget( new QLabel( text.c_str() ), 0, 0, 1, 2 );
			}
			{
				PreferencesPage preferencesPage( *this, grid );
				Paths_constructPreferences( preferencesPage );
			}
		}
		{
			auto *buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok );
			vbox->addWidget( buttons );
			QObject::connect( buttons, &QDialogButtonBox::accepted, GetWidget(), &QDialog::accept );
		}
	}
};

PathsDialog g_PathsDialog;

static bool g_strEnginePath_was_empty_1st_start = false;
static bool g_startup_onboarding_completed = false;
static int g_editor_style_preference = 0; // 0 = NetRadiant
static bool g_startup_setup_wizard_has_run = false;
static bool g_startup_setup_wizard_in_progress = false;

namespace
{
const char* k_global_pref_filename = "global.pref";
constexpr int k_editor_style_netradiant = 0;

bool Startup_hasSavedPreferences(){
	const auto globalPrefPath = StringStream( g_Preferences.m_global_rc_path, k_global_pref_filename );
	return file_exists( globalPrefPath.c_str() ) && file_exists( g_Preferences.m_inipath.c_str() );
}

bool Startup_hasValidGameInstallation(){
	return file_is_directory( g_strEnginePath.c_str() );
}

bool Startup_selectDetectedGameInstallation(){
	const auto rule = EnginePath_buildInstallRule();
	auto effectiveRule = rule;
	EnginePath_applyLegacyHints( effectiveRule );

	for ( const auto& install : g_detectedEngineInstalls )
	{
		if ( !EnginePath_hasGameDataAt( install.path.c_str(), effectiveRule ) ) {
			continue;
		}
		g_strEnginePath = install.path;
		g_strEnginePath_was_empty_1st_start = false;
		globalOutputStream() << "Startup setup auto-detected game installation (" << install.source
		                     << "): " << Quoted( g_strEnginePath ) << '\n';
		return true;
	}
	return false;
}

bool Startup_tryAutoDetectGameInstallation(){
	EnginePath_refreshDetectedInstalls();
	return Startup_selectDetectedGameInstallation();
}

void Startup_promptEditorStyle(){
	QMessageBox dialog;
	dialog.setWindowTitle( i18n::tr( "VibeRadiant Setup" ) );
	dialog.setText( i18n::tr( "Choose your preferred editor style." ) );
	dialog.setInformativeText( i18n::tr( "Currently available: NetRadiant." ) );
	QAbstractButton* netRadiantButton = dialog.addButton( i18n::tr( "Use NetRadiant" ), QMessageBox::AcceptRole );
	dialog.addButton( i18n::tr( "Decide Later" ), QMessageBox::RejectRole );
	dialog.exec();

	if ( dialog.clickedButton() == netRadiantButton ) {
		g_editor_style_preference = k_editor_style_netradiant;
		g_startup_onboarding_completed = true;
	}
}

void Startup_runSetupWizardIfNeeded(){
	if ( g_startup_setup_wizard_has_run || g_startup_setup_wizard_in_progress ) {
		return;
	}

	g_startup_setup_wizard_in_progress = true;
	g_startup_setup_wizard_has_run = true;

	const bool prefsMissing = !Startup_hasSavedPreferences();
	const bool installMissing = !Startup_hasValidGameInstallation();
	const bool onboardingMissing = !g_startup_onboarding_completed;
	if ( !prefsMissing && !installMissing && !onboardingMissing ) {
		g_startup_setup_wizard_in_progress = false;
		return;
	}

	globalOutputStream() << "Running startup setup wizard (prefsMissing=" << ( prefsMissing ? 1 : 0 )
	                     << ", installMissing=" << ( installMissing ? 1 : 0 )
	                     << ", onboardingMissing=" << ( onboardingMissing ? 1 : 0 ) << ")\n";

	if ( installMissing ) {
		Startup_tryAutoDetectGameInstallation();
	}

	if ( !g_startup_onboarding_completed && Startup_hasValidGameInstallation() ) {
		Startup_promptEditorStyle();
	}

	g_startup_setup_wizard_in_progress = false;
}
} // namespace

void Startup_PreMainWindowSetup(){
	Startup_runSetupWizardIfNeeded();
}

void EnginePath_verify(){
	bool needsPrompt = !file_exists( g_strEnginePath.c_str() ) || g_strEnginePath_was_empty_1st_start;
	if ( needsPrompt ) {
		EnginePath_refreshDetectedInstalls();
		const auto rule = EnginePath_buildInstallRule();
		auto effectiveRule = rule;
		EnginePath_applyLegacyHints( effectiveRule );
		const DetectedEngineInstall* autoSelection = nullptr;
		int validInstallCount = 0;
		for ( const auto& install : g_detectedEngineInstalls )
		{
			if ( EnginePath_hasGameDataAt( install.path.c_str(), effectiveRule ) ) {
				++validInstallCount;
				autoSelection = &install;
			}
		}
		if ( validInstallCount == 1 && autoSelection != nullptr ) {
			g_strEnginePath = autoSelection->path;
			g_strEnginePath_was_empty_1st_start = false;
			needsPrompt = false;
			globalOutputStream() << "Auto-selected detected installation (" << autoSelection->source
			                     << ") " << Quoted( g_strEnginePath ) << '\n';
		}
	}
	if ( needsPrompt ) {
		if ( Startup_selectDetectedGameInstallation() ) {
			needsPrompt = false;
		}
	}
	if ( needsPrompt ) {
		qt_MessageBox(
			MainFrame_getWindow(),
			"Unable to continue: no valid game installation is configured for this game profile.\n"
			"Please configure an installation in the game setup manager and restart VibeRadiant.",
			"VibeRadiant Setup",
			EMessageBoxType::Error
		);
		Error( "No valid game installation configured for %s", g_pGameDescription->mGameFile.c_str() );
	}
	installDevFiles(); // try this anytime, as engine path may be set via command line or -gamedetect
}

namespace
{
CopiedString g_gamename;
CopiedString g_gamemode;
ModuleObservers g_gameNameObservers;
ModuleObservers g_gameModeObservers;
}

void Radiant_attachGameNameObserver( ModuleObserver& observer ){
	g_gameNameObservers.attach( observer );
}

void Radiant_detachGameNameObserver( ModuleObserver& observer ){
	g_gameNameObservers.detach( observer );
}

const char* basegame_get(){
	return g_pGameDescription->getRequiredKeyValue( "basegame" );
}

static const char* default_gamename_get(){
	if ( g_pGameDescription == nullptr ) {
		return "";
	}

	if ( const char* startupGameName = StartupGameInstallationGameName_get(); !string_empty( startupGameName )
	  && ( g_gamename.empty() || path_equal( g_gamename.c_str(), basegame_get() ) ) ) {
		return startupGameName;
	}

	const char* defaultGameName = g_pGameDescription->getKeyValue( "defaultgamename" );
	if ( !string_empty( defaultGameName )
	  && ( g_gamename.empty() || path_equal( g_gamename.c_str(), basegame_get() ) ) ) {
		return defaultGameName;
	}

	return basegame_get();
}

const char* gamename_get(){
	if ( g_gamename.empty() ) {
		return default_gamename_get();
	}
	return g_gamename.c_str();
}

void gamename_set( const char* gamename ){
	if ( !string_equal( gamename, g_gamename.c_str() ) ) {
		g_gameNameObservers.unrealise();
		g_gamename = gamename;
		g_gameNameObservers.realise();
	}
}

void Radiant_attachGameModeObserver( ModuleObserver& observer ){
	g_gameModeObservers.attach( observer );
}

void Radiant_detachGameModeObserver( ModuleObserver& observer ){
	g_gameModeObservers.detach( observer );
}

const char* gamemode_get(){
	return g_gamemode.c_str();
}

void gamemode_set( const char* gamemode ){
	if ( !string_equal( gamemode, g_gamemode.c_str() ) ) {
		g_gameModeObservers.unrealise();
		g_gamemode = gamemode;
		g_gameModeObservers.realise();
	}
}

#include "os/dir.h"

const char* const c_library_extension =
#if defined( WIN32 )
    "dll"
#elif defined ( __APPLE__ )
    "dylib"
#elif defined( __linux__ ) || defined ( __FreeBSD__ )
    "so"
#endif
    ;

void Radiant_loadModules( const char* path ){
	Directory_forEach( path, matchFileExtension( c_library_extension, [&]( const char *name ){
		char fullname[1024];
		ASSERT_MESSAGE( strlen( path ) + strlen( name ) < 1024, "" );
		strcpy( fullname, path );
		strcat( fullname, name );
		globalOutputStream() << "Found " << SingleQuoted( fullname ) << '\n';
		GlobalModuleServer_loadModule( fullname );
	}));
}

void Radiant_loadModulesFromRoot( const char* directory ){
	Radiant_loadModules( StringStream( directory, g_pluginsDir ) );

	if ( !string_equal( g_pluginsDir, g_modulesDir ) ) {
		Radiant_loadModules( StringStream( directory, g_modulesDir ) );
	}
}


class WorldspawnColourEntityClassObserver : public ModuleObserver
{
	std::size_t m_unrealised;
public:
	WorldspawnColourEntityClassObserver() : m_unrealised( 1 ){
	}
	void realise() override {
		if ( --m_unrealised == 0 ) {
			SetWorldspawnColour( g_xywindow_globals.color_brushes );
		}
	}
	void unrealise() override {
		if ( ++m_unrealised == 1 ) {
		}
	}
};

WorldspawnColourEntityClassObserver g_WorldspawnColourEntityClassObserver;


ModuleObservers g_gameToolsPathObservers;

void Radiant_attachGameToolsPathObserver( ModuleObserver& observer ){
	g_gameToolsPathObservers.attach( observer );
}

void Radiant_detachGameToolsPathObserver( ModuleObserver& observer ){
	g_gameToolsPathObservers.detach( observer );
}

void Radiant_Initialise(){
	GlobalModuleServer_Initialise();

	Radiant_loadModulesFromRoot( AppPath_get() );

	Preferences_Load();

	bool success = Radiant_Construct( GlobalModuleServer_get() );
	ASSERT_MESSAGE( success, "module system failed to initialise - see viberadiant.log for error messages" );

	g_gameToolsPathObservers.realise();
	g_gameModeObservers.realise();
	g_gameNameObservers.realise();
}

void Radiant_Shutdown(){
	g_gameNameObservers.unrealise();
	g_gameModeObservers.unrealise();
	g_gameToolsPathObservers.unrealise();

	if ( !g_preferences_globals.disable_ini ) {
		globalOutputStream() << "Start writing prefs\n";
		Preferences_Save();
		globalOutputStream() << "Done prefs\n";
	}

	Radiant_Destroy();

	GlobalModuleServer_Shutdown();
}

void Exit(){
	if ( ConfirmModified( "Exit VibeRadiant" ) ) {
		QCoreApplication::quit();
	}
}

#ifdef WIN32
#include <process.h>
#else
#include <spawn.h>
/* According to the Single Unix Specification, environ is not
 * in any system header, although unistd.h often declares it.
 */
extern char **environ;
#endif
void Radiant_Restart(){
	if( ConfirmModified( "Restart VibeRadiant" ) ){
		const auto mapname = StringStream( Quoted( Map_Name( g_map ) ) );

		char *argv[] = { string_clone( environment_get_app_filepath() ),
	                     Map_Unnamed( g_map )? nullptr : string_clone( mapname ),
	                     nullptr };
#ifdef WIN32
		const int status = !_spawnv( P_NOWAIT, argv[0], argv );
#else
		const int status = posix_spawn( nullptr, argv[0], nullptr, nullptr, argv, environ );
#endif

		// quit if radiant successfully started
		if ( status == 0 ) {
			QCoreApplication::quit();
		}
	}
}


void Restart(){
	// Freeze the current preference callbacks before modules register them again.
	// Startup normally defers this dialog until first use, but an in-process module
	// reload must preserve the single-construction lifecycle it historically had.
	PreferencesDialog_constructWindow( MainFrame_getWindow() );

	PluginsMenu_clear();
	PluginToolbar_clear();

	Radiant_Shutdown();
	Radiant_Initialise();

	PluginsMenu_populate();

	PluginToolbar_populate();
}


void OpenUpdateURL(){
	UpdateManager_CheckForUpdates( UpdateCheckMode::Manual );
}

// open the Q3Rad manual
void OpenHelpURL(){
	// at least on win32, AppPath + "docs/index.html"
	OpenURL( StringStream( AppPath_get(), "docs/index.html" ) );
}

void OpenBugReportURL(){
	// OpenURL( "http://www.icculus.org/netradiant/?cmd=bugs" );
	OpenURL( "https://github.com/themuffinator/VibeRadiant/issues" );
}


QWidget* g_page_console;

void Console_ToggleShow(){
	if( g_page_console != nullptr ){
		GroupDialog_showPage( g_page_console );
		return;
	}

	Console_setCollapsed( !Console_isCollapsed() );
}

QWidget* g_page_entity;

void EntityInspector_ToggleShow(){
	GroupDialog_showPage( g_page_entity );
}

QWidget* g_page_layers;

void LayersBrowser_ToggleShow(){
	GroupDialog_showPage( g_page_layers);
}

QWidget* g_page_issues;

void IssueBrowser_ToggleShow(){
	GroupDialog_showPage( g_page_issues );
}

QWidget* g_page_uvview;

void UVView_ToggleShow(){
	GroupDialog_showPage( g_page_uvview );
}

namespace
{
enum class WorkspacePreset
{
	Modeling = 0,
	Texturing,
	Entity,
	Lighting,
};

bool g_focusMode = false;

struct FocusModeState
{
	bool groupDialogShown = true;
	bool entityListShown = false;
	bool surfaceInspectorShown = false;
	bool statusBarShown = true;
	std::vector<std::pair<QPointer<QToolBar>, bool>> toolbars;
} g_focusModeState;

ToggleItem g_focusModeItem{ BoolExportCaller( g_focusMode ) };

const char* workspace_preset_name( WorkspacePreset preset ){
	switch( preset )
	{
	case WorkspacePreset::Modeling: return "Modeling";
	case WorkspacePreset::Texturing: return "Texturing";
	case WorkspacePreset::Entity: return "Entity";
	case WorkspacePreset::Lighting: return "Lighting";
	default: return "Workspace";
	}
}

void workspace_focus_cue( QWidget* widget ){
	if( widget == nullptr ){
		return;
	}

	widget->setProperty( "workspaceFocus", true );
	widget->style()->unpolish( widget );
	widget->style()->polish( widget );
	widget->update();

	QTimer::singleShot( 700, widget, [widget](){
		widget->setProperty( "workspaceFocus", false );
		widget->style()->unpolish( widget );
		widget->style()->polish( widget );
		widget->update();
	} );
}

void FocusMode_set( bool enabled ){
	if( g_focusMode == enabled ){
		return;
	}

	g_focusMode = enabled;
	g_focusModeItem.update();

	if( g_pParentWnd == nullptr || g_pParentWnd->m_window == nullptr ){
		return;
	}
	QMainWindow* mainWindow = g_pParentWnd->m_window;

	if( enabled ){
		g_focusModeState.groupDialogShown = GroupDialog_isShown();
		g_focusModeState.entityListShown = EntityList_isShown();
		g_focusModeState.surfaceInspectorShown = SurfaceInspector_isShown();
		if( QStatusBar* status = mainWindow->statusBar() ){
			g_focusModeState.statusBarShown = status->isVisible();
		}

		g_focusModeState.toolbars.clear();
		for( QToolBar* toolbar : mainWindow->findChildren<QToolBar*>() )
		{
			g_focusModeState.toolbars.emplace_back( toolbar, toolbar->isVisible() );
			toolbar->setVisible( false );
		}

		GroupDialog_setShown( false );
		EntityList_setShown( false );
		SurfaceInspector_setShown( false );
		if( QStatusBar* status = mainWindow->statusBar() ){
			status->setVisible( false );
		}
	}
	else
	{
		for( const auto& [toolbar, shown] : g_focusModeState.toolbars )
		{
			if( toolbar ){
				toolbar->setVisible( shown );
			}
		}
		if( QStatusBar* status = mainWindow->statusBar() ){
			status->setVisible( g_focusModeState.statusBarShown );
		}

		GroupDialog_setShown( g_focusModeState.groupDialogShown );
		EntityList_setShown( g_focusModeState.entityListShown );
		SurfaceInspector_setShown( g_focusModeState.surfaceInspectorShown );
	}
}

void FocusMode_toggle(){
	FocusMode_set( !g_focusMode );
}
}

void WorkspacePreset_apply( WorkspacePreset preset ){
	if( GroupDialog_getWindow() == nullptr ){
		return;
	}

	const auto presentPage = []( QWidget* page ){
		if( page != nullptr ){
			GroupDialog_presentPage( page );
			workspace_focus_cue( page );
		}
		else{
			GroupDialog_setShown( true );
		}
	};

	const bool keepPanelsHidden = g_focusMode;

	switch( preset )
	{
	case WorkspacePreset::Modeling:
		presentPage( g_page_layers != nullptr ? g_page_layers : g_page_entity );
		SurfaceInspector_setShown( false );
		EntityList_setShown( false );
		CamWnd_setLightingPreviewEnabled( false );
		break;
	case WorkspacePreset::Texturing:
		presentPage( g_page_textures != nullptr ? g_page_textures : g_page_entity );
		SurfaceInspector_setShown( keepPanelsHidden ? false : true );
		EntityList_setShown( false );
		break;
	case WorkspacePreset::Entity:
		presentPage( g_page_entity );
		EntityList_setShown( keepPanelsHidden ? false : true );
		SurfaceInspector_setShown( false );
		break;
	case WorkspacePreset::Lighting:
		presentPage( g_page_issues != nullptr ? g_page_issues : g_page_entity );
		SurfaceInspector_setShown( keepPanelsHidden ? false : true );
		EntityList_setShown( false );
		CamWnd_setLightingPreviewEnabled( true );
		break;
	}

	if( keepPanelsHidden ){
		GroupDialog_setShown( false );
	}

	if( g_pParentWnd != nullptr && g_pParentWnd->m_window != nullptr ){
		if( QStatusBar* status = g_pParentWnd->m_window->statusBar() ){
			status->showMessage(
				QString::fromLatin1(
					StringStream( "Workspace: ", workspace_preset_name( preset ),
					              keepPanelsHidden ? " (Focus Mode)" : "" ).c_str()
				),
				1800
			);
		}
	}
}

void WorkspacePreset_Modeling(){
	WorkspacePreset_apply( WorkspacePreset::Modeling );
}

void WorkspacePreset_Texturing(){
	WorkspacePreset_apply( WorkspacePreset::Texturing );
}

void WorkspacePreset_Entity(){
	WorkspacePreset_apply( WorkspacePreset::Entity );
}

void WorkspacePreset_Lighting(){
	WorkspacePreset_apply( WorkspacePreset::Lighting );
}


static class EverySecondTimer
{
	QTimer m_timer;
public:
	EverySecondTimer(){
		m_timer.setInterval( 1000 );
		m_timer.callOnTimeout( [](){
			if ( QGuiApplication::mouseButtons().testFlag( Qt::MouseButton::NoButton ) ) {
				QE_CheckAutoSave();
			}
		} );
	}
	void enable(){
		m_timer.start();
	}
	void disable(){
		m_timer.stop();
	}
}
s_qe_every_second_timer;


class WaitDialog
{
public:
	QWidget* m_window;
	QLabel* m_label;
};

WaitDialog create_wait_dialog( const char* title, const char* text ){
	/* Qt::Tool window type doesn't steal focus, which saves e.g. from losing freelook camera mode on autosave
	   or entity menu from hiding, while clicked with ctrl, by tex/model loading popup.
	   Qt::WidgetAttribute::WA_ShowWithoutActivating is implied, but lets have it set too. */
	auto *window = new QWidget( MainFrame_getWindow(), Qt::Tool | Qt::WindowTitleHint );
	window->setWindowTitle( title );
	window->setWindowModality( Qt::WindowModality::ApplicationModal );
	window->setAttribute( Qt::WidgetAttribute::WA_ShowWithoutActivating );

	auto *label = new QLabel( text );
	{
		auto *box = new QHBoxLayout( window );
		box->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
		box->setContentsMargins( 20, 5, 20, 3 );
		box->addWidget( label );
		label->setMinimumWidth( 200 );
	}
	return WaitDialog{ .m_window = window, .m_label = label };
}

namespace
{
clock_t g_lastRedrawTime = 0;
const clock_t c_redrawInterval = clock_t( CLOCKS_PER_SEC / 10 );

bool redrawRequired(){
	clock_t currentTime = std::clock();
	if ( currentTime - g_lastRedrawTime >= c_redrawInterval ) {
		g_lastRedrawTime = currentTime;
		return true;
	}
	return false;
}
}

typedef std::list<CopiedString> StringStack;
StringStack g_wait_stack;
WaitDialog g_wait;

bool ScreenUpdates_Enabled(){
	return g_wait_stack.empty();
}

void ScreenUpdates_process(){
	if ( redrawRequired() ) {
		process_gui();
	}
}


void ScreenUpdates_Disable( const char* message, const char* title ){
	if ( g_wait_stack.empty() ) {
		s_qe_every_second_timer.disable();

		process_gui();

		g_wait = create_wait_dialog( title, message );

		g_wait.m_window->show();
		ScreenUpdates_process();
	}
	else {
		g_wait.m_window->setWindowTitle( title );
		g_wait.m_label->setText( message );
		ScreenUpdates_process();
	}
	g_wait_stack.push_back( message );
}

void ScreenUpdates_Enable(){
	ASSERT_MESSAGE( !ScreenUpdates_Enabled(), "screen updates already enabled" );
	g_wait_stack.pop_back();
	if ( g_wait_stack.empty() ) {
		s_qe_every_second_timer.enable();

		delete std::exchange( g_wait.m_window, nullptr );
	}
	else {
		g_wait.m_label->setText( g_wait_stack.back().c_str() );
		ScreenUpdates_process();
	}
}



void GlobalCamera_UpdateWindow(){
	if ( g_pParentWnd != 0 ) {
		CamWnd_Update( *g_pParentWnd->GetCamWnd() );
	}
}

void XY_UpdateAllWindows(){
	if ( g_pParentWnd != 0 ) {
		g_pParentWnd->forEachXYWnd( []( XYWnd* xywnd ){
			XYWnd_Update( *xywnd );
		} );
		if ( ZWnd* zwnd = g_pParentWnd->GetZWnd() ) {
			zwnd->queueDraw();
		}
	}
}

void UpdateAllWindows(){
	GlobalCamera_UpdateWindow();
	XY_UpdateAllWindows();
}


LatchedInt g_Layout_viewStyle( 0, "Window Layout" );
LatchedBool g_Layout_enableDetachableMenus( true, "Detachable Menus" );
LatchedBool g_Layout_builtInGroupDialog( false, "Built-In Group Dialog" );



void create_file_menu( QMenuBar *menubar ){
	// File menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&File" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_menu_item_with_mnemonic( menu, "&New Map", "NewMap" );
	menu->addSeparator();

	create_menu_item_with_mnemonic( menu, "&Open...", "OpenMap" );
	create_menu_item_with_mnemonic( menu, "&Import...", "ImportMap" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "&Save", "SaveMap" );
	create_menu_item_with_mnemonic( menu, "Save &as...", "SaveMapAs" );
	create_menu_item_with_mnemonic( menu, "Save s&elected...", "SaveSelected" );
	create_menu_item_with_mnemonic( menu, "Save re&gion...", "SaveRegion" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "&Pointfile", "TogglePointfile" );
	menu->addSeparator();
	MRU_constructMenu( menu );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "E&xit", "Exit" );
}

void create_edit_menu( QMenuBar *menubar ){
	// Edit menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&Edit" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_menu_item_with_mnemonic( menu, "&Undo", "Undo" );
	create_menu_item_with_mnemonic( menu, "&Redo", "Redo" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "&Copy", "Copy" );
	create_menu_item_with_mnemonic( menu, "&Paste", "Paste" );
	create_menu_item_with_mnemonic( menu, "P&aste To Camera", "PasteToCamera" );
	create_menu_item_with_mnemonic( menu, "Move To Camera", "MoveToCamera" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "&Duplicate", "CloneSelection" );
	create_menu_item_with_mnemonic( menu, "Duplicate, make uni&que", "CloneSelectionAndMakeUnique" );
	create_menu_item_with_mnemonic( menu, "Create &Linked Duplicate", "CreateLinkedDuplicate" );
	create_menu_item_with_mnemonic( menu, "Select Linked &Groups", "SelectLinkedGroups" );
	create_menu_item_with_mnemonic( menu, "Separate Linked &Groups", "SeparateLinkedGroups" );
	create_menu_item_with_mnemonic( menu, "D&elete", "DeleteSelection" );
	//create_menu_item_with_mnemonic( menu, "Pa&rent", "ParentSelection" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "C&lear Selection", "UnSelectSelection" );
	create_menu_item_with_mnemonic( menu, "&Invert Selection", "InvertSelection" );
	create_menu_item_with_mnemonic( menu, "Select i&nside", "SelectInside" );
	create_menu_item_with_mnemonic( menu, "Select &touching", "SelectTouching" );

	menu->addSeparator();

	create_menu_item_with_mnemonic( menu, "Select All Of Type", "SelectAllOfType" );
	create_menu_item_with_mnemonic( menu, "Select Textured", "SelectTextured" );
	create_menu_item_with_mnemonic( menu, "&Expand Selection To Primitives", "ExpandSelectionToPrimitives" );
	create_menu_item_with_mnemonic( menu, "&Expand Selection To Entities", "ExpandSelectionToEntities" );
	create_menu_item_with_mnemonic( menu, "&Expand Selection To Layers", "ExpandSelectionToLayers" );
	create_menu_item_with_mnemonic( menu, "Select Connected Entities", "SelectConnectedEntities" );

	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Macros" ) );
		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
		create_menu_item_with_mnemonic( submenu, "Start Recording", "MacroRecordStart" );
		create_menu_item_with_mnemonic( submenu, "Stop Recording", "MacroRecordStop" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Play Recorded Macro", "MacroPlay" );
		create_menu_item_with_mnemonic( submenu, "Clear Recorded Macro", "MacroClear" );
	}

	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Command Palette...", "CommandPalette" );
	create_menu_item_with_mnemonic( menu, "Pre&ferences...", "Preferences" );
}

void create_view_menu( QMenuBar *menubar, MainFrame::EViewStyle style ){
	// View menu
	QMenu *menu = menubar->addMenu( i18n::tr( "Vie&w" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	bool hasPreCameraSection = false;
	if ( style == MainFrame::eFloating ) {
		create_check_menu_item_with_mnemonic( menu, "Camera View", "ToggleCamera" );
		create_check_menu_item_with_mnemonic( menu, "XY (Top) View", "ToggleView" );
		create_check_menu_item_with_mnemonic( menu, "XZ (Front) View", "ToggleFrontView" );
		create_check_menu_item_with_mnemonic( menu, "YZ (Side) View", "ToggleSideView" );
		hasPreCameraSection = true;
	}
	if ( hasPreCameraSection ) {
		menu->addSeparator();
	}
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Camera" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Focus on Selected", "CameraFocusOnSelected" );
		create_menu_item_with_mnemonic( submenu, "&Center", "CenterView" );
		create_menu_item_with_mnemonic( submenu, "&Up Floor", "UpFloor" );
		create_menu_item_with_mnemonic( submenu, "&Down Floor", "DownFloor" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Far Clip Plane In", "CubicClipZoomIn" );
		create_menu_item_with_mnemonic( submenu, "Far Clip Plane Out", "CubicClipZoomOut" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Toggle Lighting Preview", "TogglePreview" );
		create_check_menu_item_with_mnemonic( submenu, "Animate Shaders", "AnimateShaders" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Next Leak Spot", "NextLeakSpot" );
		create_menu_item_with_mnemonic( submenu, "Previous Leak Spot", "PrevLeakSpot" );
		//cameramodel is not implemented in instances, thus useless
//		submenu->addSeparator();
//		create_menu_item_with_mnemonic( submenu, "Look Through Selected", "LookThroughSelected" );
//		create_menu_item_with_mnemonic( submenu, "Look Through Camera", "LookThroughCamera" );
	}
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Orthographic" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		if ( style == MainFrame::eRegular || style == MainFrame::eRegularLeft || style == MainFrame::eFloating ) {
			create_menu_item_with_mnemonic( submenu, "&Next (XY, XZ, YZ)", "NextView" );
			create_menu_item_with_mnemonic( submenu, "XY (Top)", "ViewTop" );
			create_menu_item_with_mnemonic( submenu, "XZ (Front)", "ViewFront" );
			create_menu_item_with_mnemonic( submenu, "YZ (Side)", "ViewSide" );
			submenu->addSeparator();
		}
		else{
			create_menu_item_with_mnemonic( submenu, "Center on Selected", "NextView" );
		}

		create_menu_item_with_mnemonic( submenu, "Focus on Selected", "XYFocusOnSelected" );
		create_menu_item_with_mnemonic( submenu, "Center on Selected", "CenterXYView" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "&XY 100%", "Zoom100" );
		create_menu_item_with_mnemonic( submenu, "XY Zoom &In", "ZoomIn" );
		create_menu_item_with_mnemonic( submenu, "XY Zoom &Out", "ZoomOut" );
	}

	menu->addSeparator();

	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Show" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_check_menu_item_with_mnemonic( submenu, "Show Entity &Angles", "ShowAngles" );
		create_check_menu_item_with_mnemonic( submenu, "Show Entity &Names", "ShowNames" );
		create_check_menu_item_with_mnemonic( submenu, "Show Light Radiuses", "ShowLightRadiuses" );
		create_check_menu_item_with_mnemonic( submenu, "Show Entity Boxes", "ShowBboxes" );
		create_check_menu_item_with_mnemonic( submenu, "Show Entity Connections", "ShowConnections" );
		create_check_menu_item_with_mnemonic( submenu, "Thick Connection Lines", "ShowConnectionsThick" );

		submenu->addSeparator();

		create_check_menu_item_with_mnemonic( submenu, "Show 2D Size Info", "ShowSize2d" );
		create_check_menu_item_with_mnemonic( submenu, "Show 3D Size Info", "ShowSize3d" );
		create_check_menu_item_with_mnemonic( submenu, "Show Crosshair", "ToggleCrosshairs" );
		create_check_menu_item_with_mnemonic( submenu, "Show Grid", "ToggleGrid" );
		create_check_menu_item_with_mnemonic( submenu, "Show Blocks", "ShowBlocks" );
		create_check_menu_item_with_mnemonic( submenu, "Show C&oordinates", "ShowCoordinates" );
		create_check_menu_item_with_mnemonic( submenu, "Show Window Outline", "ShowWindowOutline" );
		create_check_menu_item_with_mnemonic( submenu, "Show Axes", "ShowAxes" );
		create_check_menu_item_with_mnemonic( submenu, "Show Camera Ortho Lines", "ShowCameraOrthoLines" );
		create_check_menu_item_with_mnemonic( submenu, "Show 2D Workzone", "ShowWorkzone2d" );
		create_check_menu_item_with_mnemonic( submenu, "Show Clipper Ortho Debug", "ShowClipperOrthoDebug" );
		create_check_menu_item_with_mnemonic( submenu, "Show 3D Workzone", "ShowWorkzone3d" );
		create_check_menu_item_with_mnemonic( submenu, "Show Renderer Stats", "ShowStats" );
	}

	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Filter" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		Filters_constructMenu( submenu );
	}
	menu->addSeparator();
	{
		create_check_menu_item_with_mnemonic( menu, "Hide Selected", "HideSelected" );
		create_menu_item_with_mnemonic( menu, "Show Hidden", "ShowHidden" );
	}
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Region" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "&Off", "RegionOff" );
		create_menu_item_with_mnemonic( submenu, "&Set XY", "RegionSetXY" );
		create_menu_item_with_mnemonic( submenu, "Set &Brush", "RegionSetBrush" );
		create_check_menu_item_with_mnemonic( submenu, "Set Se&lection", "RegionSetSelection" );
	}

	//command_connect_accelerator( "CenterXYView" );
}

void create_window_menu( QMenuBar *menubar, MainFrame::EViewStyle style ){
	QMenu *menu = menubar->addMenu( i18n::tr( "&Window" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_menu_item_with_mnemonic( menu, "Command Palette...", "CommandPalette" );
	create_menu_item_with_mnemonic( menu, "Pre&ferences...", "Preferences" );
	menu->addSeparator();

	if ( style != MainFrame::eRegular && style != MainFrame::eRegularLeft ) {
		create_menu_item_with_mnemonic( menu, "Toggle Console", "ToggleConsole" );
	}
	if ( AssetBrowser_isEnabled() && ( ( style != MainFrame::eRegular && style != MainFrame::eRegularLeft ) || g_Layout_builtInGroupDialog.m_value ) ) {
		create_menu_item_with_mnemonic( menu, "Toggle Asset Browser", "ToggleTextures" );
	}
	create_menu_item_with_mnemonic( menu, "Toggle Entity Inspector", "ToggleEntityInspector" );
	create_menu_item_with_mnemonic( menu, "Toggle Layers Browser", "ToggleLayersBrowser" );
	create_menu_item_with_mnemonic( menu, "Toggle Issue Browser", "ToggleIssueBrowser" );
	create_menu_item_with_mnemonic( menu, "Toggle UV View", "ToggleUVView" );
	create_menu_item_with_mnemonic( menu, "Toggle Surface Inspector", "SurfaceInspector" );
	create_menu_item_with_mnemonic( menu, "Toggle Entity List", "ToggleEntityList" );

	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Workspace Presets" ) );
		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
		create_menu_item_with_mnemonic( submenu, "Apply Modeling Workspace", "WorkspacePresetModeling" );
		create_menu_item_with_mnemonic( submenu, "Apply Texturing Workspace", "WorkspacePresetTexturing" );
		create_menu_item_with_mnemonic( submenu, "Apply Entity Workspace", "WorkspacePresetEntity" );
		create_menu_item_with_mnemonic( submenu, "Apply Lighting Workspace", "WorkspacePresetLighting" );
	}
	create_check_menu_item_with_mnemonic( menu, "Focus Mode", "FocusMode" );

	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Fullscreen", "Fullscreen" );
	create_menu_item_with_mnemonic( menu, "Maximize View", "MaximizeView" );
}

void create_selection_menu( QMenuBar *menubar ){
	// Selection menu
	QMenu *menu = menubar->addMenu( i18n::tr( "M&odify" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Components" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_check_menu_item_with_mnemonic( submenu, "&Primitives", "SelectPrimitives" );
		create_check_menu_item_with_mnemonic( submenu, "&Edges", "DragEdges" );
		create_check_menu_item_with_mnemonic( submenu, "&Vertices", "DragVertices" );
		create_check_menu_item_with_mnemonic( submenu, "&Faces", "DragFaces" );
	}

	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Snap To Grid", "SnapToGrid" );

	menu->addSeparator();

	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Nudge" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Nudge Left", "SelectNudgeLeft" );
		create_menu_item_with_mnemonic( submenu, "Nudge Right", "SelectNudgeRight" );
		create_menu_item_with_mnemonic( submenu, "Nudge Up", "SelectNudgeUp" );
		create_menu_item_with_mnemonic( submenu, "Nudge Down", "SelectNudgeDown" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Nudge +Z", "MoveSelectionUP" );
		create_menu_item_with_mnemonic( submenu, "Nudge -Z", "MoveSelectionDOWN" );
	}
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Rotate" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Rotate X", "RotateSelectionX" );
		create_menu_item_with_mnemonic( submenu, "Rotate Y", "RotateSelectionY" );
		create_menu_item_with_mnemonic( submenu, "Rotate Z", "RotateSelectionZ" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Rotate Clockwise", "RotateSelectionClockwise" );
		create_menu_item_with_mnemonic( submenu, "Rotate Anticlockwise", "RotateSelectionAnticlockwise" );
	}
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Flip" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Flip &X", "MirrorSelectionX" );
		create_menu_item_with_mnemonic( submenu, "Flip &Y", "MirrorSelectionY" );
		create_menu_item_with_mnemonic( submenu, "Flip &Z", "MirrorSelectionZ" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Flip Horizontally", "MirrorSelectionHorizontally" );
		create_menu_item_with_mnemonic( submenu, "Flip Vertically", "MirrorSelectionVertically" );
	}
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Arbitrary rotation...", "ArbitraryRotation" );
	create_menu_item_with_mnemonic( menu, "Arbitrary scale...", "ArbitraryScale" );
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( i18n::tr( "Repeat" ) );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Repeat Transforms", "RepeatTransforms" );

		using SetTextCB = PointerCaller<QAction, void(const char*), +[]( QAction *action, const char *text ){ action->setText( text ); }>;
		const auto addItem = [submenu]<SelectionSystem::EManipulatorMode mode>() -> SetTextCB {
			return SetTextCB( create_menu_item_with_mnemonic( submenu, "", makeCallbackF( +[](){ GlobalSelectionSystem().resetTransforms( mode ); } ) ) );
		};
		SelectionSystem_connectTransformsCallbacks( { addItem.operator()<SelectionSystem::eTranslate>(),
		                                              addItem.operator()<SelectionSystem::eRotate>(),
		                                              addItem.operator()<SelectionSystem::eScale>(),
		                                              addItem.operator()<SelectionSystem::eSkew>() } );
		GlobalSelectionSystem().resetTransforms(); // init texts immediately

		create_menu_item_with_mnemonic( submenu, "Reset Transforms", "ResetTransforms" );
	}
}

void create_bsp_menu( QMenuBar *menubar ){
	// BSP menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&Build" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_menu_item_with_mnemonic( menu, "Run Build Task", "BuildTaskBuild" );
	create_menu_item_with_mnemonic( menu, "Run Launch Task", "BuildTaskLaunch" );
	create_menu_item_with_mnemonic( menu, "Run Build + Launch Task", "BuildTaskBuildAndLaunch" );
	create_menu_item_with_mnemonic( menu, "Run recent build (legacy)", "Build_runRecentExecutedBuild" );
	create_menu_item_with_mnemonic( menu, "Customize Tasks...", "BuildMenuCustomize" );

	menu->addSeparator();

	menu->setToolTipsVisible( true );
	Build_constructMenu( menu );

	g_bsp_menu = menu;
}

void create_grid_menu( QMenuBar *menubar ){
	// Grid menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&Grid" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	Grid_constructMenu( menu );
}

void create_map_menu( QMenuBar *menubar ){
	QMenu *menu = menubar->addMenu( i18n::tr( "&Map" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_menu_item_with_mnemonic( menu, "Find Brush...", "FindBrush" );
	create_menu_item_with_mnemonic( menu, "Map Info...", "MapInfo" );
	create_menu_item_with_mnemonic( menu, "Set 2D Background Image...", "Set2DBackgroundImage" );
}

void create_genai_menu( QMenuBar *menubar ){
	QMenu *menu = menubar->addMenu( i18n::tr( "&GenAI" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	create_check_menu_item_with_mnemonic( menu, "Enable GenAI", "GenAIEnable" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Prompt-to-Blockout...", "GenAIPromptToBlockout" );
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "GenAI Preferences...", "GenAIPreferences" );
	create_menu_item_with_mnemonic( menu, "Show GenAI Status", "GenAIStatus" );
	create_menu_item_with_mnemonic( menu, "Open OpenAI API Docs", "GenAIOpenAPIDocs" );
	create_menu_item_with_mnemonic( menu, "Clear Stored API Key", "GenAIClearAPIKey" );
}

void create_entity_menu( QMenuBar *menubar ){
	// Entity menu
	QMenu *menu = menubar->addMenu( i18n::tr( "E&ntity" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	Entity_constructMenu( menu );
}

void create_brush_menu( QMenuBar *menubar ){
	// Brush menu
	QMenu *menu = menubar->addMenu( i18n::tr( "Brush" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	Brush_constructMenu( menu );
}

void create_patch_menu( QMenuBar *menubar ){
	// Curve menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&Curve" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

	Patch_constructMenu( menu );
}

bool game_supports_curve_tools(){
	if ( string_equal( g_pGameDescription->getKeyValue( "no_patch" ), "1" ) ) {
		return false;
	}
	return !string_equal( g_pGameDescription->getKeyValue( "brushtypes" ), "quake2" );
}

void create_help_menu( QMenuBar *menubar ){
	// Help menu
	QMenu *menu = menubar->addMenu( i18n::tr( "&Help" ) );

	menu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

//	create_menu_item_with_mnemonic( menu, "Manual", "OpenManual" );

	// this creates all the per-game drop downs for the game pack helps
	// it will take care of hooking the Sys_OpenURL calls etc.
	create_game_help_menu( menu );

	create_menu_item_with_mnemonic( menu, "Bug report", makeCallbackF( OpenBugReportURL ) );
	create_menu_item_with_mnemonic( menu, "Check for VibeRadiant update", "CheckForUpdate" ); // FIXME
	create_menu_item_with_mnemonic( menu, "&About", makeCallbackF( DoAbout ) );
}

void create_main_menu( QMenuBar *menubar, MainFrame::EViewStyle style ){
	create_file_menu( menubar );
  	create_edit_menu( menubar );
	create_view_menu( menubar, style );
	create_window_menu( menubar, style );
	create_genai_menu( menubar );
	create_selection_menu( menubar );
	create_map_menu( menubar );
	create_bsp_menu( menubar );
	create_grid_menu( menubar );
	create_entity_menu( menubar );
	create_brush_menu( menubar );
	if ( game_supports_curve_tools() )
		create_patch_menu( menubar );
	create_plugins_menu( menubar );
	create_help_menu( menubar );
}


void Patch_registerShortcuts(){
	command_connect_accelerator( "InvertCurveTextureX" );
	command_connect_accelerator( "InvertCurveTextureY" );
	command_connect_accelerator( "PatchInsertInsertColumn" );
	command_connect_accelerator( "PatchInsertInsertRow" );
	command_connect_accelerator( "PatchDeleteLastColumn" );
	command_connect_accelerator( "PatchDeleteLastRow" );
	command_connect_accelerator( "NaturalizePatch" );
}

void Manipulators_registerShortcuts(){
	command_connect_accelerator( "MouseRotateOrScale" );
	command_connect_accelerator( "MouseDragOrTransform" );
}

void TexdefNudge_registerShortcuts(){
	command_connect_accelerator( "TexRotateClock" );
	command_connect_accelerator( "TexRotateCounter" );
	command_connect_accelerator( "TexScaleUp" );
	command_connect_accelerator( "TexScaleDown" );
	command_connect_accelerator( "TexScaleLeft" );
	command_connect_accelerator( "TexScaleRight" );
	command_connect_accelerator( "TexShiftUp" );
	command_connect_accelerator( "TexShiftDown" );
	command_connect_accelerator( "TexShiftLeft" );
	command_connect_accelerator( "TexShiftRight" );
}

void SelectNudge_registerShortcuts(){
	command_connect_accelerator( "MoveSelectionDOWN" );
	command_connect_accelerator( "MoveSelectionUP" );
	command_connect_accelerator( "SelectNudgeLeft" );
	command_connect_accelerator( "SelectNudgeRight" );
	command_connect_accelerator( "SelectNudgeUp" );
	command_connect_accelerator( "SelectNudgeDown" );
}

void SnapToGrid_registerShortcuts(){
	command_connect_accelerator( "SnapToGrid" );
}

void SelectByType_registerShortcuts(){
	command_connect_accelerator( "SelectAllOfType" );
}

void SurfaceInspector_registerShortcuts(){
	command_connect_accelerator( "FitTexture" );
	command_connect_accelerator( "FitTextureWidth" );
	command_connect_accelerator( "FitTextureHeight" );
	command_connect_accelerator( "FitTextureWidthOnly" );
	command_connect_accelerator( "FitTextureHeightOnly" );
	command_connect_accelerator( "TextureProjectAxial" );
	command_connect_accelerator( "TextureProjectOrtho" );
	command_connect_accelerator( "TextureProjectCam" );
}

void TexBro_registerShortcuts(){
	toggle_add_accelerator( "SearchFromStart" );
}

void Misc_registerShortcuts(){
	command_connect_accelerator( "Redo2" );
	command_connect_accelerator( "UnSelectSelection2" );
	command_connect_accelerator( "DeleteSelection2" );
	command_connect_accelerator( "DeleteSelection3" );
}


void register_shortcuts(){
//	Patch_registerShortcuts();
	Grid_registerShortcuts();
//	XYWnd_registerShortcuts();
	CamWnd_registerShortcuts();
	Manipulators_registerShortcuts();
	SurfaceInspector_registerShortcuts();
	TexdefNudge_registerShortcuts();
//	SelectNudge_registerShortcuts();
//	SnapToGrid_registerShortcuts();
//	SelectByType_registerShortcuts();
	if ( AssetBrowser_isEnabled() ) {
		TexBro_registerShortcuts();
	}
	Misc_registerShortcuts();
	Entity_registerShortcuts();
	Layers_registerShortcuts();
}

void File_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "Open an existing map", "file_open.png", "OpenMap" );
	toolbar_append_button( toolbar, "Save the active map", "file_save.png", "SaveMap" );
}

void UndoRedo_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "Undo", "undo.png", "Undo" );
	toolbar_append_button( toolbar, "Redo", "redo.png", "Redo" );
}

void RotateFlip_constructToolbar( QToolBar* toolbar ){
//	toolbar_append_button( toolbar, "x-axis Flip", "brush_flipx.png", "MirrorSelectionX" );
//	toolbar_append_button( toolbar, "x-axis Rotate", "brush_rotatex.png", "RotateSelectionX" );
//	toolbar_append_button( toolbar, "y-axis Flip", "brush_flipy.png", "MirrorSelectionY" );
//	toolbar_append_button( toolbar, "y-axis Rotate", "brush_rotatey.png", "RotateSelectionY" );
//	toolbar_append_button( toolbar, "z-axis Flip", "brush_flipz.png", "MirrorSelectionZ" );
//	toolbar_append_button( toolbar, "z-axis Rotate", "brush_rotatez.png", "RotateSelectionZ" );
	toolbar_append_button( toolbar, "Flip Horizontally", "brush_flip_hor.png", "MirrorSelectionHorizontally" );
	toolbar_append_button( toolbar, "Flip Vertically", "brush_flip_vert.png", "MirrorSelectionVertically" );

	toolbar_append_button( toolbar, "Rotate Anticlockwise", "brush_rotate_anti.png", "RotateSelectionAnticlockwise" );
	toolbar_append_button( toolbar, "Rotate Clockwise", "brush_rotate_clock.png", "RotateSelectionClockwise" );
}

void Select_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "Select touching", "selection_selecttouching.png", "SelectTouching" );
	toolbar_append_button( toolbar, "Select inside", "selection_selectinside.png", "SelectInside" );
}

void CSG_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "CSG Subtract", "selection_csgsubtract.png", "CSGSubtract" );
	toolbar_append_button( toolbar, "CSG Wrap Merge", "selection_csgmerge.png", "CSGWrapMerge" );
	toolbar_append_button( toolbar, "Room", "selection_makeroom.png", "CSGroom" );
	toolbar_append_button( toolbar, "CSG Tool", "ellipsis.png", "CSGTool" );
}

void ComponentModes_constructToolbar( QToolBar* toolbar ){
	toolbar_append_toggle_button( toolbar, "Select Primitives", "select.png", "SelectPrimitives" );
	toolbar_append_toggle_button( toolbar, "Select Vertices", "modify_vertices.png", "DragVertices" );
	toolbar_append_toggle_button( toolbar, "Select Edges", "modify_edges.png", "DragEdges" );
	toolbar_append_toggle_button( toolbar, "Select Faces", "modify_faces.png", "DragFaces" );
}

void XYWnd_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "Change views", "view_change.png", "NextView" );
}

void Manipulators_constructToolbar( QToolBar* toolbar ){
	toolbar_append_toggle_button( toolbar, "Resize (Q)", "select_mouseresize.png", "MouseDrag" ); // hardcoded shortcut tip of "MouseDragOrTransform"...
	toolbar_append_toggle_button( toolbar, "Clipper", "select_clipper.png", "ToggleClipper" );
	toolbar_append_toggle_button( toolbar, "Translate", "select_mousetranslate.png", "MouseTranslate" );
	toolbar_append_toggle_button( toolbar, "Rotate", "select_mouserotate.png", "MouseRotate" );
	toolbar_append_toggle_button( toolbar, "Scale", "select_mousescale.png", "MouseScale" );
	toolbar_append_toggle_button( toolbar, "Transform (Q)", "select_mousetransform.png", "MouseTransform" ); // hardcoded shortcut tip of "MouseDragOrTransform"...
//	toolbar_append_toggle_button( toolbar, "Build", "select_mouserotate.png", "MouseBuild" );
	toolbar_append_toggle_button( toolbar, "UV Tool", "select_mouseuv.png", "MouseUV" );
}

extern CopiedString g_toolbarHiddenButtons;

#include <QSvgGenerator>
void create_main_toolbar( QToolBar *toolbar,  MainFrame::EViewStyle style ){
	QSvgGenerator dummy; // reference symbol so the QtSvg DLL is explicit and install-dlls-msys2-mingw.sh can find it

 	File_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	UndoRedo_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	RotateFlip_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	Select_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	CSG_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	ComponentModes_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	if ( style != MainFrame::eSplit ) {
		XYWnd_constructToolbar( toolbar );
		toolbar_append_separator( toolbar );
	}

	CamWnd_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	Manipulators_constructToolbar( toolbar );
	toolbar_append_separator( toolbar );

	if ( game_supports_curve_tools() ) {
		Patch_constructToolbar( toolbar );
		toolbar_append_separator( toolbar );
	}

	toolbar_append_toggle_button( toolbar, "Texture Lock", "texture_lock.png", "TogTexLock" );
	toolbar_append_toggle_button( toolbar, "Texture Vertex Lock", "texture_vertexlock.png", "TogTexVertexLock" );
	toolbar_append_separator( toolbar );

	toolbar_append_button( toolbar, "Entities", "entities.png", "ToggleEntityInspector" );
	// disable the console and texture button in the regular layouts
	if ( style != MainFrame::eRegular && style != MainFrame::eRegularLeft ) {
		toolbar_append_button( toolbar, "Console", "console.png", "ToggleConsole" );
	}
	if ( AssetBrowser_isEnabled() && ( ( style != MainFrame::eRegular && style != MainFrame::eRegularLeft ) || g_Layout_builtInGroupDialog.m_value ) ) {
		toolbar_append_button( toolbar, "Asset Browser", "texture_browser.png", "ToggleTextures" );
	}

	// TODO: call light inspector
	//QAction* g_view_lightinspector_button = toolbar_append_button( toolbar, "Light Inspector", "lightinspector.png", "ToggleLightInspector" );

	toolbar_append_separator( toolbar );
	toolbar_append_button( toolbar, "Refresh Models", "refresh_models.png", "RefreshReferences" );
}

namespace
{
struct ConsoleSummaryStatusWidgets
{
	QPointer<QWidget> container;
	QPointer<QToolButton> build;
	QPointer<QToolButton> notifications;
	QPointer<QToolButton> warnings;
	QPointer<QToolButton> errors;
	QPointer<QTimer> refreshTimer;
} g_consoleSummaryStatusWidgets;

void ConsoleSummaryStatus_refresh();

void ConsoleSummaryStatus_showCategoryMessages( ConsoleSummaryCategory category, const QString& title ){
	const auto lines = Console_getCategoryMessages( category );
	Console_clearCategory( category );
	ConsoleSummaryStatus_refresh();

	if( lines.empty() ){
		return;
	}

	auto *dialog = new QDialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog->setAttribute( Qt::WidgetAttribute::WA_DeleteOnClose );
	dialog->setWindowTitle( title );
	dialog->resize( 760, 420 );

	auto *vbox = new QVBoxLayout( dialog );
	{
		auto *text = new QPlainTextEdit( dialog );
		text->setReadOnly( true );

		StringOutputStream content( 16384 );
		for( const auto& line : lines )
			content << line.c_str() << '\n';
		text->setPlainText( QString::fromUtf8( content.c_str() ) );
		vbox->addWidget( text );
	}
	{
		auto *buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Close, dialog );
		QObject::connect( buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close );
		QObject::connect( buttons, &QDialogButtonBox::accepted, dialog, &QDialog::close );
		vbox->addWidget( buttons );
	}

	dialog->show();
}

void ConsoleSummaryStatus_refresh(){
	if( g_consoleSummaryStatusWidgets.container.isNull() ){
		return;
	}

	const auto updateBuildBadge = [](){
		QToolButton* button = g_consoleSummaryStatusWidgets.build;
		if( button == nullptr ){
			return;
		}

		const BuildRuntimeState state = BuildMonitor_getRuntimeState();
		const QString text = QString::fromLatin1( BuildMonitor_getRuntimeText() );
		switch ( state )
		{
		case BuildRuntimeState::Building:
			button->setText( i18n::tr( "Build" ) );
			button->setStyleSheet( "QToolButton { border: 1px solid #3f6f8f; border-radius: 9px; padding: 1px 8px; background: #21465e; color: #d9f0ff; font-weight: 600; }" );
			button->setVisible( true );
			break;
		case BuildRuntimeState::Launching:
			button->setText( i18n::tr( "Launch" ) );
			button->setStyleSheet( "QToolButton { border: 1px solid #4d6f45; border-radius: 9px; padding: 1px 8px; background: #2c4a28; color: #d8f5cf; font-weight: 600; }" );
			button->setVisible( true );
			break;
		case BuildRuntimeState::Succeeded:
			button->setText( i18n::tr( "Done" ) );
			button->setStyleSheet( "QToolButton { border: 1px solid #4d6f45; border-radius: 9px; padding: 1px 8px; background: #2f4f2a; color: #d9f5cf; font-weight: 600; }" );
			button->setVisible( true );
			break;
		case BuildRuntimeState::Failed:
			button->setText( i18n::tr( "Failed" ) );
			button->setStyleSheet( "QToolButton { border: 1px solid #8d3942; border-radius: 9px; padding: 1px 8px; background: #4f2327; color: #f7c0c6; font-weight: 600; }" );
			button->setVisible( true );
			break;
		case BuildRuntimeState::Idle:
		default:
			button->setVisible( false );
			break;
		}
		button->setToolTip( text );
	};
	updateBuildBadge();

	const ConsoleSummary summary = Console_getSummary();
	const auto updateBadge = []( QToolButton* button, const char* prefix, int count, const std::string& tooltip ){
		if( button == nullptr ){
			return;
		}
		button->setText( QString( "%1 %2" ).arg( prefix ).arg( count ) );
		button->setToolTip( QString::fromUtf8( tooltip.c_str() ) );
		button->setVisible( count > 0 );
	};

	updateBadge( g_consoleSummaryStatusWidgets.notifications, "\xE2\x84\xB9", summary.notifications, summary.notificationsTooltip ); // ℹ
	updateBadge( g_consoleSummaryStatusWidgets.warnings, "\xE2\x9A\xA0", summary.warnings, summary.warningsTooltip ); // ⚠
	updateBadge( g_consoleSummaryStatusWidgets.errors, "\xE2\x9C\x96", summary.errors, summary.errorsTooltip ); // ✖

	const bool anyVisible = ( g_consoleSummaryStatusWidgets.build != nullptr && g_consoleSummaryStatusWidgets.build->isVisible() )
	                     || summary.notifications > 0 || summary.warnings > 0 || summary.errors > 0;
	g_consoleSummaryStatusWidgets.container->setVisible( anyVisible );
}

QToolButton* ConsoleSummaryStatus_createBadge( QWidget* parent, const QString& tooltip, const char* stylesheet ){
	auto *button = new QToolButton( parent );
	button->setAutoRaise( false );
	button->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
	button->setMinimumWidth( button->fontMetrics().horizontalAdvance( "⚠ 9999" ) );
	button->setToolTip( tooltip );
	button->setStyleSheet( stylesheet );
	return button;
}

void ConsoleSummaryStatus_connect( QObject* parent ){
	if( g_consoleSummaryStatusWidgets.refreshTimer.isNull() ){
		auto *timer = new QTimer( parent );
		timer->setInterval( 250 );
		QObject::connect( timer, &QTimer::timeout, [](){ ConsoleSummaryStatus_refresh(); } );
		timer->start();
		g_consoleSummaryStatusWidgets.refreshTimer = timer;
	}
	ConsoleSummaryStatus_refresh();
}

void ConsoleSummaryStatus_disconnect(){
	if( !g_consoleSummaryStatusWidgets.refreshTimer.isNull() ){
		g_consoleSummaryStatusWidgets.refreshTimer->stop();
		g_consoleSummaryStatusWidgets.refreshTimer = nullptr;
	}
}
}


void create_main_statusbar( QStatusBar *statusbar, QLabel *pStatusLabel[c_status__count] ){
	statusbar->setSizeGripEnabled( false );
	{
		auto *label = new QLabel;
		statusbar->addPermanentWidget( label, 1 );
		pStatusLabel[c_status_command] = label;
	}

	for ( int i = 1; i < c_status__count; ++i )
	{
		if( i == c_status_brushcount ){
			auto *widget = new QWidget;
			auto *hbox = new QHBoxLayout( widget );
			hbox->setContentsMargins( 0, 0, 0, 0 );
			statusbar->addPermanentWidget( widget, 0 );
			const char* imgs[3] = { "status_brush.png", "status_patch.png", "status_entity.png" };
			for( ; i < c_status_brushcount + 3; ++i ){
				auto *label = new QLabel();
				auto pixmap = new_local_image( imgs[i - c_status_brushcount] );
				pixmap.setDevicePixelRatio( label->devicePixelRatio() );
				label->setPixmap( pixmap.scaledToHeight( 16 * label->devicePixelRatio() * label->logicalDpiX() / 96, Qt::TransformationMode::SmoothTransformation ) );
				hbox->addWidget( label );

				label = new QLabel();
				label->setMinimumWidth( label->fontMetrics().horizontalAdvance( "99999" ) );
				hbox->addWidget( label );
				pStatusLabel[i] = label;
			}
			--i;
		}
		else{
			auto *label = new QLabel;
			if( i == c_status_brushsize ){
				statusbar->addPermanentWidget( label, 0 );
				label->setMinimumWidth( label->fontMetrics().horizontalAdvance( "9999x9999x9999" ) );
				label->setToolTip( i18n::tr( "Selection size (X x Y x Z)" ) );
			}
			else if( i == c_status_grid ){
				statusbar->addPermanentWidget( label, 0 );
				label->setToolTip( i18n::tr( " <b>G</b>: <u>G</u>rid size<br> <b>F</b>: map <u>F</u>ormat<br> <b>C</b>: camera <u>C</u>lip distance <br> <b>L</b>: texture <u>L</u>ock" ) );
			}
			else
				statusbar->addPermanentWidget( label, 1 );
			pStatusLabel[i] = label;
		}
	}

	{
		auto *widget = new QWidget;
		auto *hbox = new QHBoxLayout( widget );
		hbox->setContentsMargins( 2, 0, 2, 0 );
		hbox->setSpacing( 4 );

		g_consoleSummaryStatusWidgets.build = ConsoleSummaryStatus_createBadge(
			widget,
			i18n::tr( "Build status" ),
			"QToolButton { border: 1px solid #3f6f8f; border-radius: 9px; padding: 1px 8px; background: #21465e; color: #d9f0ff; font-weight: 600; }"
		);
		g_consoleSummaryStatusWidgets.notifications = ConsoleSummaryStatus_createBadge(
			widget,
			i18n::tr( "Notifications" ),
			"QToolButton { border: 1px solid #4b6678; border-radius: 9px; padding: 1px 8px; background: #243746; color: #dcecf8; font-weight: 600; }"
		);
		g_consoleSummaryStatusWidgets.warnings = ConsoleSummaryStatus_createBadge(
			widget,
			i18n::tr( "Warnings" ),
			"QToolButton { border: 1px solid #8a6d2f; border-radius: 9px; padding: 1px 8px; background: #4a3b1f; color: #f6df95; font-weight: 600; }"
		);
		g_consoleSummaryStatusWidgets.errors = ConsoleSummaryStatus_createBadge(
			widget,
			i18n::tr( "Errors" ),
			"QToolButton { border: 1px solid #8d3942; border-radius: 9px; padding: 1px 8px; background: #4f2327; color: #f7c0c6; font-weight: 600; }"
		);
		QObject::connect( g_consoleSummaryStatusWidgets.notifications, &QToolButton::clicked, [](){
			ConsoleSummaryStatus_showCategoryMessages( ConsoleSummaryCategory::Notifications, i18n::tr( "Notifications" ) );
		} );
		QObject::connect( g_consoleSummaryStatusWidgets.warnings, &QToolButton::clicked, [](){
			ConsoleSummaryStatus_showCategoryMessages( ConsoleSummaryCategory::Warnings, i18n::tr( "Warnings" ) );
		} );
		QObject::connect( g_consoleSummaryStatusWidgets.errors, &QToolButton::clicked, [](){
			ConsoleSummaryStatus_showCategoryMessages( ConsoleSummaryCategory::Errors, i18n::tr( "Errors" ) );
		} );

		hbox->addWidget( g_consoleSummaryStatusWidgets.build );
		hbox->addWidget( g_consoleSummaryStatusWidgets.notifications );
		hbox->addWidget( g_consoleSummaryStatusWidgets.warnings );
		hbox->addWidget( g_consoleSummaryStatusWidgets.errors );

		g_consoleSummaryStatusWidgets.container = widget;
		statusbar->addWidget( widget, 0 );
	}
	ConsoleSummaryStatus_connect( statusbar );
}

SignalHandlerId XYWindowDestroyed_connect( const SignalHandler& handler ){
	return g_pParentWnd->GetXYWnd()->onDestroyed.connectFirst( handler );
}

void XYWindowDestroyed_disconnect( SignalHandlerId id ){
	g_pParentWnd->GetXYWnd()->onDestroyed.disconnect( id );
}

MouseEventHandlerId XYWindowMouseDown_connect( const MouseEventHandler& handler ){
	return g_pParentWnd->GetXYWnd()->onMouseDown.connectFirst( handler );
}

void XYWindowMouseDown_disconnect( MouseEventHandlerId id ){
	g_pParentWnd->GetXYWnd()->onMouseDown.disconnect( id );
}

// =============================================================================
// MainFrame class

MainFrame* g_pParentWnd = 0;

QWidget* MainFrame_getWindow(){
	return g_pParentWnd == 0? 0 : g_pParentWnd->m_window;
}

MainFrame::MainFrame() : m_idleRedrawStatusText( RedrawStatusTextCaller( *this ) ){
	Create();
}

MainFrame::~MainFrame(){
	if ( m_window != nullptr ) {
		SaveGuiState();
		m_window->hide(); // hide to avoid resize events during content deletion
	}

	Shutdown();

	delete std::exchange( m_window, nullptr );
}

void MainFrame::SetActiveXY( XYWnd* p ){
	if ( m_pActiveXY ) {
		m_pActiveXY->SetActive( false );
	}

	m_pActiveXY = p;

	if ( m_pActiveXY ) {
		m_pActiveXY->SetActive( true );
	}
}

#ifdef WIN32
#include <QtPlatformHeaders/QWindowsWindowFunctions>
#endif
void MainFrame_toggleFullscreen(){
	QWidget *w = MainFrame_getWindow();
#ifdef WIN32 // https://doc.qt.io/qt-5.15/windows-issues.html#fullscreen-opengl-based-windows
	QWindowsWindowFunctions::setHasBorderInFullScreen( w->windowHandle(), true );
#endif
	w->setWindowState( w->windowState() ^ Qt::WindowState::WindowFullScreen );
}

class MaximizeView
{
	bool m_maximized{};
	QList<int> m_vSplitSizes;
	QList<int> m_vSplit2Sizes;
	QList<int> m_hSplitSizes;

	void maximize(){
		m_maximized = true;
		m_vSplitSizes = g_pParentWnd->m_vSplit->sizes();
		m_vSplit2Sizes = g_pParentWnd->m_vSplit2->sizes();
		m_hSplitSizes = g_pParentWnd->m_hSplit->sizes();

		const QPoint cursor = g_pParentWnd->m_hSplit->mapFromGlobal( QCursor::pos() );

		if( cursor.y() < m_vSplitSizes[0] )
			g_pParentWnd->m_vSplit->setSizes( { 9999, 0 } );
		else
			g_pParentWnd->m_vSplit->setSizes( { 0, 9999 } );

		if( cursor.y() < m_vSplit2Sizes[0] )
			g_pParentWnd->m_vSplit2->setSizes( { 9999, 0 } );
		else
			g_pParentWnd->m_vSplit2->setSizes( { 0, 9999 } );

		if( cursor.x() < m_hSplitSizes[0] )
			g_pParentWnd->m_hSplit->setSizes( { 9999, 0 } );
		else
			g_pParentWnd->m_hSplit->setSizes( { 0, 9999 } );
	}
public:
	void unmaximize(){
		if( m_maximized ){
			m_maximized = false;
			g_pParentWnd->m_vSplit->setSizes( m_vSplitSizes );
			g_pParentWnd->m_vSplit2->setSizes( m_vSplit2Sizes );
			g_pParentWnd->m_hSplit->setSizes( m_hSplitSizes );
		}
	}
	void toggle(){
		m_maximized ? unmaximize() : maximize();
	}
};

MaximizeView g_maximizeview;

void Maximize_View(){
	if( g_pParentWnd != 0 && g_pParentWnd->m_vSplit != 0 && g_pParentWnd->m_vSplit2 != 0 && g_pParentWnd->m_hSplit != 0 )
		g_maximizeview.toggle();
}



class RadiantQMainWindow : public QMainWindow
{
protected:
	void closeEvent( QCloseEvent *event ) override {
		event->ignore();
		Exit();
	}
	bool event( QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride && !QGuiApplication::mouseButtons().testFlag( Qt::MouseButton::NoButton ) ){
			event->accept(); // block shortcuts while mouse buttons are pressed
		}
		return QMainWindow::event( event );
	}
public:
	QMenu* createPopupMenu() override {
		auto *menu = QMainWindow::createPopupMenu();
		if( menu == nullptr )
			menu = new QMenu;
		else
			menu->addSeparator();
		toolbar_construct_control_menu( menu );
		return menu;
	}
};


QString splash_message_text( const QString& status ){
	const QString version = QString::fromLatin1( RADIANT_VERSION_NUMBER );
	const QString buildDate = QString::fromLatin1( __DATE__ );
	const QString state = status.trimmed().isEmpty() ? QStringLiteral( "Preparing startup..." ) : status.trimmed();

	return QStringLiteral(
		"VibeRadiant\n"
		"Version %1\n"
		"Build %2\n\n"
		"%3" )
		.arg( version, buildDate, state );
}

QSplashScreen *create_splash(){
	auto *splash = new QSplashScreen( new_local_image( "splash.png" ) );
	splash->showMessage(
		splash_message_text( QStringLiteral( "Starting up..." ) ),
		Qt::AlignLeft | Qt::AlignBottom,
		Qt::white );
	splash->show();
	return splash;
}

static QSplashScreen *splash_screen = 0;

void show_splash(){
	splash_screen = create_splash();

	process_gui();
}

void set_splash_status( const char* status ){
	if ( splash_screen == nullptr ) {
		return;
	}

	const QString text = QString::fromLatin1( status ? status : "" );
	splash_screen->showMessage( splash_message_text( text ), Qt::AlignLeft | Qt::AlignBottom, Qt::white );
	process_gui();
}

QWidget* splash_window(){
	return splash_screen;
}

void hide_splash(){
//.	splash_screen->finish();
	delete splash_screen;
	splash_screen = nullptr;
}


void user_shortcuts_init(){
	const auto path = StringStream( SettingsPath_get(), g_pGameDescription->mGameFile, '/' );
	LoadCommandMap( path );
	SaveCommandMap( path );
}

void user_shortcuts_save(){
	const auto path = StringStream( SettingsPath_get(), g_pGameDescription->mGameFile, '/' );
	SaveCommandMap( path );
}


void MainFrame::Create(){
	QMainWindow *window = m_window = new RadiantQMainWindow();

	GlobalWindowObservers_connectTopLevel( window );

	/* GlobalCommands_insert plugins commands */
	GetPlugInMgr().Init( window );
	/* then load shortcuts cfg */
	user_shortcuts_init();

	GlobalPressedKeys_connect( window );
	GlobalShortcuts_setWidget( window );
	register_shortcuts();

	const int configuredStyle = g_Layout_viewStyle.m_value;
	if ( configuredStyle < static_cast<int>( eRegular ) || configuredStyle > static_cast<int>( eRegularLeft ) ) {
		globalWarningStream() << "Invalid layout style in preferences (" << configuredStyle
		                      << "), falling back to regular layout\n";
		m_nCurrentStyle = eRegular;
	}
	else
	{
		m_nCurrentStyle = static_cast<EViewStyle>( configuredStyle );
	}

	create_main_menu( window->menuBar(), CurrentStyle() );

	{
		{
			auto *toolbar = new QToolBar( i18n::tr( "Main Toolbar" ) );
			toolbar->setObjectName( "Main_Toolbar" ); // required for proper state save/restore
			window->addToolBar( Qt::ToolBarArea::TopToolBarArea, toolbar );
			create_main_toolbar( toolbar, CurrentStyle() );
		}
		{
			auto *toolbar = new QToolBar( i18n::tr( "Filter Toolbar" ) );
			toolbar->setObjectName( "Filter_Toolbar" ); // required for proper state save/restore
			window->addToolBar( Qt::ToolBarArea::RightToolBarArea, toolbar );
			create_filter_toolbar( toolbar );
		}
		{
			auto *toolbar = new QToolBar( i18n::tr( "Plugin Toolbar" ) );
			toolbar->setObjectName( "Plugin_Toolbar" ); // required for proper state save/restore
			window->addToolBar( Qt::ToolBarArea::RightToolBarArea, toolbar );
			create_plugin_toolbar( toolbar );
		}
	}

	create_main_statusbar( window->statusBar(), m_statusLabel );

	GroupDialog_constructWindow( window );

	g_page_entity = GroupDialog_addPage( "Entities", EntityInspector_constructWindow( GroupDialog_getWindow() ), RawStringExportCaller( "Entities" ) );

	if ( FloatingGroupDialog() ) {
		g_page_console = GroupDialog_addPage( "Console", Console_constructWindow(), RawStringExportCaller( "Console" ) );
		if ( AssetBrowser_isEnabled() ) {
			g_page_textures = GroupDialog_addPage( "Asset Browser", AssetBrowser_constructWindow( GroupDialog_getWindow() ), TextureBrowserExportTitleCaller() );
		}
	}

	g_page_layers = GroupDialog_addPage( "Layers", LayersBrowser_constructWindow( GroupDialog_getWindow() ), RawStringExportCaller( "Layers" ) );
	g_page_issues = GroupDialog_addPage( "Issues", IssueBrowser_constructWindow( GroupDialog_getWindow() ), RawStringExportCaller( "Issues" ) );
	g_page_uvview = GroupDialog_addPage( "UV View", UVView_constructWindow( GroupDialog_getWindow() ), RawStringExportCaller( "UV View" ) );

	if ( CurrentStyle() == eRegular || CurrentStyle() == eRegularLeft ) {
		window->setCentralWidget( m_hSplit = new QSplitter() );
		{
			m_vSplit = new QSplitter( Qt::Vertical );
			m_vSplit2 = new QSplitter( Qt::Vertical );
			if ( CurrentStyle() == eRegular ){
				m_hSplit->addWidget( m_vSplit );
				m_hSplit->addWidget( m_vSplit2 );
			}
			else{
				m_hSplit->addWidget( m_vSplit2 );
				m_hSplit->addWidget( m_vSplit );
			}

			Console_constructWindow();

			// xy + z
			m_pXYWnd = new XYWnd();
			m_pXYWnd->SetViewType( XY );
			m_pZWnd = new ZWnd();
			m_xySplit = new QSplitter( Qt::Horizontal );
			m_xySplit->addWidget( m_pZWnd->GetWidget() );
			m_xySplit->addWidget( m_pXYWnd->GetWidget() );
			m_xySplit->setStretchFactor( 0, 0 );
			m_xySplit->setStretchFactor( 1, 1 );
			auto *xyOverlayHost = new QWidget();
			auto *xyOverlayLayout = new QVBoxLayout( xyOverlayHost );
			xyOverlayLayout->setContentsMargins( 0, 0, 0, 0 );
			xyOverlayLayout->setSpacing( 0 );
			xyOverlayLayout->addWidget( m_xySplit );
			m_vSplit->insertWidget( 0, xyOverlayHost );
			Console_setOverlayHost( xyOverlayHost );
			{
				// camera
				m_pCamWnd = NewCamWnd();
				GlobalCamera_setCamWnd( *m_pCamWnd );
				CamWnd_setParent( *m_pCamWnd, window );
				m_vSplit2->addWidget( CamWnd_getWidget( *m_pCamWnd ) );

				// textures
				if ( AssetBrowser_isEnabled() ) {
					if( g_Layout_builtInGroupDialog.m_value )
						g_page_textures = GroupDialog_addPage( "Asset Browser", AssetBrowser_constructWindow( GroupDialog_getWindow() ), TextureBrowserExportTitleCaller() );
					else
						m_vSplit2->addWidget( AssetBrowser_constructWindow( window ) );
				}
			}
		}
	}
	else if ( CurrentStyle() == eFloating ) {
		{
			auto *window = new QWidget( m_window, Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
			window->setWindowTitle( i18n::tr( "Camera" ) );
			g_guiSettings.addWindow( window, "floating/cam", 400, 300, 50, 100 );

			m_pCamWnd = NewCamWnd();
			GlobalCamera_setCamWnd( *m_pCamWnd );

			{
				auto *box = new QHBoxLayout( window );
				box->setContentsMargins( 1, 1, 1, 1 );
				box->addWidget( CamWnd_getWidget( *m_pCamWnd ) );
			}

			CamWnd_setParent( *m_pCamWnd, window );
			GlobalPressedKeys_connect( window );
			GlobalWindowObservers_connectTopLevel( window );
			CamWnd_Shown_Construct( window );
		}

		{
			auto *window = new QWidget( m_window, Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
			g_guiSettings.addWindow( window, "floating/xy", 400, 300, 500, 100 );

			m_pXYWnd = new XYWnd();
			m_pXYWnd->m_parent = window;
			m_pXYWnd->SetViewType( XY );
			m_pZWnd = new ZWnd();
			m_xySplit = new QSplitter( Qt::Horizontal );
			m_xySplit->addWidget( m_pZWnd->GetWidget() );
			m_xySplit->addWidget( m_pXYWnd->GetWidget() );
			m_xySplit->setStretchFactor( 0, 0 );
			m_xySplit->setStretchFactor( 1, 1 );

			{
				auto *box = new QHBoxLayout( window );
				box->setContentsMargins( 1, 1, 1, 1 );
				box->addWidget( m_xySplit );
			}

			GlobalWindowObservers_connectTopLevel( window );
			XY_Top_Shown_Construct( window );
		}

		{
			auto *window = new QWidget( m_window, Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
			g_guiSettings.addWindow( window, "floating/xz", 400, 300, 500, 450 );

			m_pXZWnd = new XYWnd();
			m_pXZWnd->m_parent = window;
			m_pXZWnd->SetViewType( XZ );

			{
				auto *box = new QHBoxLayout( window );
				box->setContentsMargins( 1, 1, 1, 1 );
				box->addWidget( m_pXZWnd->GetWidget() );
			}

			GlobalWindowObservers_connectTopLevel( window );
			XZ_Front_Shown_Construct( window );
		}

		{
			auto *window = new QWidget( m_window, Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
			g_guiSettings.addWindow( window, "floating/yz", 400, 300, 50, 450 );

			m_pYZWnd = new XYWnd();
			m_pYZWnd->m_parent = window;
			m_pYZWnd->SetViewType( YZ );

			{
				auto *box = new QHBoxLayout( window );
				box->setContentsMargins( 1, 1, 1, 1 );
				box->addWidget( m_pYZWnd->GetWidget() );
			}

			GlobalWindowObservers_connectTopLevel( window );
			YZ_Side_Shown_Construct( window );
		}

		GroupDialog_show();
	}
	else // 4 way
	{
		window->setCentralWidget( m_hSplit = new QSplitter() );
		m_hSplit->addWidget( m_vSplit = new QSplitter( Qt::Vertical ) );
		m_hSplit->addWidget( m_vSplit2 = new QSplitter( Qt::Vertical ) );

		m_pCamWnd = NewCamWnd();
		GlobalCamera_setCamWnd( *m_pCamWnd );
		CamWnd_setParent( *m_pCamWnd, window );

		m_vSplit->addWidget( CamWnd_getWidget( *m_pCamWnd ) );

		m_pYZWnd = new XYWnd();
		m_pYZWnd->SetViewType( YZ );

		m_vSplit->addWidget( m_pYZWnd->GetWidget() );

		m_pXYWnd = new XYWnd();
		m_pXYWnd->SetViewType( XY );
		m_pZWnd = new ZWnd();
		m_xySplit = new QSplitter( Qt::Horizontal );
		m_xySplit->addWidget( m_pZWnd->GetWidget() );
		m_xySplit->addWidget( m_pXYWnd->GetWidget() );
		m_xySplit->setStretchFactor( 0, 0 );
		m_xySplit->setStretchFactor( 1, 1 );
		m_vSplit2->addWidget( m_xySplit );

		m_pXZWnd = new XYWnd();
		m_pXZWnd->SetViewType( XZ );

		m_vSplit2->addWidget( m_pXZWnd->GetWidget() );
	}

	if( g_Layout_builtInGroupDialog.m_value && CurrentStyle() != eFloating ){
		m_hSplit->addWidget( GroupDialog_getWindow() );
		m_hSplit->setStretchFactor( 0, 2222 ); // set relative splitter sizes for eSplit (no sizes are restored)
		m_hSplit->setStretchFactor( 1, 2222 );
		m_hSplit->setStretchFactor( 2, 0 );
	}
	else{ // floating group dialog
		GlobalWindowObservers_connectTopLevel( GroupDialog_getWindow() ); // for layers browser icons toggle
	}

	EntityList_constructWindow( window );
	FindTextureDialog_constructWindow( window );
	SurfaceInspector_constructWindow( window );

	SetActiveXY( m_pXYWnd );

	AddGridChangeCallback( SetGridStatusCaller( *this ) );
	AddGridChangeCallback( FreeCaller<void(), XY_UpdateAllWindows>() );

	s_qe_every_second_timer.enable();

	toolbar_importState( g_toolbarHiddenButtons.c_str() );
	RestoreGuiState();

	GlobalShortcuts_reportDuplicates();
	//GlobalShortcuts_reportUnregistered();
}

void MainFrame::SaveGuiState(){
	//restore good state first
	g_maximizeview.unmaximize();

	g_guiSettings.save();
}

void MainFrame::RestoreGuiState(){
	g_guiSettings.addWindow( m_window, "MainFrame/geometry", 962, 480 );
	g_guiSettings.addMainWindow( m_window, "MainFrame/state" );

	if( !FloatingGroupDialog() && m_hSplit != nullptr && m_vSplit != nullptr && m_vSplit2 != nullptr ){
		g_guiSettings.addSplitter( m_hSplit, "MainFrame/m_hSplit", { 384, 576 } );
		if( m_vSplit->count() > 1 ){
			g_guiSettings.addSplitter( m_vSplit, "MainFrame/m_vSplit", { 377, 20 } );
		}
		else{
			g_guiSettings.addSplitter( m_vSplit, "MainFrame/m_vSplit", { 377 } );
		}
		if ( m_xySplit != nullptr ) {
			g_guiSettings.addSplitter( m_xySplit, "MainFrame/m_xySplit", { 64, 500 } );
		}
		g_guiSettings.addSplitter( m_vSplit2, "MainFrame/m_vSplit2", { 250, 150 } );
	}
	if ( CurrentStyle() == eFloating && m_xySplit != nullptr ) {
		g_guiSettings.addSplitter( m_xySplit, "floating/xySplit", { 64, 500 } );
	}
}

void MainFrame::Shutdown(){
	s_qe_every_second_timer.disable();
	ConsoleSummaryStatus_disconnect();
	g_consoleSummaryStatusWidgets.container = nullptr;
	g_consoleSummaryStatusWidgets.build = nullptr;
	g_consoleSummaryStatusWidgets.notifications = nullptr;
	g_consoleSummaryStatusWidgets.warnings = nullptr;
	g_consoleSummaryStatusWidgets.errors = nullptr;

	EntityList_destroyWindow();

	delete std::exchange( m_pXYWnd, nullptr );
	delete std::exchange( m_pZWnd, nullptr );
	delete std::exchange( m_pYZWnd, nullptr );
	delete std::exchange( m_pXZWnd, nullptr );

	ModelBrowser_destroyWindow();
	LayersBrowser_destroyWindow();
	IssueBrowser_destroyWindow();
	UVView_destroyWindow();
	if ( AssetBrowser_isEnabled() ) {
		SoundBrowser_destroyWindow();
		EntityBrowser_destroyWindow();
		TextureBrowser_destroyWindow();
		AssetBrowser_destroyWindow();
	}

	DeleteCamWnd( m_pCamWnd );
	m_pCamWnd = 0;

	PreferencesDialog_destroyWindow();
	SurfaceInspector_destroyWindow();
	FindTextureDialog_destroyWindow();

	g_DbgDlg.destroyWindow();

	// destroying group-dialog last because it may contain texture-browser
	GroupDialog_destroyWindow();

	user_shortcuts_save();
}

void MainFrame::RedrawStatusText(){
	for( int i = 0; i < c_status__count; ++i ) {
		if ( m_statusLabel[i] != nullptr ) {
			m_statusLabel[i]->setText( m_status[i].c_str() );
		}
	}
}

void MainFrame::UpdateStatusText(){
	m_idleRedrawStatusText.queueDraw();
}

void MainFrame::SetStatusText( int status_n, const char* status ){
	m_status[status_n] = status;
	UpdateStatusText();
}

void Sys_Status( const char* status ){
	if ( g_pParentWnd )
		g_pParentWnd->SetStatusText( c_status_command, status );
}

void brushCountChanged( const Selectable& selectable ){
	QE_brushCountChanged();
}

//int getRotateIncrement(){
//	return static_cast<int>( g_si_globals.rotate );
//}

int getFarClipDistance(){
	return g_camwindow_globals.m_nCubicScale;
}

float ( *GridStatus_getGridSize )() = GetGridSize;
//int ( *GridStatus_getRotateIncrement )() = getRotateIncrement;
int ( *GridStatus_getFarClipDistance )() = getFarClipDistance;
bool ( *GridStatus_getTextureLockEnabled )();
const char* ( *GridStatus_getTexdefTypeIdLabel )();

void MainFrame::SetGridStatus(){
	StringOutputStream status( 64 );
	const char* lock = ( GridStatus_getTextureLockEnabled() ) ? "ON   " : "OFF  ";
	status << ( GetSnapGridSize() > 0 ? "G:" : "g:" ) << GridStatus_getGridSize()
	       << "  F:" << GridStatus_getTexdefTypeIdLabel()
	       << "  C:" << GridStatus_getFarClipDistance()
	       << "  L:" << lock;
	SetStatusText( c_status_grid, status );
}

void GridStatus_changed(){
	if ( g_pParentWnd != 0 ) {
		g_pParentWnd->SetGridStatus();
	}
}

CopiedString g_OpenGLFont( "Myriad Pro" );
int g_OpenGLFontSize = 8;
namespace
{
constexpr int kOpenGLFontFallbackPixelHeight = 12;
constexpr int kOpenGLFontFallbackPixelDescent = 2;
GLFont* g_openGLFont = nullptr;
int g_openGLFontPixelHeight = kOpenGLFontFallbackPixelHeight;
int g_openGLFontPixelDescent = kOpenGLFontFallbackPixelDescent;

void OpenGLFont_syncGlobalState(){
	GlobalOpenGL().m_font = g_openGLFont;
	if ( g_openGLFont != nullptr ) {
		g_openGLFontPixelHeight = g_openGLFont->getPixelHeight();
		g_openGLFontPixelDescent = g_openGLFont->getPixelDescent();
	}
	else
	{
		g_openGLFontPixelHeight = kOpenGLFontFallbackPixelHeight;
		g_openGLFontPixelDescent = kOpenGLFontFallbackPixelDescent;
	}
}
}

int OpenGLFont_getPixelHeightSafe(){
	return g_openGLFontPixelHeight;
}

int OpenGLFont_getPixelDescentSafe(){
	return g_openGLFontPixelDescent;
}

bool OpenGLFont_canDrawSafe(){
	return g_openGLFont != nullptr;
}

void OpenGLFont_drawStringSafe( const char* string ){
	if ( g_openGLFont != nullptr ) {
		g_openGLFont->printString( string );
	}
}

void OpenGLFont_drawCharSafe( char character ){
	if ( g_openGLFont != nullptr ) {
		char s[2] = { character, 0 };
		g_openGLFont->printString( s );
	}
}

void OpenGLFont_select(){
	CopiedString newfont;
	int newsize;
	if( OpenGLFont_dialog( MainFrame_getWindow(), g_OpenGLFont.c_str(), g_OpenGLFontSize, newfont, newsize ) ){
		{
			ScopeDisableScreenUpdates disableScreenUpdates( "Processing...", "Changing OpenGL Font" );
			delete g_openGLFont;
			g_openGLFont = nullptr;
			g_OpenGLFont = newfont;
			g_OpenGLFontSize = newsize;
			g_openGLFont = glfont_create( g_OpenGLFont.c_str(), g_OpenGLFontSize, g_strAppPath.c_str() );
			OpenGLFont_syncGlobalState();
		}
		UpdateAllWindows();
	}
}


void GlobalGL_sharedContextCreated(){
	// report OpenGL information
	globalOutputStream() << "GL_VENDOR: " << reinterpret_cast<const char*>( gl().glGetString( GL_VENDOR ) ) << '\n';
	globalOutputStream() << "GL_RENDERER: " << reinterpret_cast<const char*>( gl().glGetString( GL_RENDERER ) ) << '\n';
	globalOutputStream() << "GL_VERSION: " << reinterpret_cast<const char*>( gl().glGetString( GL_VERSION ) ) << '\n';
	globalOutputStream() << "GL_EXTENSIONS: " << reinterpret_cast<const char*>( gl().glGetString( GL_EXTENSIONS ) ) << '\n';

	QGL_sharedContextCreated( GlobalOpenGL() );

	ShaderCache_extensionsInitialised();

	GlobalShaderCache().realise();
	Textures_Realise();

	g_openGLFont = glfont_create( g_OpenGLFont.c_str(), g_OpenGLFontSize, g_strAppPath.c_str() );
	OpenGLFont_syncGlobalState();
}

void GlobalGL_sharedContextDestroyed(){
	g_openGLFont = nullptr;
	OpenGLFont_syncGlobalState();
	Textures_Unrealise();
	GlobalShaderCache().unrealise();

	QGL_sharedContextDestroyed( GlobalOpenGL() );
}


void Layout_constructPreferences( PreferencesPage& page ){
	{
		const char* layouts[] = { "window1.png", "window2.png", "window3.png", "window4.png" };
		page.appendRadioIcons(
		    "Window Layout",
		    StringArrayRange( layouts ),
		    LatchedImportCaller( g_Layout_viewStyle ),
		    IntExportCaller( g_Layout_viewStyle.m_latched )
		);
	}
	page.appendCheckBox(
	    "", "Detachable Menus",
	    LatchedImportCaller( g_Layout_enableDetachableMenus ),
	    BoolExportCaller( g_Layout_enableDetachableMenus.m_latched )
	);
	page.appendCheckBox(
	    "", "Built-In Group Dialog",
	    LatchedImportCaller( g_Layout_builtInGroupDialog ),
	    BoolExportCaller( g_Layout_builtInGroupDialog.m_latched )
	);
}

void Layout_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Layout", "Layout Preferences" ) );
	Layout_constructPreferences( page );
}

void Layout_registerPreferencesPage(){
	PreferencesDialog_addInterfacePage( makeCallbackF( Layout_constructPage ) );
}


void FocusAllViews(){
	XY_Centralize(); //using centralizing here, not focusing function
	GlobalCamera_FocusOnSelected();
}

#include "preferencesystem.h"
#include "stringio.h"

void ConsoleCollapsedImport( bool value ){
	Console_setCollapsed( value );
}
typedef FreeCaller<void(bool), ConsoleCollapsedImport> ConsoleCollapsedImportCaller;

void ConsoleCollapsedExport( const BoolImportCallback& importer ){
	importer( Console_isCollapsed() );
}
typedef FreeCaller<void(const BoolImportCallback&), ConsoleCollapsedExport> ConsoleCollapsedExportCaller;

void MainFrame_Construct(){
	GlobalCommands_insert( "OpenManual", makeCallbackF( OpenHelpURL ), QKeySequence( "F1" ) );

	GlobalCommands_insert( "RefreshReferences", makeCallbackF( RefreshReferences ) );
	GlobalCommands_insert( "CheckForUpdate", makeCallbackF( OpenUpdateURL ) );
	GlobalCommands_insert( "Exit", makeCallbackF( Exit ) );

	GlobalCommands_insert( "CommandPalette", makeCallbackF( DoCommandListDlg ), QKeySequence( "Ctrl+Shift+P" ) );
	GlobalCommands_insert( "Shortcuts", makeCallbackF( DoCommandListDlg ) ); // legacy alias
	GlobalCommands_insert( "Preferences", makeCallbackF( PreferencesDialog_showDialog ), QKeySequence( "P" ) );

	GlobalCommands_insert( "MacroRecordStart", makeCallbackF( GlobalCommandMacro_startRecording ), QKeySequence( "Ctrl+Shift+F9" ) );
	GlobalCommands_insert( "MacroRecordStop", makeCallbackF( GlobalCommandMacro_stopRecording ), QKeySequence( "Ctrl+Shift+F10" ) );
	GlobalCommands_insert( "MacroPlay", makeCallbackF( GlobalCommandMacro_playback ), QKeySequence( "Ctrl+Shift+F11" ) );
	GlobalCommands_insert( "MacroClear", makeCallbackF( GlobalCommandMacro_clear ) );

	GlobalCommands_insert( "ToggleConsole", makeCallbackF( Console_ToggleShow ), QKeySequence( "O" ) );
	GlobalCommands_insert( "ToggleEntityInspector", makeCallbackF( EntityInspector_ToggleShow ), QKeySequence( "N" ) );
	GlobalCommands_insert( "ToggleLayersBrowser", makeCallbackF( LayersBrowser_ToggleShow ), QKeySequence( "L" ) );
	GlobalCommands_insert( "ToggleIssueBrowser", makeCallbackF( IssueBrowser_ToggleShow ) );
	GlobalCommands_insert( "ToggleUVView", makeCallbackF( UVView_ToggleShow ) );
	GlobalCommands_insert( "ToggleEntityList", makeCallbackF( EntityList_toggleShown ), QKeySequence( "Shift+L" ) );
	GlobalCommands_insert( "WorkspacePresetModeling", makeCallbackF( WorkspacePreset_Modeling ) );
	GlobalCommands_insert( "WorkspacePresetTexturing", makeCallbackF( WorkspacePreset_Texturing ) );
	GlobalCommands_insert( "WorkspacePresetEntity", makeCallbackF( WorkspacePreset_Entity ) );
	GlobalCommands_insert( "WorkspacePresetLighting", makeCallbackF( WorkspacePreset_Lighting ) );
	GlobalToggles_insert( "FocusMode", makeCallbackF( FocusMode_toggle ), ToggleItem::AddCallbackCaller( g_focusModeItem ), QKeySequence( "Ctrl+Shift+`" ) );

	Select_registerCommands();
	Layers_registerCommands();

	Tools_registerCommands();

	GlobalCommands_insert( "BuildMenuCustomize", makeCallbackF( DoBuildMenu ) );
	GlobalCommands_insert( "BuildTaskBuild", makeCallbackF( Build_runTaskBuild ), QKeySequence( "Ctrl+Shift+B" ) );
	GlobalCommands_insert( "BuildTaskLaunch", makeCallbackF( Build_runTaskLaunch ), QKeySequence( "F5" ) );
	GlobalCommands_insert( "BuildTaskBuildAndLaunch", makeCallbackF( Build_runTaskBuildAndLaunch ), QKeySequence( "Ctrl+F5" ) );
	GlobalCommands_insert( "Build_runRecentExecutedBuild", makeCallbackF( Build_runRecentExecutedBuild ) ); // legacy alias
	GlobalCommands_insert( "Set2DBackgroundImage", makeCallbackF( WXY_SetBackgroundImage ) );

	GlobalCommands_insert( "OpenGLFont", makeCallbackF( OpenGLFont_select ) );

	Colors_registerCommands();
	GenAI_Construct();

	GlobalCommands_insert( "Fullscreen", makeCallbackF( MainFrame_toggleFullscreen ), QKeySequence( "F11" ) );
	GlobalCommands_insert( "MaximizeView", makeCallbackF( Maximize_View ), QKeySequence( "F12" ) );

	CSG_registerCommands();

	Grid_registerCommands();

	Patch_registerCommands();
	XYShow_registerCommands();

	GlobalPreferenceSystem().registerPreference( "DetachableMenus", makeBoolStringImportCallback( LatchedAssignCaller( g_Layout_enableDetachableMenus ) ), BoolExportStringCaller( g_Layout_enableDetachableMenus.m_latched ) );
	GlobalPreferenceSystem().registerPreference( "QE4StyleWindows", makeIntStringImportCallback( LatchedAssignCaller( g_Layout_viewStyle ) ), IntExportStringCaller( g_Layout_viewStyle.m_latched ) );
	GlobalPreferenceSystem().registerPreference( "BuiltInGroupDialog", makeBoolStringImportCallback( LatchedAssignCaller( g_Layout_builtInGroupDialog ) ), BoolExportStringCaller( g_Layout_builtInGroupDialog.m_latched ) );
	GlobalPreferenceSystem().registerPreference( "ToolbarHiddenButtons", CopiedStringImportStringCaller( g_toolbarHiddenButtons ), CopiedStringExportStringCaller( g_toolbarHiddenButtons ) );
	GlobalPreferenceSystem().registerPreference( "OpenGLFont", CopiedStringImportStringCaller( g_OpenGLFont ), CopiedStringExportStringCaller( g_OpenGLFont ) );
	GlobalPreferenceSystem().registerPreference( "OpenGLFontSize", IntImportStringCaller( g_OpenGLFontSize ), IntExportStringCaller( g_OpenGLFontSize ) );
	GlobalPreferenceSystem().registerPreference( "ConsoleCollapsed", makeBoolStringImportCallback( ConsoleCollapsedImportCaller() ), makeBoolStringExportCallback( ConsoleCollapsedExportCaller() ) );
	GlobalPreferenceSystem().registerPreference( "StartupOnboardingCompleted", BoolImportStringCaller( g_startup_onboarding_completed ), BoolExportStringCaller( g_startup_onboarding_completed ) );
	GlobalPreferenceSystem().registerPreference( "StartupOnboardingDone", BoolImportStringCaller( g_startup_onboarding_completed ), BoolExportStringCaller( g_startup_onboarding_completed ) ); // legacy compatibility
	GlobalPreferenceSystem().registerPreference( "EditorStyle", IntImportStringCaller( g_editor_style_preference ), IntExportStringCaller( g_editor_style_preference ) );
	GlobalPreferenceSystem().registerPreference( "EditorStylePreset", IntImportStringCaller( g_editor_style_preference ), IntExportStringCaller( g_editor_style_preference ) ); // legacy compatibility

	for( size_t i = 0; i < g_strExtraResourcePaths.size(); ++i )
		GlobalPreferenceSystem().registerPreference( StringStream<32>( "ExtraResourcePath", i ),
			CopiedStringImportStringCaller( g_strExtraResourcePaths[i] ), CopiedStringExportStringCaller( g_strExtraResourcePaths[i] ) );

	GlobalPreferenceSystem().registerPreference( "EnginePath", CopiedStringImportStringCaller( g_strEnginePath ), CopiedStringExportStringCaller( g_strEnginePath ) );
	GlobalPreferenceSystem().registerPreference( "InstalledDevFilesPath", CopiedStringImportStringCaller( g_installedDevFilesPath ), CopiedStringExportStringCaller( g_installedDevFilesPath ) );
	if ( const char* startupInstallationPath = StartupGameInstallationPath_get(); !string_empty( startupInstallationPath ) )
	{
		g_strEnginePath = StringStream( DirectoryCleaned( startupInstallationPath ) );
		g_strEnginePath_was_empty_1st_start = false;
	}
	else if ( g_strEnginePath.empty() )
	{
		g_strEnginePath_was_empty_1st_start = true;
		const char* ENGINEPATH_ATTRIBUTE =
#if defined( WIN32 )
		    "enginepath_win32"
#elif defined( __APPLE__ )
		    "enginepath_macos"
#elif defined( __linux__ ) || defined ( __FreeBSD__ )
		    "enginepath_linux"
#else
#error "unknown platform"
#endif
		    ;
		g_strEnginePath = StringStream( DirectoryCleaned( g_pGameDescription->getRequiredKeyValue( ENGINEPATH_ATTRIBUTE ) ) );
	}


	Layout_registerPreferencesPage();
	Paths_registerPreferencesPage();
	Colors_registerPreferencesPage();

	g_brushCount.setCountChangedCallback( makeCallbackF( QE_brushCountChanged ) );
	g_patchCount.setCountChangedCallback( makeCallbackF( QE_brushCountChanged ) );
	g_entityCount.setCountChangedCallback( makeCallbackF( QE_brushCountChanged ) );
	GlobalEntityCreator().setCounter( &g_entityCount );
	GlobalSelectionSystem().addSelectionChangeCallback( FreeCaller<void(const Selectable&), brushCountChanged>() );

	GLWidget_sharedContextCreated = GlobalGL_sharedContextCreated;
	GLWidget_sharedContextDestroyed = GlobalGL_sharedContextDestroyed;

	GlobalEntityClassManager().attach( g_WorldspawnColourEntityClassObserver );
}

void MainFrame_Destroy(){
	GenAI_Destroy();
	GlobalEntityClassManager().detach( g_WorldspawnColourEntityClassObserver );

	GlobalEntityCreator().setCounter( 0 );
	g_entityCount.setCountChangedCallback( Callback<void()>() );
	g_patchCount.setCountChangedCallback( Callback<void()>() );
	g_brushCount.setCountChangedCallback( Callback<void()>() );
}
