/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

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

#include "environment.h"

#include "stream/textstream.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "debugging/debugging.h"
#include "os/path.h"
#include "os/file.h"
#include "commandlib.h"

#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <set>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <system_error>
#include <limits>

#include <libxml/parser.h>

int g_argc;
const char** g_argv;

void args_init( int argc, char* argv[] ){
	int i, j, k;

	for ( i = 1; i < argc; ++i )
	{
		for ( k = i; k < argc; ++k )
			if ( argv[k] != 0 ) {
				break;
			}

		if ( k > i ) {
			k -= i;
			for ( j = i + k; j < argc; ++j )
				argv[j - k] = argv[j];
			argc -= k;
		}
	}

	g_argc = argc;
	g_argv = const_cast<const char **>( argv );
}

const char *gamedetect_argv_buffer[1024];
void gamedetect_found_game( const char *game, const char *path ){
	int argc;
	static char buf[128];

	if ( g_argv == gamedetect_argv_buffer ) {
		return;
	}

	globalOutputStream() << "Detected game " << game << " in " << path << '\n';

	sprintf( buf, "-%s-EnginePath", game );
	argc = 0;
	gamedetect_argv_buffer[argc++] = "-global-gamefile";
	gamedetect_argv_buffer[argc++] = game;
	gamedetect_argv_buffer[argc++] = buf;
	gamedetect_argv_buffer[argc++] = path;
	if ( (size_t) ( argc + g_argc ) >= std::size( gamedetect_argv_buffer ) - 1 ) {
		g_argc = std::size( gamedetect_argv_buffer ) - g_argc - 1;
	}
	memcpy( gamedetect_argv_buffer + 4, g_argv, sizeof( *gamedetect_argv_buffer ) * g_argc );
	g_argc += argc;
	g_argv = gamedetect_argv_buffer;
}

namespace
{
struct GameDetectRule
{
	std::string gameFile;
	std::string gameName;
	std::string basegame;
	std::string engineExecutable;
	std::vector<std::string> archiveTypes;
	std::vector<std::string> requiredFiles;
	std::vector<std::string> aliases;
};

std::string gamedetect_toLower( const std::string& value ){
	std::string lower;
	lower.reserve( value.size() );
	for ( const unsigned char c : value )
	{
		lower.push_back( static_cast<char>( std::tolower( c ) ) );
	}
	return lower;
}

std::string gamedetect_trim( std::string value ){
	while ( !value.empty() && std::isspace( static_cast<unsigned char>( value.front() ) ) )
		value.erase( value.begin() );
	while ( !value.empty() && std::isspace( static_cast<unsigned char>( value.back() ) ) )
		value.pop_back();
	return value;
}

std::string gamedetect_normaliseToken( const char* value ){
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

std::vector<std::string> gamedetect_splitList( const char* value, const char* delimiters ){
	std::vector<std::string> tokens;
	if ( value == nullptr || string_empty( value ) ) {
		return tokens;
	}

	std::string token;
	for ( const char c : std::string_view( value ) )
	{
		if ( strchr( delimiters, c ) != nullptr ) {
			token = gamedetect_trim( token );
			if ( !token.empty() ) {
				tokens.push_back( token );
			}
			token.clear();
			continue;
		}
		token.push_back( c );
	}

	token = gamedetect_trim( token );
	if ( !token.empty() ) {
		tokens.push_back( token );
	}
	return tokens;
}

void gamedetect_appendUniqueValue( std::vector<std::string>& values, const std::string& value, bool normalisePathSeparators = false ){
	if ( value.empty() ) {
		return;
	}

	std::string canonical = value;
	if ( normalisePathSeparators ) {
		std::replace( canonical.begin(), canonical.end(), '\\', '/' );
	}
	const auto canonicalLower = gamedetect_toLower( canonical );
	for ( const auto& existing : values )
	{
		std::string existingCanonical = existing;
		if ( normalisePathSeparators ) {
			std::replace( existingCanonical.begin(), existingCanonical.end(), '\\', '/' );
		}
		if ( gamedetect_toLower( existingCanonical ) == canonicalLower ) {
			return;
		}
	}
	values.push_back( value );
}

void gamedetect_appendUniqueAlias( std::vector<std::string>& aliases, const char* alias ){
	if ( alias == nullptr || string_empty( alias ) ) {
		return;
	}

	const auto trimmed = gamedetect_trim( alias );
	if ( trimmed.empty() ) {
		return;
	}

	const auto token = gamedetect_normaliseToken( trimmed.c_str() );
	if ( token.empty() ) {
		return;
	}
	for ( const auto& existing : aliases )
	{
		if ( gamedetect_normaliseToken( existing.c_str() ) == token ) {
			return;
		}
	}
	aliases.push_back( trimmed );
}

bool gamedetect_isWeakAlias( const std::string& token ){
	static const std::set<std::string> weak = {
		"base", "data", "main", "default", "pkg", "id1", "mod", "game"
	};
	return token.size() < 4 || weak.find( token ) != weak.end();
}

bool gamedetect_textMatchesAliases( const std::string& textNormalised, const GameDetectRule& rule ){
	for ( const auto& alias : rule.aliases )
	{
		const auto token = gamedetect_normaliseToken( alias.c_str() );
		if ( gamedetect_isWeakAlias( token ) ) {
			continue;
		}
		if ( textNormalised.find( token ) != std::string::npos ) {
			return true;
		}
	}
	return false;
}

const char* gamedetect_platformEngineAttribute(){
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

const char* gamedetect_xmlAttrName( xmlAttrPtr attr ){
	return reinterpret_cast<const char*>( attr->name );
}

const char* gamedetect_xmlAttrValue( xmlAttrPtr attr ){
	if ( attr->children == nullptr || attr->children->content == nullptr ) {
		return "";
	}
	return reinterpret_cast<const char*>( attr->children->content );
}

bool gamedetect_loadRule( const std::filesystem::path& gamePath, GameDetectRule& outRule ){
	xmlDocPtr pDoc = xmlParseFile( gamePath.string().c_str() );
	if ( pDoc == nullptr ) {
		return false;
	}

	xmlNodePtr pNode = pDoc->children;
	while ( pNode != nullptr && strcmp( reinterpret_cast<const char*>( pNode->name ), "game" ) )
	{
		pNode = pNode->next;
	}
	if ( pNode == nullptr ) {
		xmlFreeDoc( pDoc );
		return false;
	}

	std::map<std::string, std::string> attrs;
	for ( xmlAttrPtr attr = pNode->properties; attr != nullptr; attr = attr->next )
	{
		attrs[gamedetect_xmlAttrName( attr )] = gamedetect_xmlAttrValue( attr );
	}
	xmlFreeDoc( pDoc );

	const auto attr = [&attrs]( const char* key )->const char* {
		if ( const auto found = attrs.find( key ); found != attrs.end() ) {
			return found->second.c_str();
		}
		return "";
	};

	outRule.gameFile = gamePath.filename().string();
	outRule.gameName = attr( "name" );
	outRule.basegame = attr( "basegame" );
	if ( outRule.basegame.empty() ) {
		return false;
	}
	outRule.engineExecutable = attr( gamedetect_platformEngineAttribute() );

	for ( const auto& token : gamedetect_splitList( attr( "archivetypes" ), " ,;|\t\r\n" ) )
	{
		gamedetect_appendUniqueValue( outRule.archiveTypes, gamedetect_toLower( token ) );
	}
	for ( const char* key : { "detect_file1", "detect_file2" } )
	{
		const std::string value = attr( key );
		if ( !value.empty() ) {
			gamedetect_appendUniqueValue( outRule.requiredFiles, value, true );
		}
	}
	for ( const auto& token : gamedetect_splitList( attr( "detect_files" ), ",;|" ) )
	{
		gamedetect_appendUniqueValue( outRule.requiredFiles, token, true );
	}

	gamedetect_appendUniqueAlias( outRule.aliases, outRule.basegame.c_str() );
	gamedetect_appendUniqueAlias( outRule.aliases, attr( "basegamename" ) );
	gamedetect_appendUniqueAlias( outRule.aliases, attr( "type" ) );
	gamedetect_appendUniqueAlias( outRule.aliases, outRule.gameName.c_str() );
	gamedetect_appendUniqueAlias( outRule.aliases, attr( "knowngame" ) );
	gamedetect_appendUniqueAlias( outRule.aliases, attr( "knowngamename" ) );
	gamedetect_appendUniqueAlias( outRule.aliases, attr( "unknowngamename" ) );
	for ( const auto& token : gamedetect_splitList( attr( "knownmods" ), ",;|" ) )
	{
		gamedetect_appendUniqueAlias( outRule.aliases, token.c_str() );
	}
	for ( const auto& token : gamedetect_splitList( attr( "knownmodnames" ), ",;|" ) )
	{
		gamedetect_appendUniqueAlias( outRule.aliases, token.c_str() );
	}
	for ( const auto& token : gamedetect_splitList( attr( "install_aliases" ), ",;|" ) )
	{
		gamedetect_appendUniqueAlias( outRule.aliases, token.c_str() );
	}

	const auto stem = std::filesystem::path( outRule.gameFile ).stem().string();
	gamedetect_appendUniqueAlias( outRule.aliases, stem.c_str() );
	if ( !outRule.engineExecutable.empty() ) {
		const auto engineStem = std::filesystem::path( outRule.engineExecutable ).stem().string();
		gamedetect_appendUniqueAlias( outRule.aliases, engineStem.c_str() );
	}

	return true;
}

void gamedetect_applyLegacyHints( GameDetectRule& rule ){
	const auto gameFileLower = gamedetect_toLower( rule.gameFile );
	const auto addRequired = [&rule]( const char* value ){
		gamedetect_appendUniqueValue( rule.requiredFiles, value, true );
	};
	const auto addAlias = [&rule]( const char* value ){
		gamedetect_appendUniqueAlias( rule.aliases, value );
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
		addAlias( "Quake 3" );
		addAlias( "Quake III Arena" );
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
	else if ( gameFileLower == "warsow.game" ) {
		addRequired( "basewsw/dedicated_autoexec.cfg" );
		addAlias( "Warsow" );
	}
}

void gamedetect_collectRules( std::vector<GameDetectRule>& rules ){
	const std::filesystem::path gamesPath( StringStream( environment_get_app_path(), "gamepacks/games/" ).c_str() );
	std::error_code err;
	if ( !std::filesystem::is_directory( gamesPath, err ) ) {
		return;
	}

	for ( const auto& entry : std::filesystem::directory_iterator( gamesPath, std::filesystem::directory_options::skip_permission_denied, err ) )
	{
		if ( !entry.is_regular_file( err ) ) {
			continue;
		}
		auto ext = gamedetect_toLower( entry.path().extension().string() );
		if ( ext != ".game" ) {
			continue;
		}
		GameDetectRule rule;
		if ( gamedetect_loadRule( entry.path(), rule ) ) {
			gamedetect_applyLegacyHints( rule );
			rules.push_back( std::move( rule ) );
		}
	}

	std::sort( rules.begin(), rules.end(), []( const GameDetectRule& lhs, const GameDetectRule& rhs ){
		return lhs.gameFile < rhs.gameFile;
	} );
}

std::filesystem::path gamedetect_normaliseInstallRoot( const std::filesystem::path& root, const GameDetectRule& rule ){
	if ( rule.basegame.empty() || root.empty() ) {
		return root;
	}

	const auto leaf = gamedetect_toLower( root.filename().string() );
	const auto base = gamedetect_toLower( rule.basegame );
	if ( leaf != base ) {
		return root;
	}

	const auto parent = root.parent_path();
	if ( parent.empty() ) {
		return root;
	}

	std::error_code err;
	if ( std::filesystem::is_directory( parent / rule.basegame, err ) ) {
		return parent;
	}
	return root;
}

bool gamedetect_hasArchiveType( const std::filesystem::path& directory, const GameDetectRule& rule ){
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
		if ( !ext.empty() && ext.front() == '.' ) {
			ext.erase( ext.begin() );
		}
		ext = gamedetect_toLower( ext );
		for ( const auto& wanted : rule.archiveTypes )
		{
			if ( ext == wanted ) {
				return true;
			}
		}
	}
	return false;
}

bool gamedetect_hasContentHints( const std::filesystem::path& directory ){
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
}

bool gamedetect_hasGameDataAt( const std::filesystem::path& installRoot, const GameDetectRule& rule, int& score ){
	score = 0;
	const std::string cleaned = StringStream( DirectoryCleaned( installRoot.string().c_str() ) ).c_str();
	if ( !file_is_directory( cleaned.c_str() ) ) {
		return false;
	}

	std::vector<std::filesystem::path> baseCandidates;
	baseCandidates.emplace_back( installRoot / rule.basegame );
	if ( gamedetect_toLower( installRoot.filename().string() ) == gamedetect_toLower( rule.basegame ) ) {
		baseCandidates.emplace_back( installRoot );
	}

	bool hasBaseDir = false;
	for ( const auto& basePath : baseCandidates )
	{
		if ( file_is_directory( basePath.string().c_str() ) ) {
			hasBaseDir = true;
			break;
		}
	}
	if ( !hasBaseDir ) {
		return false;
	}
	score += 25;

	bool hasEngine = !rule.engineExecutable.empty() && file_exists( ( installRoot / rule.engineExecutable ).string().c_str() );
	if ( hasEngine ) {
		score += 35;
	}

	bool hasArchives = false;
	bool hasContent = false;
	for ( const auto& basePath : baseCandidates )
	{
		if ( !file_is_directory( basePath.string().c_str() ) ) {
			continue;
		}
		hasArchives = hasArchives || gamedetect_hasArchiveType( basePath, rule );
		hasContent = hasContent || gamedetect_hasContentHints( basePath );
	}
	if ( hasArchives ) {
		score += 25;
	}
	if ( hasContent ) {
		score += 10;
	}

	if ( !rule.requiredFiles.empty() ) {
		for ( const auto& required : rule.requiredFiles )
		{
			const std::filesystem::path rel( required );
			if ( file_exists( ( installRoot / rel ).string().c_str() ) ) {
				continue;
			}

			bool foundInBase = false;
			for ( const auto& basePath : baseCandidates )
			{
				if ( file_exists( ( basePath / rel ).string().c_str() ) ) {
					foundInBase = true;
					break;
				}
			}
			if ( !foundInBase ) {
				return false;
			}
		}
		score += 45;
	}

	if ( !( hasEngine || hasArchives || hasContent || !rule.requiredFiles.empty() ) ) {
		return false;
	}

	const auto installToken = gamedetect_normaliseToken( cleaned.c_str() );
	if ( !installToken.empty() && gamedetect_textMatchesAliases( installToken, rule ) ) {
		score += 15;
	}

	return true;
}

bool gamedetect_findBestMatch( const std::vector<GameDetectRule>& rules, std::string& gameFile, std::string& installPath ){
	if ( rules.empty() ) {
		return false;
	}

	std::filesystem::path current( environment_get_app_path() );
	if ( current.empty() ) {
		return false;
	}

	int bestScore = std::numeric_limits<int>::min();
	int bestDepth = std::numeric_limits<int>::max();
	const GameDetectRule* bestRule = nullptr;
	std::filesystem::path bestPath;

	for ( int depth = 0;; ++depth )
	{
		for ( const auto& rule : rules )
		{
			const auto root = gamedetect_normaliseInstallRoot( current, rule );
			int score = 0;
			if ( !gamedetect_hasGameDataAt( root, rule, score ) ) {
				continue;
			}

			score += std::max( 0, 24 - depth * 2 );
			if ( score > bestScore || ( score == bestScore && depth < bestDepth ) ) {
				bestScore = score;
				bestDepth = depth;
				bestRule = &rule;
				bestPath = root;
			}
		}

		const auto parent = current.parent_path();
		if ( parent.empty() || parent == current ) {
			break;
		}
		current = parent;
	}

	if ( bestRule == nullptr ) {
		return false;
	}

	gameFile = bestRule->gameFile;
	installPath = StringStream( DirectoryCleaned( bestPath.string().c_str() ) ).c_str();
	return true;
}
} // namespace

void gamedetect(){
	bool gamedetect = false;
	for ( int i = 1; i < g_argc; ++i ){
		if ( !strcmp( g_argv[i], "-gamedetect" ) ) {
			gamedetect = true;
			break;
		}
	}
	if ( !gamedetect ) {
		return;
	}

	std::vector<GameDetectRule> rules;
	gamedetect_collectRules( rules );
	if ( rules.empty() ) {
		return;
	}

	std::string gameFile;
	std::string installPath;
	if ( gamedetect_findBestMatch( rules, gameFile, installPath ) ) {
		gamedetect_found_game( gameFile.c_str(), installPath.c_str() );
	}
}

namespace
{
// directories
CopiedString home_path;
CopiedString app_path;
// executable file path
CopiedString app_filepath;
}

const char* environment_get_home_path(){
	return home_path.c_str();
}

const char* environment_get_app_path(){
	return app_path.c_str();
}

const char* environment_get_app_filepath(){
	return app_filepath.c_str();
}

bool portable_app_setup(){
	const auto confdir = StringStream( app_path, "settings/" );
	if ( file_exists( confdir ) ) {
		home_path = confdir;
		return true;
	}
	return false;
}


CopiedString g_openMapByCmd;

void cmdMap(){
	for ( int i = 1; i < g_argc; ++i )
		if( path_extension_is( g_argv[i], "map" ) ){
			g_openMapByCmd = StringStream( PathCleaned( g_argv[i] ) );
			return;
		}
}

#if defined( POSIX )

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

#include <glib.h>

const char* LINK_NAME =
#if defined ( __linux__ )
    "/proc/self/exe"
#else // FreeBSD and OSX
    "/proc/curproc/file"
#endif
    ;

/// brief Returns the filename of the executable belonging to the current process, or empty string, if not found.
const char* getexename( char *buf ){
	/* Now read the symbolic link */
	const int ret = readlink( LINK_NAME, buf, PATH_MAX );

	if ( ret == -1 ) {
		globalWarningStream() << "getexename: falling back to argv[0]: " << Quoted( g_argv[0] );
		if( realpath( g_argv[0], buf ) == 0 )
			*buf = '\0'; /* In case of an error, leave the handling up to the caller */
	}
	else{
		/* Ensure proper NUL termination */
		buf[ret] = 0;
	}
	return buf;
}

void environment_init( int argc, char* argv[] ){
	// Give away unnecessary root privileges.
	// Important: must be done before calling gtk_init().
	char *loginname;
	struct passwd *pw;
	seteuid( getuid() );
	if ( geteuid() == 0 && ( loginname = getlogin() ) != 0 &&
	     ( pw = getpwnam( loginname ) ) != 0 ) {
		setuid( pw->pw_uid );
	}

	args_init( argc, argv );

	{
		char real[PATH_MAX];
		app_filepath = getexename( real );
		ASSERT_MESSAGE( !app_filepath.empty(), "failed to deduce app path" );
		// NOTE: we build app path with a trailing '/'
		// it's a general convention in Radiant to have the slash at the end of directories
		app_path = PathFilenameless( real );
	}

	if ( !portable_app_setup() ) {
		home_path = StringStream( DirectoryCleaned( g_get_home_dir() ), ".netradiant/" );
		Q_mkdir( home_path.c_str() );
	}
	gamedetect();
	cmdMap();
}

#elif defined( WIN32 )

#include <windows.h>

void environment_init( int argc, char* argv[] ){
	args_init( argc, argv );

	{
		// get path to the editor
		char filename[MAX_PATH + 1];
		GetModuleFileName( 0, filename, MAX_PATH );

		app_filepath = StringStream( PathCleaned( filename ) );
		app_path = PathFilenameless( app_filepath.c_str() );
	}

	if ( !portable_app_setup() ) {
		char *appdata = getenv( "APPDATA" );
		StringOutputStream home( 256 );
		if ( !appdata || string_empty( appdata ) ) {
			ERROR_MESSAGE( "Application Data folder not available.\n"
			               "VibeRadiant will use C:\\ for user preferences.\n" );
			home << "C:";
		}
		else
		{
			home << PathCleaned( appdata );
		}
		home << "/VibeRadiantSettings/";
		Q_mkdir( home );
		home_path = home;
	}
	gamedetect();
	cmdMap();
}

#else
#error "unsupported platform"
#endif
