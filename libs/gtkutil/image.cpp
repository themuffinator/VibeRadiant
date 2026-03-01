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

#include "image.h"

#include "os/file.h"
#include "os/path.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringBuilder>
#include <array>
#include <QColor>


namespace
{
CopiedString g_bitmapsPath;

QString qhex( const QColor& color ){
	return QStringLiteral( "#%1%2%3" )
		.arg( color.red(),   2, 16, QChar( '0' ) )
		.arg( color.green(), 2, 16, QChar( '0' ) )
		.arg( color.blue(),  2, 16, QChar( '0' ) )
		.toUpper();
}

void recolorSvgInPlace( QString& svg, const QString& primary, const QString& muted, const QString& disabled ){
	const auto replaceColor = [&svg]( const char* from, const QString& to ){
		svg.replace(
			QRegularExpression( QStringLiteral( "(?i)%1" ).arg( QRegularExpression::escape( QString::fromLatin1( from ) ) ) ),
			to
		);
	};

	for( const char* color : { "#C0C0C0", "#C0C0C1", "#DBDADA", "#FFFFFF" } )
		replaceColor( color, primary );
	for( const char* color : { "#8E8E8E", "#9D9D9D", "#7A7A7A" } )
		replaceColor( color, muted );
	for( const char* color : { "#666666", "#575757" } )
		replaceColor( color, disabled );
}

void copyAdaptiveIcons( const QDir& sourceDir, const QDir& targetDir, const QString& primary, const QString& muted, const QString& disabled ){
	if( !sourceDir.exists() ){
		return;
	}

	QDir target( targetDir );
	target.mkpath( "." );

	QDir source( sourceDir );
	source.setFilter( QDir::Filter::Files );
	source.setNameFilters( QStringList() << "*.svg" << "*.png" << "*.ico" );

	for( const QFileInfo& fileinfo : source.entryInfoList() )
	{
		QFile file( fileinfo.absoluteFilePath() );
		if( !file.open( QIODevice::OpenModeFlag::ReadOnly ) ){
			continue;
		}

		QByteArray data = file.readAll();
		file.close();

		if( fileinfo.suffix().compare( "svg", Qt::CaseInsensitive ) == 0 ){
			QString svg = QString::fromUtf8( data );
			recolorSvgInPlace( svg, primary, muted, disabled );
			data = svg.toUtf8();
		}

		QFile outFile( target.absoluteFilePath( fileinfo.fileName() ) );
		if( outFile.open( QIODevice::OpenModeFlag::WriteOnly | QIODevice::OpenModeFlag::Truncate ) ){
			outFile.write( data );
		}
	}
}

QString buildAdaptiveThemeName( const QColor& primary, const QColor& muted, const QColor& disabled ){
	auto compact = []( const QColor& color ){
		return QStringLiteral( "%1%2%3" )
			.arg( color.red(),   2, 16, QChar( '0' ) )
			.arg( color.green(), 2, 16, QChar( '0' ) )
			.arg( color.blue(),  2, 16, QChar( '0' ) )
			.toLower();
	};
	return QStringLiteral( "bitmaps_adaptive_%1_%2_%3" )
		.arg( compact( primary ) )
		.arg( compact( muted ) )
		.arg( compact( disabled ) );
}

void writeIndexThemeFile( const QDir& themeDir, const QString& themeName ){
	QFile indexFile( themeDir.absoluteFilePath( "index.theme" ) );
	if( !indexFile.open( QIODevice::OpenModeFlag::WriteOnly | QIODevice::OpenModeFlag::Truncate ) ){
		return;
	}

	const QString contents = QStringLiteral(
		"[Icon Theme]\n"
		"Name=%1\n"
		"Comment=VibeRadiant adaptive icon theme\n"
		"Directories=.,plugins\n"
		"\n"
		"[.]\n"
		"Size=32\n"
		"Type=Scalable\n"
		"MinSize=8\n"
		"MaxSize=256\n"
		"\n"
		"[plugins]\n"
		"Size=32\n"
		"Type=Scalable\n"
		"MinSize=8\n"
		"MaxSize=256\n"
	).arg( themeName );

	indexFile.write( contents.toUtf8() );
}

QString ensureAdaptiveTheme( const char* appPath, const char* settingsPath, const QColor& primary, const QColor& muted, const QColor& disabled ){
	const QString adaptiveThemeName = buildAdaptiveThemeName( primary, muted, disabled );
	const QDir settingsRoot( QString::fromLatin1( settingsPath ) );
	const QDir adaptiveThemeDir( settingsRoot.absoluteFilePath( adaptiveThemeName ) );
	const QFileInfo indexInfo( adaptiveThemeDir.absoluteFilePath( "index.theme" ) );

	if( !indexInfo.exists() ){
		QDir themeDir( adaptiveThemeDir );
		themeDir.mkpath( "." );
		themeDir.mkpath( "plugins" );

		const QString primaryHex = qhex( primary );
		const QString mutedHex = qhex( muted );
		const QString disabledHex = qhex( disabled );

		copyAdaptiveIcons(
			QDir( QString::fromLatin1( appPath ) + "bitmaps/" ),
			themeDir,
			primaryHex, mutedHex, disabledHex
		);
		copyAdaptiveIcons(
			QDir( QString::fromLatin1( appPath ) + "plugins/bitmaps/" ),
			QDir( themeDir.absoluteFilePath( "plugins" ) ),
			primaryHex, mutedHex, disabledHex
		);
		writeIndexThemeFile( themeDir, adaptiveThemeName );
	}

	return adaptiveThemeName;
}
}

void BitmapsPath_set( const char* path ){
	g_bitmapsPath = path;
}

/* generate in settings path, app path may have no write permission */
void Bitmaps_generateLight( const char *appPath, const char *settingsPath ){
	const char *fromto[][2] = { { "bitmaps/", "bitmaps_light/" }, { "plugins/bitmaps/", "plugins/bitmaps/" } };
	for( const auto& [ f, t ] : fromto )
	{
		QDir from( QString( appPath ) + f );
		QDir to( QString( settingsPath ) + t );
		for( auto *d : { &from, &to } ){
			d->setNameFilters( QStringList() << "*.svg" << "*.png" << "*.ico" << "*.theme" );
			d->setFilter( QDir::Filter::Files );
		}

		if( to.count() < from.count() ){
			to.mkpath( to.absolutePath() );
			for( const QFileInfo& fileinfo : from.entryInfoList() )
			{
				QFile file( fileinfo.absoluteFilePath() );
				if( file.open( QIODevice::OpenModeFlag::ReadOnly ) ){
					QByteArray data( file.readAll() );
					if( fileinfo.suffix() == "svg" )
						data.replace( "#C0C0C0", "#575757" );
					QFile outfile( to.absolutePath() + '/' + fileinfo.fileName() );
					if( outfile.open( QIODevice::OpenModeFlag::WriteOnly ) )
						outfile.write( data );
				}
			}
		}
	}
}

void Bitmaps_configureTheme( const char* appPath, const char* settingsPath, const QColor& primary, const QColor& muted, const QColor& disabled ){
	const QString adaptiveThemeName = ensureAdaptiveTheme( appPath, settingsPath, primary, muted, disabled );
	const QString bitmapsPath = QDir( QString::fromLatin1( settingsPath ) ).absoluteFilePath( adaptiveThemeName + '/' );
	BitmapsPath_set( bitmapsPath.toLatin1().constData() );

	QStringList searchPaths = QIcon::themeSearchPaths();
	for( const QString& path : { QString::fromLatin1( settingsPath ), QString::fromLatin1( appPath ) } )
	{
		if( !searchPaths.contains( path ) ){
			searchPaths.push_back( path );
		}
	}
	QIcon::setThemeSearchPaths( searchPaths );
	QIcon::setThemeName( adaptiveThemeName );
}

QPixmap new_local_image( const char* filename ){
	StringOutputStream fullpath( 256 );

	for( const auto *ext : { ".svg", ".png" } )
		if( file_exists( fullpath( g_bitmapsPath, PathExtensionless( filename ), ext ) ) )
			return QPixmap( fullpath.c_str() );

	return {};
}

QIcon new_local_icon( const char* filename ){
	if( QString name( CopiedString( PathExtensionless( filename ) ).c_str() ); QIcon::hasThemeIcon( name ) )
		return QIcon::fromTheme( name );

	StringOutputStream fullpath( 256 );

	for( const auto *ext : { ".svg", ".png", ".ico" } )
		if( file_exists( fullpath( g_bitmapsPath, PathExtensionless( filename ), ext ) ) )
			return QIcon( fullpath.c_str() );

	return {};
}
