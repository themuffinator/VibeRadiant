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

/*! \mainpage GtkRadiant Documentation Index

   \section intro_sec Introduction

   This documentation is generated from comments in the source code.

   \section links_sec Useful Links

   \link include/itextstream.h include/itextstream.h \endlink - Global output and error message streams, similar to std::cout and std::cerr. \n

   FileInputStream - similar to std::ifstream (binary mode) \n
   FileOutputStream - similar to std::ofstream (binary mode) \n
   TextFileInputStream - similar to std::ifstream (text mode) \n
   TextFileOutputStream - similar to std::ofstream (text mode) \n
   StringOutputStream - similar to std::stringstream \n

   \link string/string.h string/string.h \endlink - C-style string comparison and memory management. \n
   \link os/path.h os/path.h \endlink - Path manipulation for radiant's standard path format \n
   \link os/file.h os/file.h \endlink - OS file-system access. \n

   ::CopiedString - automatic string memory management \n
   Array - automatic array memory management \n
   HashTable - generic hashtable, similar to std::hash_map \n

   \link math/vector.h math/vector.h \endlink - Vectors \n
   \link math/matrix.h math/matrix.h \endlink - Matrices \n
   \link math/quaternion.h math/quaternion.h \endlink - Quaternions \n
   \link math/plane.h math/plane.h \endlink - Planes \n
   \link math/aabb.h math/aabb.h \endlink - AABBs \n

   Callback MemberCaller FunctionCaller - callbacks similar to using boost::function with boost::bind \n
   SmartPointer SmartReference - smart-pointer and smart-reference similar to Loki's SmartPtr \n

   \link generic/bitfield.h generic/bitfield.h \endlink - Type-safe bitfield \n
   \link generic/enumeration.h generic/enumeration.h \endlink - Type-safe enumeration \n

   DefaultAllocator - Memory allocation using new/delete, compliant with std::allocator interface \n

   \link debugging/debugging.h debugging/debugging.h \endlink - Debugging macros \n

 */

#include "main.h"

#include "version.h"

#include "debugging/debugging.h"

#include "commandlib.h"
#include "os/file.h"
#include "os/path.h"
#include "stream/stringstream.h"
#include "stream/textfilestream.h"
#include "character.h"

#include "gtkutil/messagebox.h"
#include "gtkutil/image.h"
#include "gtkutil/i18n.h"
#include "console.h"
#include "texwindow.h"
#include "map.h"
#include "mainframe.h"
#include "commands.h"
#include "preferences.h"
#include "theme.h"
#include "localization.h"
#include "environment.h"
#include "referencecache.h"
#include "stacktrace.h"
#include "error.h"
#include "update.h"
#include "url.h"
#include "mru.h"
#include "qe3.h"

#include <QApplication>
#include <QDate>
#include <QDialog>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gtkutil/glwidget.h"

#ifdef WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

void show_splash();
void set_splash_status( const char* status );
QWidget* splash_window();
void hide_splash();

#if defined ( _DEBUG ) && defined ( WIN32 ) && defined ( _MSC_VER )
#include "crtdbg.h"
#endif

void crt_init(){
#if defined ( _DEBUG ) && defined ( WIN32 ) && defined ( _MSC_VER )
	_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif
}

void qute_messageHandler( QtMsgType type, const QMessageLogContext &context, const QString &msg )
{
	static StringOutputStream buf( 256 );
	buf.clear();
	switch ( type )
	{
	case QtInfoMsg:     buf << "QT INF "; break;
	case QtDebugMsg:    buf << "QT DBG "; break;
	case QtWarningMsg:  buf << "QT WRN "; break;
	case QtCriticalMsg: buf << "QT CRT "; break;
	case QtFatalMsg:    buf << "QT FTL "; break;
	}
	buf << context.category << ": " << msg.toLatin1().constData() << '\n';
	switch ( type )
	{
	case QtInfoMsg:
	case QtDebugMsg:    globalOutputStream() << buf; break;
	case QtWarningMsg:  globalWarningStream() << buf; break;
	case QtCriticalMsg:
	case QtFatalMsg:    globalErrorStream() << buf; break;
	}
}

#ifdef WIN32
namespace
{
using MiniDumpWriteDumpFn = BOOL( WINAPI* )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION );

LONG g_crashReportInProgress = 0;
bool g_crashReportHandlersInstalled = false;
char g_crashReportDir[MAX_PATH] = {};

class CrashLogTextOutputStream : public TextOutputStream
{
	FILE* m_file = nullptr;
public:
	explicit CrashLogTextOutputStream( FILE* file ) : m_file( file ){
	}
	std::size_t write( const char* buffer, std::size_t length ) override {
		return m_file != nullptr ? fwrite( buffer, 1, length, m_file ) : 0;
	}
};

void CrashReport_selectDirectory(){
	const char* envOverride = std::getenv( "VIBERADIANT_CRASH_DIR" );
	if ( envOverride != nullptr && envOverride[0] != '\0' ) {
		std::snprintf( g_crashReportDir, sizeof( g_crashReportDir ), "%s", envOverride );
	}
	else
	{
		const char* base = SettingsPath_get();
		if ( base == nullptr || base[0] == '\0' ) {
			base = environment_get_home_path();
		}
		if ( base == nullptr || base[0] == '\0' ) {
			base = ".";
		}
		std::snprintf( g_crashReportDir, sizeof( g_crashReportDir ), "%s%s", base, "crashes/" );
	}

	const std::size_t length = std::strlen( g_crashReportDir );
	if ( length > 0 && g_crashReportDir[length - 1] != '/' && g_crashReportDir[length - 1] != '\\' ) {
		if ( length + 1 < sizeof( g_crashReportDir ) ) {
			g_crashReportDir[length] = '/';
			g_crashReportDir[length + 1] = '\0';
		}
	}
}

void CrashReport_ensureDirectory(){
	if ( g_crashReportDir[0] == '\0' ) {
		CrashReport_selectDirectory();
	}
	Q_mkdir( g_crashReportDir );
}

void CrashReport_buildFilePaths( char* logPath, std::size_t logPathSize, char* dumpPath, std::size_t dumpPathSize ){
	SYSTEMTIME st;
	GetLocalTime( &st );
	const DWORD pid = GetCurrentProcessId();
	const DWORD tid = GetCurrentThreadId();
	std::snprintf( logPath, logPathSize,
	               "%sviberadiant-crash-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu.log",
	               g_crashReportDir,
	               static_cast<unsigned>( st.wYear ), static_cast<unsigned>( st.wMonth ),
	               static_cast<unsigned>( st.wDay ), static_cast<unsigned>( st.wHour ),
	               static_cast<unsigned>( st.wMinute ), static_cast<unsigned>( st.wSecond ),
	               static_cast<unsigned long>( pid ), static_cast<unsigned long>( tid ) );
	std::snprintf( dumpPath, dumpPathSize,
	               "%sviberadiant-crash-%04u%02u%02u-%02u%02u%02u-p%lu-t%lu.dmp",
	               g_crashReportDir,
	               static_cast<unsigned>( st.wYear ), static_cast<unsigned>( st.wMonth ),
	               static_cast<unsigned>( st.wDay ), static_cast<unsigned>( st.wHour ),
	               static_cast<unsigned>( st.wMinute ), static_cast<unsigned>( st.wSecond ),
	               static_cast<unsigned long>( pid ), static_cast<unsigned long>( tid ) );
}

bool CrashReport_writeMinidump( const char* dumpPath, EXCEPTION_POINTERS* exceptionPointers, FILE* log ){
	HMODULE dbghelp = LoadLibraryA( "DbgHelp.dll" );
	if ( dbghelp == nullptr ) {
		if ( log != nullptr ) {
			std::fprintf( log, "MiniDump: failed to load DbgHelp.dll (error=%lu)\n",
			              static_cast<unsigned long>( GetLastError() ) );
		}
		return false;
	}

	const MiniDumpWriteDumpFn writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
	    GetProcAddress( dbghelp, "MiniDumpWriteDump" ) );
	if ( writeDump == nullptr ) {
		if ( log != nullptr ) {
			std::fprintf( log, "MiniDump: failed to resolve MiniDumpWriteDump (error=%lu)\n",
			              static_cast<unsigned long>( GetLastError() ) );
		}
		FreeLibrary( dbghelp );
		return false;
	}

	HANDLE dumpFile = CreateFileA( dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	if ( dumpFile == INVALID_HANDLE_VALUE ) {
		if ( log != nullptr ) {
			std::fprintf( log, "MiniDump: failed to create dump file '%s' (error=%lu)\n",
			              dumpPath, static_cast<unsigned long>( GetLastError() ) );
		}
		FreeLibrary( dbghelp );
		return false;
	}

	MINIDUMP_EXCEPTION_INFORMATION dumpExceptionInfo = {};
	dumpExceptionInfo.ThreadId = GetCurrentThreadId();
	dumpExceptionInfo.ExceptionPointers = exceptionPointers;
	dumpExceptionInfo.ClientPointers = FALSE;

	const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
	    MiniDumpWithDataSegs
	  | MiniDumpWithHandleData
	  | MiniDumpWithThreadInfo
	  | MiniDumpWithUnloadedModules
	  | MiniDumpWithProcessThreadData
	  | MiniDumpScanMemory );

	const BOOL ok = writeDump(
	    GetCurrentProcess(),
	    GetCurrentProcessId(),
	    dumpFile,
	    dumpType,
	    exceptionPointers != nullptr ? &dumpExceptionInfo : nullptr,
	    nullptr,
	    nullptr );
	const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();

	CloseHandle( dumpFile );
	FreeLibrary( dbghelp );

	if ( log != nullptr ) {
		if ( ok ) {
			std::fprintf( log, "MiniDump: wrote '%s'\n", dumpPath );
		}
		else{
			std::fprintf( log, "MiniDump: failed to write '%s' (error=%lu)\n",
			              dumpPath, static_cast<unsigned long>( writeError ) );
		}
	}

	return ok == TRUE;
}

void CrashReport_write( const char* reason, EXCEPTION_POINTERS* exceptionPointers ){
	if ( InterlockedCompareExchange( &g_crashReportInProgress, 1, 0 ) != 0 ) {
		return;
	}

	CrashReport_ensureDirectory();

	char logPath[MAX_PATH] = {};
	char dumpPath[MAX_PATH] = {};
	CrashReport_buildFilePaths( logPath, sizeof( logPath ), dumpPath, sizeof( dumpPath ) );

	FILE* log = std::fopen( logPath, "wt" );
	if ( log != nullptr ) {
		const DWORD pid = GetCurrentProcessId();
		const DWORD tid = GetCurrentThreadId();
		std::fprintf( log, "VibeRadiant crash report\n" );
		std::fprintf( log, "reason: %s\n", reason != nullptr ? reason : "unknown" );
		std::fprintf( log, "pid: %lu\n", static_cast<unsigned long>( pid ) );
		std::fprintf( log, "tid: %lu\n", static_cast<unsigned long>( tid ) );
		std::fprintf( log, "version: %s\n", RADIANT_VERSION_NUMBER );
		if ( exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr ) {
			const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
			std::fprintf( log, "exceptionCode: 0x%08lX\n", static_cast<unsigned long>( record->ExceptionCode ) );
			std::fprintf( log, "exceptionAddress: %p\n", record->ExceptionAddress );
			std::fprintf( log, "exceptionFlags: 0x%08lX\n", static_cast<unsigned long>( record->ExceptionFlags ) );
		}
		else{
			std::fprintf( log, "exceptionCode: n/a\n" );
			std::fprintf( log, "exceptionAddress: n/a\n" );
		}

		CrashLogTextOutputStream stream( log );
		stream << "stacktrace:\n";
		write_stack_trace( stream );
		stream << "end stacktrace\n";

		std::fflush( log );
	}

	CrashReport_writeMinidump( dumpPath, exceptionPointers, log );

	if ( log != nullptr ) {
		std::fprintf( log, "logFile: %s\n", logPath );
		std::fclose( log );
	}
}

LONG WINAPI CrashReport_unhandledExceptionFilter( EXCEPTION_POINTERS* exceptionPointers ){
	CrashReport_write( "UnhandledExceptionFilter", exceptionPointers );
	return EXCEPTION_EXECUTE_HANDLER;
}

void CrashReport_signalHandler( int signum ){
	CrashReport_write( "signal", nullptr );
	std::_Exit( signum );
}

void CrashReport_terminateHandler(){
	CrashReport_write( "std::terminate", nullptr );
	std::_Exit( EXIT_FAILURE );
}

void CrashReport_InstallHandlers(){
	const char* disable = std::getenv( "VIBERADIANT_DISABLE_CRASH_REPORTING" );
	if ( disable != nullptr && disable[0] != '\0' && std::strcmp( disable, "0" ) != 0 ) {
		return;
	}

	CrashReport_selectDirectory();
	CrashReport_ensureDirectory();

	if ( g_crashReportHandlersInstalled ) {
		return;
	}
	g_crashReportHandlersInstalled = true;

	SetUnhandledExceptionFilter( CrashReport_unhandledExceptionFilter );
	std::signal( SIGABRT, CrashReport_signalHandler );
	std::signal( SIGSEGV, CrashReport_signalHandler );
	std::signal( SIGFPE, CrashReport_signalHandler );
	std::signal( SIGILL, CrashReport_signalHandler );
	std::set_terminate( CrashReport_terminateHandler );
}
} // namespace
#endif

class Lock
{
	bool m_locked;
public:
	Lock() : m_locked( false ){
	}
	void lock(){
		m_locked = true;
	}
	void unlock(){
		m_locked = false;
	}
	bool locked() const {
		return m_locked;
	}
};

class ScopedLock
{
	Lock& m_lock;
public:
	ScopedLock( Lock& lock ) : m_lock( lock ){
		m_lock.lock();
	}
	~ScopedLock(){
		m_lock.unlock();
	}
};

class LineLimitedTextOutputStream : public TextOutputStream
{
	TextOutputStream& outputStream;
	std::size_t count;
public:
	LineLimitedTextOutputStream( TextOutputStream& outputStream, std::size_t count )
		: outputStream( outputStream ), count( count ){
	}
	std::size_t write( const char* buffer, std::size_t length ) override {
		if ( count != 0 ) {
			const char* p = buffer;
			const char* end = buffer + length;
			for (;; )
			{
				p = std::find( p, end, '\n' );
				if ( p == end ) {
					break;
				}
				++p;
				if ( --count == 0 ) {
					length = p - buffer;
					break;
				}
			}
			outputStream.write( buffer, length );
		}
		return length;
	}
};

class PopupDebugMessageHandler : public DebugMessageHandler
{
	StringOutputStream m_buffer;
	Lock m_lock;
public:
	TextOutputStream& getOutputStream() override {
		if ( !m_lock.locked() ) {
			return m_buffer;
		}
		return globalErrorStream();
	}
	bool handleMessage() override {
		getOutputStream() << "----------------\n";
		LineLimitedTextOutputStream outputStream( getOutputStream(), 24 );
		write_stack_trace( outputStream );
		getOutputStream() << "----------------\n";
		globalErrorStream() << m_buffer;
		if ( !m_lock.locked() ) {
			ScopedLock lock( m_lock );
#if defined _DEBUG
			m_buffer << "Break into the debugger?\n";
			bool handled = qt_MessageBox( 0, m_buffer, "VibeRadiant - Runtime Error", EMessageBoxType::Error, eIDYES | eIDNO ) == eIDNO;
			m_buffer.clear();
			return handled;
#else
			m_buffer << "Please report this error to the developers\n";
			qt_MessageBox( 0, m_buffer, "VibeRadiant - Runtime Error", EMessageBoxType::Error );
			m_buffer.clear();
#endif
		}
		return true;
	}
};

typedef Static<PopupDebugMessageHandler> GlobalPopupDebugMessageHandler;

void streams_init(){
	GlobalErrorStream::instance().setOutputStream( getSysPrintErrorStream() );
	GlobalWarningStream::instance().setOutputStream( getSysPrintWarningStream() );
	GlobalOutputStream::instance().setOutputStream( getSysPrintOutputStream() );
}

void paths_init(){
	const char* home = environment_get_home_path();

	if( !string_is_ascii( home ) )
		Error( "Home path is not ASCII: %s", home );

	Q_mkdir( home );

	g_strSettingsPath = StringStream( home, "1." RADIANT_MAJOR_VERSION "." RADIANT_MINOR_VERSION "/" );

	Q_mkdir( g_strSettingsPath.c_str() );

	g_strAppPath = environment_get_app_path();

	if( !string_is_ascii( g_strAppPath.c_str() ) )
		Error( "VibeRadiant path is not ASCII: %s", g_strAppPath.c_str() );

	// radiant is installed in the parent dir of "tools/"
	// NOTE: this is not very easy for debugging
	// maybe add options to lookup in several places?
	// (for now I had to create symlinks)
	BitmapsPath_set( StringStream( g_strAppPath, "bitmaps/" ) );

	// we will set this right after the game selection is done
	g_strGameToolsPath = g_strAppPath;
}

bool check_version_file( const char* filename, const char* version ){
	TextFileInputStream file( filename );
	if ( !file.failed() ) {
		char buf[10];
		buf[file.read( buf, 9 )] = '\0';

		// chomp it (the hard way)
		int chomp = 0;
		while ( buf[chomp] >= '0' && buf[chomp] <= '9' )
			chomp++;
		buf[chomp] = '\0';

		return string_equal( buf, version );
	}
	return false;
}

bool check_version(){
	// a safe check to avoid people running broken installations
	// (otherwise, they run it, crash it, and blame us for not forcing them hard enough to pay attention while installing)
	// make something idiot proof and someone will make better idiots, this may be overkill
	// let's leave it disabled in debug mode in any case
	// http://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=431
#ifndef _DEBUG
	const auto majorVersionFile = StringStream( AppPath_get(), "RADIANT_MAJOR" );
	const auto minorVersionFile = StringStream( AppPath_get(), "RADIANT_MINOR" );

	if ( !( file_exists( majorVersionFile.c_str() ) && file_exists( minorVersionFile.c_str() ) ) ) {
		globalWarningStream() << "Version marker files are missing in " << AppPath_get()
		                      << ", skipping strict install-version check\n";
		return true;
	}

	// locate and open RADIANT_MAJOR and RADIANT_MINOR
	if ( !( check_version_file( majorVersionFile.c_str(), RADIANT_MAJOR_VERSION )
	     && check_version_file( minorVersionFile.c_str(), RADIANT_MINOR_VERSION ) ) ) {
		const auto msg = StringStream(
			"This editor binary (", RADIANT_VERSION, ") doesn't match what the latest setup has configured in this directory\n"
			"Make sure you run the right/latest editor binary you installed\n", AppPath_get() );
		qt_MessageBox( 0, msg, "VibeRadiant" );
		return false;
	}
#endif
	return true;
}

void create_global_pid(){
	/*!
	   the global prefs loading / game selection dialog might fail for any reason we don't know about
	   we need to catch when it happens, to cleanup the stateful prefs which might be killing it
	   and to turn on console logging for lookup of the problem
	   this is the first part of the two step .pid system
	   http://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=297
	 */
	const auto g_pidFile = StringStream( SettingsPath_get(), "viberadiant.pid" ); ///< the global .pid file (only for global part of the startup)

	FILE *pid;
	pid = fopen( g_pidFile, "r" );
	if ( pid != 0 ) {
		fclose( pid );

		if ( remove( g_pidFile ) == -1 ) {
			qt_MessageBox( 0, StringStream( "WARNING: Could not delete ", g_pidFile ), "VibeRadiant", EMessageBoxType::Error );
		}

		// in debug, never prompt to clean registry
#if !defined( _DEBUG )
		const char msg[] = "VibeRadiant failed to start properly the last time it was run.\n"
		                   "The failure may be related to current global preferences.\n"
		                   "Do you want to reset global preferences to defaults?";

		if ( qt_MessageBox( 0, msg, "VibeRadiant - Startup Failure", EMessageBoxType::Question ) == eIDYES ) {
			g_GamesDialog.Reset();
		}

		const auto msg2 = StringStream( "Logging console output to ", SettingsPath_get(),
		                                "viberadiant.log\nRefer to the log if VibeRadiant fails to start again." );
		qt_MessageBox( 0, msg2, "VibeRadiant - Console Log" );
#endif
	}

	// create a primary .pid for global init run
	pid = fopen( g_pidFile, "w" );
	if ( pid ) {
		fclose( pid );
	}
}

void remove_global_pid(){
	const auto g_pidFile = StringStream( SettingsPath_get(), "viberadiant.pid" );
	if ( !file_exists( g_pidFile ) ) {
		return;
	}
	// close the primary
	if ( remove( g_pidFile ) == -1 ) {
		qt_MessageBox( 0, StringStream( "WARNING: Could not delete ", g_pidFile ), "VibeRadiant", EMessageBoxType::Error );
	}
}

/*!
   now the secondary game dependant .pid file
   http://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=297
 */
void create_local_pid(){
	const auto g_pidGameFile = StringStream( SettingsPath_get(), g_pGameDescription->mGameFile, "/viberadiant-game.pid" ); ///< the game-specific .pid file

	FILE *pid = fopen( g_pidGameFile, "r" );
	if ( pid != 0 ) {
		fclose( pid );
		if ( remove( g_pidGameFile ) == -1 ) {
			qt_MessageBox( 0, StringStream( "WARNING: Could not delete ", g_pidGameFile ), "VibeRadiant", EMessageBoxType::Error );
		}

		// in debug, never prompt to clean registry
#if !defined( _DEBUG )
		const char msg[] = "VibeRadiant failed to start properly the last time it was run.\n"
		                   "The failure may be caused by current preferences.\n"
		                   "Do you want to reset all preferences to defaults?";

		if ( qt_MessageBox( 0, msg, "VibeRadiant - Startup Failure", EMessageBoxType::Question ) == eIDYES ) {
			Preferences_Reset();
		}

		const auto msg2 = StringStream( "Logging console output to ", SettingsPath_get(),
		                                "viberadiant.log\nRefer to the log if VibeRadiant fails to start again." );
		qt_MessageBox( 0, msg2, "VibeRadiant - Console Log" );
#endif
	}
	else
	{
		// create one, will remove right after entering message loop
		pid = fopen( g_pidGameFile, "w" );
		if ( pid ) {
			fclose( pid );
		}
	}
}


/*!
   now the secondary game dependant .pid file
   http://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=297
 */
void remove_local_pid(){
	if ( g_pGameDescription == nullptr ) {
		return;
	}
	const auto g_pidGameFile = StringStream( SettingsPath_get(), g_pGameDescription->mGameFile, "/viberadiant-game.pid" );
	if ( !file_exists( g_pidGameFile ) ) {
		return;
	}
	if ( remove( g_pidGameFile ) == -1 ) {
		qt_MessageBox( 0, StringStream( "WARNING: Could not delete ", g_pidGameFile ), "VibeRadiant", EMessageBoxType::Error );
	}
}

namespace
{
class StartupJourneyMetrics
{
	struct Entry
	{
		CopiedString label;
		qint64 durationMs = 0;
	};

	QElapsedTimer m_timer;
	qint64 m_lastElapsed = 0;
	std::vector<Entry> m_entries;
public:
	StartupJourneyMetrics(){
		m_timer.start();
	}

	void mark( const char* label ){
		const qint64 elapsed = m_timer.elapsed();
		Entry e;
		e.label = label ? label : "unknown";
		e.durationMs = elapsed - m_lastElapsed;
		m_entries.push_back( e );
		m_lastElapsed = elapsed;
	}

	void flushToLog() const {
		globalOutputStream() << "Startup journey timings:\n";
		for ( const auto& e : m_entries )
		{
			globalOutputStream() << "  - " << e.label.c_str() << ": " << static_cast<int>( e.durationMs ) << " ms\n";
		}
	}
};

struct StartupCliOptions
{
	bool legacyFlow = false;
	bool noLoadingScreen = false;
	bool noWelcome = false;
	bool diagnostics = false;
	bool debugSkipToLoading = false;
	bool debugMainWindowOnly = false;
	bool debugFastForward() const {
		return debugSkipToLoading || debugMainWindowOnly;
	}
};

StartupCliOptions startup_parse_cli_options( int argc, char* argv[] ){
	StartupCliOptions options;
	for ( int i = 1; i < argc; ++i )
	{
		const char* const arg = argv[i];
		if ( string_equal( arg, "-startup-legacy-flow" ) ) {
			options.legacyFlow = true;
		}
		else if ( string_equal( arg, "-startup-no-loading-screen" ) ) {
			options.noLoadingScreen = true;
		}
		else if ( string_equal( arg, "-startup-no-welcome" ) ) {
			options.noWelcome = true;
		}
		else if ( string_equal( arg, "-startup-diagnostics" ) ) {
			options.diagnostics = true;
		}
		else if ( string_equal( arg, "-startup-debug-skip-to-loading" ) ) {
			options.debugSkipToLoading = true;
		}
		else if ( string_equal( arg, "-startup-debug-mainwindow-only" ) ) {
			options.debugMainWindowOnly = true;
		}
	}
	return options;
}

class StartupLoadingDialog : public QDialog
{
	QLabel *m_title = nullptr;
	QLabel *m_status = nullptr;
	QProgressBar *m_progress = nullptr;
public:
	explicit StartupLoadingDialog() : QDialog( nullptr, Qt::FramelessWindowHint | Qt::Dialog ){
		setModal( false );
		setWindowTitle( i18n::tr( "VibeRadiant Loading" ) );
		setAttribute( Qt::WA_DeleteOnClose, false );
		setMinimumSize( 560, 220 );

		auto *root = new QVBoxLayout( this );
		root->setContentsMargins( 24, 24, 24, 24 );
		root->setSpacing( 12 );

		auto *card = new QFrame( this );
		card->setFrameShape( QFrame::StyledPanel );
		card->setObjectName( "startupLoadingCard" );
		root->addWidget( card );

		auto *layout = new QVBoxLayout( card );
		layout->setContentsMargins( 16, 16, 16, 16 );
		layout->setSpacing( 10 );

		m_title = new QLabel( i18n::tr( "Loading VibeRadiant" ), card );
		auto titleFont = m_title->font();
		titleFont.setPointSize( titleFont.pointSize() + 2 );
		titleFont.setBold( true );
		m_title->setFont( titleFont );
		layout->addWidget( m_title );

		m_status = new QLabel( i18n::tr( "Preparing startup..." ), card );
		m_status->setWordWrap( true );
		layout->addWidget( m_status );

		m_progress = new QProgressBar( card );
		m_progress->setRange( 0, 100 );
		m_progress->setValue( 0 );
		layout->addWidget( m_progress );
	}

	void setProgress( int value, const char* text ){
		m_progress->setValue( value < 0 ? 0 : ( value > 100 ? 100 : value ) );
		m_status->setText( text ? QString::fromLatin1( text ) : i18n::tr( "Preparing startup..." ) );
	}
};

void loading_show( StartupLoadingDialog*& dialog ){
	if ( dialog == nullptr ) {
		dialog = new StartupLoadingDialog();
	}
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

void loading_hide( StartupLoadingDialog*& dialog ){
	if ( dialog != nullptr ) {
		dialog->hide();
		delete dialog;
		dialog = nullptr;
	}
}

class StartupRuntimeGuard
{
	bool m_messageHandlerInstalled = false;
	bool m_logOpen = false;
	bool m_radiantInitialised = false;
	bool m_globalPidActive = false;
	bool m_localPidActive = false;
	bool m_splashVisible = false;
	StartupLoadingDialog* m_loadingDialog = nullptr;
public:
	~StartupRuntimeGuard(){
		cleanup();
	}

	void markMessageHandlerInstalled(){
		m_messageHandlerInstalled = true;
	}

	void openLog(){
		if ( !m_logOpen ) {
			Sys_LogFile( true );
			m_logOpen = true;
		}
	}

	void closeLog(){
		if ( m_logOpen ) {
			Sys_LogFile( false );
			m_logOpen = false;
		}
	}

	void createGlobalPid(){
		create_global_pid();
		m_globalPidActive = true;
	}

	void removeGlobalPid(){
		if ( m_globalPidActive ) {
			remove_global_pid();
			m_globalPidActive = false;
		}
	}

	void createLocalPid(){
		create_local_pid();
		m_localPidActive = true;
	}

	void removeLocalPid(){
		if ( m_localPidActive ) {
			remove_local_pid();
			m_localPidActive = false;
		}
	}

	void showSplash(){
		if ( !m_splashVisible ) {
			::show_splash();
			m_splashVisible = true;
		}
	}

	void hideSplash(){
		if ( m_splashVisible ) {
			::hide_splash();
			m_splashVisible = false;
		}
	}

	void showLoading(){
		loading_show( m_loadingDialog );
	}

	void hideLoading(){
		loading_hide( m_loadingDialog );
	}

	StartupLoadingDialog* loadingDialog() const {
		return m_loadingDialog;
	}

	void markRadiantInitialised(){
		m_radiantInitialised = true;
	}

	void markRadiantShutdown(){
		m_radiantInitialised = false;
	}

	void cleanup(){
		hideLoading();
		hideSplash();
		removeLocalPid();
		removeGlobalPid();

		if ( g_pParentWnd != nullptr ) {
			delete g_pParentWnd;
			g_pParentWnd = nullptr;
		}

		if ( m_radiantInitialised ) {
			Radiant_Shutdown();
			m_radiantInitialised = false;
		}

		if ( m_messageHandlerInstalled ) {
			qInstallMessageHandler( nullptr );
			m_messageHandlerInstalled = false;
		}

		closeLog();
	}
};

void startup_load_initial_map(){
	if( !g_openMapByCmd.empty() ){
		if ( file_readable( g_openMapByCmd.c_str() ) ) {
			Map_LoadFile( g_openMapByCmd.c_str() );
			return;
		}
		const QString msg = i18n::tr( "Could not open map from command line:\n%1\n\nCreating a new map instead." )
			.arg( g_openMapByCmd.c_str() );
		const QByteArray msgUtf8 = msg.toUtf8();
		const QByteArray titleUtf8 = i18n::tr( "Startup" ).toUtf8();
		globalWarningStream() << msgUtf8.constData() << '\n';
		qt_MessageBox( MainFrame_getWindow(), msgUtf8.constData(), titleUtf8.constData(), EMessageBoxType::Warning );
	}
	else if ( g_bLoadLastMap && !g_strLastMap.empty() ) {
		if ( file_readable( g_strLastMap.c_str() ) ) {
			Map_LoadFile( g_strLastMap.c_str() );
			return;
		}
		const QString msg = i18n::tr( "The previous map could not be found:\n%1\n\nCreating a new map instead." )
			.arg( g_strLastMap.c_str() );
		const QByteArray msgUtf8 = msg.toUtf8();
		const QByteArray titleUtf8 = i18n::tr( "Startup" ).toUtf8();
		globalWarningStream() << msgUtf8.constData() << '\n';
		qt_MessageBox( MainFrame_getWindow(), msgUtf8.constData(), titleUtf8.constData(), EMessageBoxType::Warning );
	}

	Map_New();
}

class StartupWelcomeDialog : public QDialog
{
	class AspectPixmapLabel : public QLabel
	{
		QPixmap m_source;
	public:
		explicit AspectPixmapLabel( QWidget* parent = nullptr ) : QLabel( parent ){
			setAlignment( Qt::AlignCenter );
		}

		void setSourcePixmap( const QPixmap& pixmap ){
			m_source = pixmap;
			updateScaledPixmap();
			updateGeometry();
		}

		bool hasHeightForWidth() const override {
			return !m_source.isNull();
		}

		int heightForWidth( int width ) const override {
			if ( m_source.isNull() || width <= 0 ) {
				return QLabel::heightForWidth( width );
			}
			return std::max( 1, ( width * m_source.height() ) / std::max( 1, m_source.width() ) );
		}

	protected:
		void resizeEvent( QResizeEvent* event ) override {
			QLabel::resizeEvent( event );
			updateScaledPixmap();
		}

	private:
		void updateScaledPixmap(){
			if ( m_source.isNull() ) {
				clear();
				return;
			}
			const QSize target = size().isValid() ? size() : m_source.size();
			QLabel::setPixmap( m_source.scaled( target, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
		}
	};

public:
	enum class Action
	{
		None,
		NewMap,
		OpenMap,
		ReopenPrevious,
		GettingStarted,
		Donate,
	};

private:
	Action m_action = Action::None;
	QCheckBox* m_showOnStartup = nullptr;

	QPushButton* createActionButton( const QString& text ){
		auto *button = new QPushButton( text, this );
		button->setMinimumHeight( 36 );
		button->setMinimumWidth( 220 );
		button->setStyleSheet( "QPushButton { text-align: left; padding: 8px 12px; }" );
		button->setCursor( Qt::PointingHandCursor );
		return button;
	}

public:
		explicit StartupWelcomeDialog( QWidget* parent ) : QDialog( parent ){
			setWindowTitle( i18n::tr( "Welcome to VibeRadiant" ) );
			setModal( true );

			auto *root = new QVBoxLayout( this );
			root->setContentsMargins( 14, 14, 14, 14 );
			root->setSpacing( 12 );

			auto *topPanel = new QFrame( this );
			topPanel->setFrameShape( QFrame::StyledPanel );
			topPanel->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
			root->addWidget( topPanel, 1 );

			auto *topGrid = new QGridLayout( topPanel );
			topGrid->setContentsMargins( 0, 0, 0, 0 );
			topGrid->setColumnStretch( 0, 1 );
			topGrid->setRowStretch( 0, 1 );

			const QPixmap heroSource = new_local_image( "splash.png" );
			auto *hero = new AspectPixmapLabel( topPanel );
			hero->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
			hero->setSourcePixmap( heroSource );
			topGrid->addWidget( hero, 0, 0 );

		auto *title = new QLabel( i18n::tr( "VibeRadiant" ), topPanel );
		auto titleFont = title->font();
		titleFont.setBold( true );
		titleFont.setPointSize( titleFont.pointSize() + 6 );
		title->setFont( titleFont );
		title->setStyleSheet( "QLabel { color: white; background-color: rgba(0,0,0,110); padding: 8px 10px; }" );
		topGrid->addWidget( title, 0, 0, Qt::AlignLeft | Qt::AlignTop );

		const QString badge = QStringLiteral( "v%1\n%2" )
			.arg( QString::fromLatin1( RADIANT_VERSION_NUMBER ),
			      QDate::currentDate().toString( QStringLiteral( "yyyy-MM-dd" ) ) );
		auto *versionBadge = new QLabel( badge, topPanel );
		versionBadge->setAlignment( Qt::AlignRight | Qt::AlignTop );
		versionBadge->setStyleSheet( "QLabel { color: white; background-color: rgba(0,0,0,120); padding: 6px; }" );
		topGrid->addWidget( versionBadge, 0, 0, Qt::AlignRight | Qt::AlignTop );

			auto *bottom = new QFrame( this );
			bottom->setFrameShape( QFrame::StyledPanel );
			bottom->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
			root->addWidget( bottom, 0 );

		auto *bottomLayout = new QGridLayout( bottom );
		bottomLayout->setContentsMargins( 16, 16, 16, 16 );
		bottomLayout->setHorizontalSpacing( 24 );
		bottomLayout->setVerticalSpacing( 10 );

		auto *newFileHeader = new QLabel( i18n::tr( "New File" ), bottom );
		auto *helpHeader = new QLabel( i18n::tr( "Getting Started" ), bottom );
		auto headerFont = newFileHeader->font();
		headerFont.setBold( true );
		headerFont.setPointSize( headerFont.pointSize() + 1 );
		newFileHeader->setFont( headerFont );
		helpHeader->setFont( headerFont );

		auto *newMap = createActionButton( i18n::tr( "Create New Map" ) );
		auto *openMap = createActionButton( i18n::tr( "Open Existing Map" ) );
		auto *reopen = createActionButton( i18n::tr( "Reopen Previous Map" ) );
		auto *gettingStarted = createActionButton( i18n::tr( "Getting Started" ) );
		auto *donate = createActionButton( i18n::tr( "Donate" ) );
		auto *continueButton = createActionButton( i18n::tr( "Continue to Editor" ) );
		if ( g_strLastMap.empty() || !file_exists( g_strLastMap.c_str() ) ) {
			reopen->setEnabled( false );
		}

		bottomLayout->addWidget( newFileHeader, 0, 0 );
		bottomLayout->addWidget( helpHeader, 0, 1 );
		bottomLayout->addWidget( newMap, 1, 0 );
		bottomLayout->addWidget( openMap, 2, 0 );
		bottomLayout->addWidget( reopen, 3, 0 );
		bottomLayout->addWidget( gettingStarted, 1, 1 );
		bottomLayout->addWidget( donate, 2, 1 );
		bottomLayout->addWidget( continueButton, 3, 1 );

		m_showOnStartup = new QCheckBox( i18n::tr( "Show this welcome screen on startup" ), bottom );
		m_showOnStartup->setChecked( StartupWelcome_ShowOnStartup() );
		bottomLayout->addWidget( m_showOnStartup, 4, 0, 1, 2, Qt::AlignLeft );

		connect( newMap, &QPushButton::clicked, this, [this](){ m_action = Action::NewMap; accept(); } );
		connect( openMap, &QPushButton::clicked, this, [this](){ m_action = Action::OpenMap; accept(); } );
		connect( reopen, &QPushButton::clicked, this, [this](){ m_action = Action::ReopenPrevious; accept(); } );
		connect( gettingStarted, &QPushButton::clicked, this, [this](){ m_action = Action::GettingStarted; accept(); } );
		connect( donate, &QPushButton::clicked, this, [this](){ m_action = Action::Donate; accept(); } );
		connect( continueButton, &QPushButton::clicked, this, [this](){ m_action = Action::None; accept(); } );

			QScreen* screen = nullptr;
			if ( parentWidget() != nullptr ) {
				screen = parentWidget()->screen();
			}
			if ( screen == nullptr ) {
				screen = this->screen();
			}
			if ( screen == nullptr ) {
				screen = QGuiApplication::primaryScreen();
			}
		const QRect available = ( screen != nullptr )
			? screen->availableGeometry()
			: QRect( 0, 0, 1280, 800 );

		const int availableWidth = std::max( 620, available.width() - 40 );
		const int availableHeight = std::max( 560, available.height() - 40 );
		const int preferredHeroWidth = std::clamp( 720, 560, availableWidth - 28 );
		const int heroHeight = heroSource.isNull()
			? 320
			: std::max( 220, ( preferredHeroWidth * heroSource.height() ) / std::max( 1, heroSource.width() ) );
		const int preferredWidth = std::min( availableWidth, preferredHeroWidth + 28 );
		const int preferredHeight = std::min( availableHeight, heroHeight + 310 );

			setFixedSize( preferredWidth, preferredHeight );

			const QPoint centered(
				available.x() + ( available.width() - width() ) / 2,
				available.y() + ( available.height() - height() ) / 2
			);
			move( centered );
		}

	Action action() const {
		return m_action;
	}

	bool showOnStartup() const {
		return m_showOnStartup != nullptr ? m_showOnStartup->isChecked() : true;
	}
};

void show_startup_welcome_dialog(){
	if ( !StartupWelcome_ShowOnStartup() ) {
		return;
	}

	QWidget* parent = MainFrame_getWindow();
	StartupWelcomeDialog welcome( parent );
	if ( welcome.exec() != QDialog::DialogCode::Accepted ) {
		StartupWelcome_SetShowOnStartup( welcome.showOnStartup() );
		return;
	}
	StartupWelcome_SetShowOnStartup( welcome.showOnStartup() );

	switch ( welcome.action() )
	{
	case StartupWelcomeDialog::Action::NewMap:
		NewMap();
		break;
	case StartupWelcomeDialog::Action::OpenMap:
		OpenMap();
		break;
	case StartupWelcomeDialog::Action::ReopenPrevious:
	{
		if ( g_strLastMap.empty() || !file_readable( g_strLastMap.c_str() ) ) {
			const QByteArray msgUtf8 = i18n::tr( "No previous map could be reopened." ).toUtf8();
			const QByteArray titleUtf8 = i18n::tr( "Welcome" ).toUtf8();
			qt_MessageBox( parent, msgUtf8.constData(), titleUtf8.constData(), EMessageBoxType::Info );
			break;
		}
		const QByteArray openMapUtf8 = i18n::tr( "Open Map" ).toUtf8();
		if ( !ConfirmModified( openMapUtf8.constData() ) ) {
			break;
		}
		MRU_AddFile( g_strLastMap.c_str() );
		Map_Free();
		Map_LoadFile( g_strLastMap.c_str() );
		break;
	}
	case StartupWelcomeDialog::Action::GettingStarted:
		OpenURL( StringStream( AppPath_get(), "docs/index.html" ) );
		break;
	case StartupWelcomeDialog::Action::Donate:
		OpenURL( "https://github.com/sponsors/themuffinator" );
		break;
	case StartupWelcomeDialog::Action::None:
	default:
		break;
	}
}
} // namespace


int main( int argc, char* argv[] ){
#ifdef __linux__
	// Mouse pointer warping functions do not work with the Wayland backend.
	// Forcing the backend to X11 will let us run using XWayland
	// which does provide emulation of this functionality.
	setenv( "QT_QPA_PLATFORM", "xcb", 0 );
#endif

	crt_init();

	streams_init();

#ifdef WIN32
	_setmaxstdio( 2048 );
#endif

	glwidget_setDefaultFormat(); // must go before QApplication instantiation

#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
	QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling );
	// QGuiApplication::setHighDpiScaleFactorRoundingPolicy( Qt::HighDpiScaleFactorRoundingPolicy::PassThrough );
	QCoreApplication::setAttribute( Qt::AA_UseHighDpiPixmaps );
#endif

	QApplication qapplication( argc, argv );
	const StartupCliOptions cliOptions = startup_parse_cli_options( argc, argv );
	StartupRuntimeGuard startupGuard;
	StartupJourneyMetrics startupMetrics;
	const bool startupVerboseLog = cliOptions.diagnostics || cliOptions.debugFastForward();
	auto startup_log_phase = [startupVerboseLog]( const char* phase ){
		if ( startupVerboseLog && phase != nullptr ) {
			globalOutputStream() << "[startup] " << phase << '\n';
		}
	};
	setlocale( LC_NUMERIC, "C" );
	qInstallMessageHandler( qute_messageHandler );
	startupGuard.markMessageHandlerInstalled();
	QCoreApplication::setOrganizationName( "VibeRadiant" );
	QCoreApplication::setApplicationName( "VibeRadiant" );
	QCoreApplication::setApplicationVersion( QT_VERSION_STR );

	GlobalDebugMessageHandler::instance().setHandler( GlobalPopupDebugMessageHandler::instance() );

	environment_init( argc, argv );
#ifdef WIN32
	CrashReport_InstallHandlers();
#endif
	startupMetrics.mark( "Environment initialized" );

	paths_init();
#ifdef WIN32
	CrashReport_InstallHandlers();
#endif
	Localization_init();
	startupMetrics.mark( "Paths and localization initialized" );

	if ( !check_version() ) {
		return EXIT_FAILURE;
	}

	startupGuard.openLog();
	startupMetrics.mark( "Log file opened" );

	QApplication::setWindowIcon( new_local_icon( "radiant.ico" ) ); // before any windows, after paths_init()

	if ( !cliOptions.debugFastForward() ) {
		startupGuard.showSplash();
		set_splash_status( "Loading settings..." );
		startupMetrics.mark( "Splash shown" );
	}
	else
	{
		globalWarningStream() << "Startup debug fast-forward active: skipping splash/update/setup-preflight phases\n";
		if ( cliOptions.debugMainWindowOnly ) {
			globalWarningStream() << "Startup debug mode: mainwindow-only path enabled\n";
		}
		startupMetrics.mark( "Splash skipped by debug flag" );
	}

	startupGuard.createGlobalPid();

	GlobalPreferences_Init();
	startupMetrics.mark( "Global preferences initialized" );

	if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Selecting game profile..." );
	}
	g_GamesDialog.Init();
	startupMetrics.mark( "Game profile selected" );

	g_strGameToolsPath = g_pGameDescription->mGameToolsPath;

	startupGuard.removeGlobalPid();

	if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Loading user preferences..." );
	}
	g_Preferences.Init(); // must occur before create_local_pid() to allow preferences to be reset
	startupMetrics.mark( "Local preference paths initialized" );

	startupMetrics.mark( cliOptions.debugFastForward()
		? "Startup setup preflight skipped by debug flag"
		: "Startup setup preflight deferred" );

	startupGuard.createLocalPid();

	if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Initializing editor modules..." );
	}
	Radiant_Initialise();
	startupGuard.markRadiantInitialised();
	startupMetrics.mark( "Editor modules initialized" );

	startupMetrics.mark( cliOptions.debugFastForward()
		? "Startup update check skipped by debug flag"
		: "Startup update check deferred" );

	const bool useModernStartup = cliOptions.debugFastForward()
		|| ( StartupJourney_ModernEnabled() && !cliOptions.legacyFlow );
	const bool useLoadingScreen = cliOptions.debugFastForward()
		|| ( useModernStartup && StartupJourney_ShowLoadingScreen() && !cliOptions.noLoadingScreen );
	const bool showWelcomeScreen = !cliOptions.debugFastForward()
		&& useModernStartup
		&& !cliOptions.noWelcome
		&& StartupWelcome_ShowOnStartup();

	if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Preparing main window..." );
	}
	startup_log_phase( "construct main frame begin" );
	g_pParentWnd = new MainFrame();
	startup_log_phase( "construct main frame end" );
	if ( g_pParentWnd == nullptr || MainFrame_getWindow() == nullptr ) {
		globalErrorStream() << "Failed to construct main window: null main frame/window\n";
		startupMetrics.mark( "Main window creation failed (null frame/window)" );
		startupMetrics.flushToLog();
		return EXIT_FAILURE;
	}
	if ( MainFrame_getWindow() ) {
		startup_log_phase( "hide main window after construction" );
		MainFrame_getWindow()->hide();
	}
	startupMetrics.mark( "Main window created" );

	if ( useLoadingScreen ) {
		startup_log_phase( "show loading dialog" );
		startupGuard.showLoading();
		if ( startupGuard.loadingDialog() != nullptr ) {
			startupGuard.loadingDialog()->setProgress( 35, "Preparing main window..." );
		}
		if ( !cliOptions.debugFastForward() ) {
			startupGuard.hideSplash();
		}
	}

	if ( useLoadingScreen ) {
		startup_log_phase( "loading dialog progress: map load" );
		if ( startupGuard.loadingDialog() != nullptr ) {
			startupGuard.loadingDialog()->setProgress( 55, "Loading map..." );
		}
	}
	else if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Loading map..." );
	}
	if ( !cliOptions.debugMainWindowOnly ) {
		startup_log_phase( "initial map load begin" );
		startup_load_initial_map();
		startup_log_phase( "initial map load end" );
	}
	else
	{
		startup_log_phase( "initial map load skipped by mainwindow-only debug mode" );
	}
	startupMetrics.mark( "Initial map loaded" );

	if ( useLoadingScreen ) {
		startup_log_phase( "loading dialog progress: theme apply" );
		if ( startupGuard.loadingDialog() != nullptr ) {
			startupGuard.loadingDialog()->setProgress( 85, "Applying startup theme..." );
		}
	}
	else if ( !cliOptions.debugFastForward() ) {
		set_splash_status( "Applying startup theme..." );
	}
	if ( !cliOptions.debugMainWindowOnly ) {
		startup_log_phase( "theme apply begin" );
		theme_apply_startup();
		startup_log_phase( "theme apply end" );
	}
	else
	{
		startup_log_phase( "theme apply skipped by mainwindow-only debug mode" );
	}
	startupMetrics.mark( "Startup theme applied" );

	if ( useLoadingScreen ) {
		startup_log_phase( "loading dialog progress: startup complete" );
		if ( startupGuard.loadingDialog() != nullptr ) {
			startupGuard.loadingDialog()->setProgress( 100, "Startup complete." );
		}
		startup_log_phase( "hide loading dialog" );
		startupGuard.hideLoading();
	}

	if ( !useLoadingScreen && !cliOptions.debugFastForward() ) {
		set_splash_status( "Opening editor..." );
		startupGuard.hideSplash();
	}

	startupGuard.removeLocalPid();
	if ( MainFrame_getWindow() ) {
		startup_log_phase( "show main window begin" );
		MainFrame_getWindow()->show();
		MainFrame_getWindow()->raise();
		MainFrame_getWindow()->activateWindow();
		startup_log_phase( "show main window end" );
	}
	startupMetrics.mark( "Main window shown" );

	if ( !cliOptions.debugFastForward() ) {
		QTimer::singleShot( 0, [showWelcomeScreen](){
			Startup_PreMainWindowSetup();
			if ( showWelcomeScreen ) {
				show_startup_welcome_dialog();
			}
			UpdateManager_MaybeAutoCheck();
		} );
	}

	if ( cliOptions.diagnostics || useModernStartup ) {
		startupMetrics.flushToLog();
	}

	QApplication::exec();

	Map_Free();

	if ( !Map_Unnamed( g_map ) ) {
		g_strLastMap = Map_Name( g_map );
	}

//	user_shortcuts_save();

	return EXIT_SUCCESS;
}
