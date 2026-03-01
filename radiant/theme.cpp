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

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QColorDialog>
#include <QFont>
#include <QGuiApplication>
#include <QMenu>
#include <QStyleHints>
#include <array>
#include <algorithm>
#include <cmath>

#include "colors.h"
#include "preferences.h"
#include "mainframe.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "stream/stringstream.h"
#include "gtkutil/image.h"
#include "gtkutil/i18n.h"


enum class ETheme{
	System = 0,
	Light,
	Dark,
	Darker,
	Blender,
};

enum class EInterfaceDensity{
	Compact = 0,
	Standard,
	Comfortable,
};

static ETheme s_theme = ETheme::System;
static EInterfaceDensity s_density = EInterfaceDensity::Standard;
static bool s_accentOverrideEnabled = false;
static QColor s_accentOverride = QColor( 88, 157, 234 );
static bool s_themeApplied = false;

struct ThemeVisuals
{
	const char* viewportPreset;

	QColor window;
	QColor windowAlt;
	QColor surface;
	QColor input;
	QColor inputAlt;
	QColor text;
	QColor textMuted;
	QColor border;
	QColor borderStrong;
	QColor accent;
	QColor selectionText;
};

struct ThemeDensity
{
	const char* label;
	double uiScale;
	double fontDeltaPt;
};

const ThemeDensity& density_for( EInterfaceDensity density ){
	switch( density )
	{
	case EInterfaceDensity::Compact:
	{
		static const ThemeDensity compact{ "Compact", 0.90, -1.0 };
		return compact;
	}
	case EInterfaceDensity::Comfortable:
	{
		static const ThemeDensity comfortable{ "Comfortable", 1.16, +1.0 };
		return comfortable;
	}
	case EInterfaceDensity::Standard:
	default:
	{
		static const ThemeDensity standard{ "Standard", 1.0, 0.0 };
		return standard;
	}
	}
}

int density_px( const ThemeDensity& density, int value ){
	return std::max( 1, static_cast<int>( std::lround( static_cast<double>( value ) * density.uiScale ) ) );
}

QString qss_color( const QColor& color ){
	if( color.alpha() == 255 ){
		return QString( "rgb(%1, %2, %3)" ).arg( color.red() ).arg( color.green() ).arg( color.blue() );
	}
	return QString( "rgba(%1, %2, %3, %4)" )
		.arg( color.red() )
		.arg( color.green() )
		.arg( color.blue() )
		.arg( color.alphaF(), 0, 'f', 3 );
}

double wcag_luminance( const QColor& color ){
	const auto channel = []( double value ){
		value /= 255.0;
		return value <= 0.03928 ? value / 12.92 : std::pow( ( value + 0.055 ) / 1.055, 2.4 );
	};
	return 0.2126 * channel( color.red() ) + 0.7152 * channel( color.green() ) + 0.0722 * channel( color.blue() );
}

double wcag_contrast_ratio( const QColor& a, const QColor& b ){
	const double la = wcag_luminance( a );
	const double lb = wcag_luminance( b );
	const double bright = std::max( la, lb );
	const double dark = std::min( la, lb );
	return ( bright + 0.05 ) / ( dark + 0.05 );
}

QColor accent_with_contrast_guardrails( const QColor& requested, const QColor& surface ){
	QColor accent = requested.isValid() ? requested : QColor( 88, 157, 234 );
	const bool surfaceIsDark = wcag_luminance( surface ) < 0.5;
	const double targetContrast = 3.0;

	for( int i = 0; i < 64 && wcag_contrast_ratio( accent, surface ) < targetContrast; ++i )
	{
		const int nextLightness = accent.lightness() + ( surfaceIsDark ? 3 : -3 );
		accent = QColor::fromHsl(
			accent.hslHue(),
			accent.hslSaturation(),
			std::clamp( nextLightness, 0, 255 ),
			accent.alpha()
		);
	}
	return accent;
}

QColor readable_selection_text( const QColor& accent ){
	const QColor black( 0, 0, 0 );
	const QColor white( 255, 255, 255 );
	return wcag_contrast_ratio( black, accent ) >= wcag_contrast_ratio( white, accent ) ? black : white;
}

bool os_prefers_dark_theme(){
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
	const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
	if( scheme == Qt::ColorScheme::Dark ){
		return true;
	}
	if( scheme == Qt::ColorScheme::Light ){
		return false;
	}
#endif
	return qApp->palette().color( QPalette::Window ).lightnessF() < 0.5f;
}

const ThemeVisuals& light_theme(){
	static const ThemeVisuals theme{
		"Q3Radiant Original",
		QColor( 238, 240, 244 ),
		QColor( 246, 248, 252 ),
		QColor( 231, 235, 241 ),
		QColor( 255, 255, 255 ),
		QColor( 248, 250, 252 ),
		QColor( 31, 36, 43 ),
		QColor( 102, 113, 125 ),
		QColor( 193, 200, 210 ),
		QColor( 164, 174, 187 ),
		QColor( 47, 111, 188 ),
		QColor( 255, 255, 255 ),
	};
	return theme;
}

const ThemeVisuals& dark_theme(){
	static const ThemeVisuals theme{
		"Adwaita Dark",
		QColor( 50, 55, 60 ),
		QColor( 57, 63, 70 ),
		QColor( 65, 72, 80 ),
		QColor( 38, 43, 49 ),
		QColor( 46, 52, 59 ),
		QColor( 231, 236, 242 ),
		QColor( 170, 179, 189 ),
		QColor( 86, 96, 107 ),
		QColor( 111, 123, 137 ),
		QColor( 88, 157, 234 ),
		QColor( 16, 20, 25 ),
	};
	return theme;
}

const ThemeVisuals& darker_theme(){
	static const ThemeVisuals theme{
		"Adwaita Dark",
		QColor( 29, 32, 36 ),
		QColor( 35, 39, 44 ),
		QColor( 41, 46, 52 ),
		QColor( 21, 24, 28 ),
		QColor( 28, 32, 37 ),
		QColor( 229, 233, 238 ),
		QColor( 150, 160, 172 ),
		QColor( 58, 65, 74 ),
		QColor( 78, 87, 100 ),
		QColor( 83, 148, 219 ),
		QColor( 13, 17, 22 ),
	};
	return theme;
}

const ThemeVisuals& blender_theme(){
	// Based on Blender's bundled default theme values (userdef_default_theme.c).
	static const ThemeVisuals theme{
		"Blender Dark",
		QColor( 48, 48, 48 ),
		QColor( 61, 61, 61 ),
		QColor( 84, 84, 84 ),
		QColor( 29, 29, 29 ),
		QColor( 34, 34, 34 ),
		QColor( 230, 230, 230 ),
		QColor( 184, 184, 184 ),
		QColor( 61, 61, 61 ),
		QColor( 36, 36, 36 ),
		QColor( 71, 114, 179 ),
		QColor( 255, 255, 255 ),
	};
	return theme;
}

ETheme resolve_theme( ETheme theme ){
	if( theme == ETheme::System ){
		return os_prefers_dark_theme()? ETheme::Dark : ETheme::Light;
	}
	return theme;
}

const ThemeVisuals& visuals_for_theme( ETheme theme ){
	switch( resolve_theme( theme ) )
	{
	case ETheme::Light: return light_theme();
	case ETheme::Dark: return dark_theme();
	case ETheme::Darker: return darker_theme();
	case ETheme::Blender: return blender_theme();
	case ETheme::System:
	default:
		return dark_theme();
	}
}

ThemeVisuals visuals_with_overrides( ETheme theme ){
	ThemeVisuals visuals = visuals_for_theme( theme );
	if( s_accentOverrideEnabled ){
		visuals.accent = accent_with_contrast_guardrails( s_accentOverride, visuals.window );
		visuals.selectionText = readable_selection_text( visuals.accent );
	}
	return visuals;
}

void set_icon_theme( const ThemeVisuals& visuals ){
	Bitmaps_configureTheme( AppPath_get(), SettingsPath_get(), visuals.text, visuals.textMuted, visuals.borderStrong );
}

QPalette palette_for_theme( const ThemeVisuals& theme ){
	QPalette palette;
	const QColor disabledText = theme.textMuted;

	palette.setColor( QPalette::Window, theme.window );
	palette.setColor( QPalette::WindowText, theme.text );
	palette.setColor( QPalette::Base, theme.input );
	palette.setColor( QPalette::AlternateBase, theme.inputAlt );
	palette.setColor( QPalette::ToolTipBase, theme.surface );
	palette.setColor( QPalette::ToolTipText, theme.text );
	palette.setColor( QPalette::Text, theme.text );
	palette.setColor( QPalette::Button, theme.surface );
	palette.setColor( QPalette::ButtonText, theme.text );
	palette.setColor( QPalette::BrightText, QColor( 255, 80, 80 ) );
	palette.setColor( QPalette::Link, theme.accent );
	palette.setColor( QPalette::Highlight, theme.accent );
	palette.setColor( QPalette::HighlightedText, theme.selectionText );

	palette.setColor( QPalette::Disabled, QPalette::WindowText, disabledText );
	palette.setColor( QPalette::Disabled, QPalette::Text, disabledText );
	palette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledText );
	palette.setColor( QPalette::Disabled, QPalette::Base, theme.windowAlt );
	palette.setColor( QPalette::Disabled, QPalette::Button, theme.windowAlt );
	palette.setColor( QPalette::Disabled, QPalette::Highlight, theme.border );
	palette.setColor( QPalette::Disabled, QPalette::HighlightedText, disabledText );
#if QT_VERSION >= QT_VERSION_CHECK( 5, 12, 0 )
	palette.setColor( QPalette::PlaceholderText, theme.textMuted );
	palette.setColor( QPalette::Disabled, QPalette::PlaceholderText, disabledText );
#endif

	return palette;
}

QString stylesheet_for_theme( const ThemeVisuals& theme, const ThemeDensity& density, const QFont& font ){
	QColor accentSubtle = theme.accent;
	accentSubtle.setAlpha( 95 );
	QColor accentSubtleHover = theme.accent;
	accentSubtleHover.setAlpha( 150 );
	const QColor accentHover = theme.accent.lighter( 112 );
	const QColor accentPressed = theme.accent.darker( 112 );
	const QColor button = theme.surface;
	const QColor buttonHover = theme.surface.lighter( 110 );
	const QColor buttonPressed = theme.surface.darker( 112 );
	const QColor disabledBg = theme.windowAlt.darker( 104 );
	QColor headerBg = theme.windowAlt;
	headerBg.setAlpha( 245 );
	const QColor hoverRow = theme.inputAlt.lighter( 108 );
	const QColor scrollbarBg = theme.window;
	const QColor scrollbarHandle = theme.surface;
	const QColor scrollbarHover = theme.surface.lighter( 118 );

	const int fontPt = std::max( 7, static_cast<int>( std::lround( std::max( 7.0, font.pointSizeF() ) ) ) );
	const int menuBarPadV = density_px( density, 5 );
	const int menuBarPadH = density_px( density, 8 );
	const int menuItemPadV = density_px( density, 6 );
	const int menuItemPadH = density_px( density, 24 );
	const int toolButtonMin = density_px( density, 22 );
	const int toolButtonPad = density_px( density, 2 );
	const int buttonPadV = density_px( density, 4 );
	const int buttonPadH = density_px( density, 8 );
	const int inputPadV = density_px( density, 3 );
	const int inputPadH = density_px( density, 6 );
	const int listItemPad = density_px( density, 2 );
	const int headerPadV = density_px( density, 4 );
	const int headerPadH = density_px( density, 6 );
	const int tabPadV = density_px( density, 5 );
	const int tabPadH = density_px( density, 10 );
	const int dockTitlePadV = density_px( density, 5 );
	const int dockTitlePadH = density_px( density, 8 );
	const int groupMarginTop = density_px( density, 11 );
	const int groupPaddingTop = density_px( density, 10 );
	const int checkSpacing = density_px( density, 6 );
	const int checkIndicator = density_px( density, 15 );
	const int scrollbarSize = density_px( density, 10 );
	const int scrollbarMargin = density_px( density, 2 );
	const int scrollbarMinHandle = density_px( density, 24 );

	QString qss = QStringLiteral( R"QSS(
QWidget {
	color: @TEXT@;
	background-color: @WINDOW@;
	font-size: @FONT_PT@pt;
	selection-background-color: @ACCENT@;
	selection-color: @SELECTION_TEXT@;
}

QWidget:disabled {
	color: @DISABLED_TEXT@;
}

QWidget[workspaceFocus="true"] {
	border: 1px solid @ACCENT_HOVER@;
	border-radius: 5px;
	background-color: @ACCENT_SUBTLE@;
}

QMainWindow::separator {
	background: @BORDER@;
	width: 6px;
	height: 6px;
}

QMainWindow::separator:hover {
	background: @ACCENT@;
}

QStatusBar {
	background: @WINDOW_ALT@;
	color: @TEXT_MUTED@;
	border-top: 1px solid @BORDER@;
}

QStatusBar::item {
	border: none;
}

QMenuBar {
	background: @WINDOW_ALT@;
	border-bottom: 1px solid @BORDER@;
	padding: 2px;
	spacing: 2px;
}

QMenuBar::item {
	background: transparent;
	padding: @MENUBAR_PAD_V@px @MENUBAR_PAD_H@px;
	border-radius: 4px;
}

QMenuBar::item:selected {
	background: @ACCENT_SUBTLE@;
}

QMenu {
	background: @SURFACE@;
	border: 1px solid @BORDER_STRONG@;
	padding: 4px;
}

QMenu::item {
	padding: @MENUITEM_PAD_V@px @MENUITEM_PAD_H@px;
	border-radius: 4px;
}

QMenu::item:selected {
	background: @ACCENT_SUBTLE_HOVER@;
}

QMenu::separator {
	background: @BORDER@;
	height: 1px;
	margin: 5px 8px;
}

QToolTip {
	color: @TEXT@;
	background: @SURFACE@;
	border: 1px solid @BORDER_STRONG@;
	padding: 4px 6px;
}

QToolBar {
	background: @WINDOW_ALT@;
	border: 1px solid @BORDER@;
	padding: 3px;
	spacing: 2px;
}

QToolBar::separator {
	background: @BORDER@;
	width: 1px;
	height: 1px;
	margin: 4px 6px;
}

QToolBar QToolButton {
	min-width: @TOOLBTN_MIN@px;
	min-height: @TOOLBTN_MIN@px;
	padding: @TOOLBTN_PAD@px;
}

QToolButton,
QPushButton {
	background: @BUTTON@;
	border: 1px solid @BORDER@;
	border-radius: 5px;
	padding: @BUTTON_PAD_V@px @BUTTON_PAD_H@px;
}

QToolButton:hover,
QPushButton:hover {
	background: @BUTTON_HOVER@;
	border-color: @BORDER_STRONG@;
}

QToolButton:pressed,
QPushButton:pressed {
	background: @BUTTON_PRESSED@;
}

QToolButton:checked,
QToolButton:on,
QPushButton:checked {
	background: @ACCENT_SUBTLE@;
	border-color: @ACCENT@;
}

QToolButton:checked:hover,
QToolButton:on:hover,
QPushButton:checked:hover {
	background: @ACCENT_SUBTLE_HOVER@;
	border-color: @ACCENT_HOVER@;
}

QToolButton:disabled,
QPushButton:disabled {
	background: @DISABLED_BG@;
	color: @DISABLED_TEXT@;
	border-color: @BORDER@;
}

QLineEdit,
QTextEdit,
QPlainTextEdit,
QSpinBox,
QDoubleSpinBox,
QAbstractSpinBox,
QComboBox {
	background: @INPUT@;
	border: 1px solid @BORDER@;
	border-radius: 4px;
	padding: @INPUT_PAD_V@px @INPUT_PAD_H@px;
}

QLineEdit:focus,
QTextEdit:focus,
QPlainTextEdit:focus,
QSpinBox:focus,
QDoubleSpinBox:focus,
QAbstractSpinBox:focus,
QComboBox:focus {
	border-color: @ACCENT@;
}

QLineEdit:disabled,
QTextEdit:disabled,
QPlainTextEdit:disabled,
QSpinBox:disabled,
QDoubleSpinBox:disabled,
QAbstractSpinBox:disabled,
QComboBox:disabled {
	background: @DISABLED_BG@;
}

QComboBox::drop-down {
	border: none;
	width: 20px;
}

QComboBox QAbstractItemView {
	background: @INPUT@;
	selection-background-color: @ACCENT_SUBTLE_HOVER@;
}

QAbstractItemView {
	background: @INPUT@;
	alternate-background-color: @INPUT_ALT@;
	border: 1px solid @BORDER@;
	gridline-color: @BORDER@;
	outline: none;
}

QAbstractItemView::item {
	padding: @LISTITEM_PAD@px;
}

QAbstractItemView::item:hover {
	background: @HOVER_ROW@;
}

QAbstractItemView::item:selected {
	background: @ACCENT_SUBTLE@;
}

QHeaderView::section {
	background: @HEADER_BG@;
	border: 1px solid @BORDER@;
	padding: @HEADER_PAD_V@px @HEADER_PAD_H@px;
}

QTabWidget::pane {
	border: 1px solid @BORDER@;
	top: -1px;
	background: @WINDOW@;
}

QTabBar::tab {
	background: @WINDOW_ALT@;
	color: @TEXT_MUTED@;
	border: 1px solid @BORDER@;
	border-bottom: none;
	border-top-left-radius: 6px;
	border-top-right-radius: 6px;
	padding: @TAB_PAD_V@px @TAB_PAD_H@px;
	margin-right: 2px;
}

QTabBar::tab:selected {
	background: @WINDOW@;
	color: @TEXT@;
	border-color: @BORDER_STRONG@;
}

QTabBar::tab:!selected:hover {
	background: @BUTTON_HOVER@;
	color: @TEXT@;
}

QDockWidget {
	border: 1px solid @BORDER@;
}

QDockWidget::title {
	background: @WINDOW_ALT@;
	border: 1px solid @BORDER@;
	padding: @DOCKTITLE_PAD_V@px @DOCKTITLE_PAD_H@px;
	text-align: left;
}

QGroupBox {
	border: 1px solid @BORDER@;
	border-radius: 5px;
	margin-top: @GROUP_MARGIN_TOP@px;
	padding-top: @GROUP_PADDING_TOP@px;
}

QGroupBox::title {
	subcontrol-origin: margin;
	left: 9px;
	padding: 0 4px;
	color: @TEXT_MUTED@;
}

QCheckBox,
QRadioButton {
	spacing: @CHECK_SPACING@px;
}

QCheckBox::indicator,
QRadioButton::indicator {
	width: @CHECK_INDICATOR@px;
	height: @CHECK_INDICATOR@px;
	border: 1px solid @BORDER_STRONG@;
	background: @INPUT@;
}

QCheckBox::indicator {
	border-radius: 3px;
}

QRadioButton::indicator {
	border-radius: 8px;
}

QCheckBox::indicator:checked,
QRadioButton::indicator:checked {
	background: @ACCENT@;
	border-color: @ACCENT_HOVER@;
}

QCheckBox::indicator:disabled,
QRadioButton::indicator:disabled {
	background: @DISABLED_BG@;
	border-color: @BORDER@;
}

QScrollBar:vertical {
	background: @SCROLLBAR_BG@;
	width: @SCROLLBAR_SIZE@px;
	margin: @SCROLLBAR_MARGIN@px;
	border: none;
}

QScrollBar::handle:vertical {
	background: @SCROLLBAR_HANDLE@;
	min-height: @SCROLLBAR_MINHANDLE@px;
	border-radius: 4px;
}

QScrollBar::handle:vertical:hover {
	background: @SCROLLBAR_HANDLE_HOVER@;
}

QScrollBar:horizontal {
	background: @SCROLLBAR_BG@;
	height: @SCROLLBAR_SIZE@px;
	margin: @SCROLLBAR_MARGIN@px;
	border: none;
}

QScrollBar::handle:horizontal {
	background: @SCROLLBAR_HANDLE@;
	min-width: @SCROLLBAR_MINHANDLE@px;
	border-radius: 4px;
}

QScrollBar::handle:horizontal:hover {
	background: @SCROLLBAR_HANDLE_HOVER@;
}

QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical,
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
	background: transparent;
	border: none;
	width: 0px;
	height: 0px;
}

QSplitter::handle {
	background: @BORDER@;
	margin: 1px;
}

QSplitter::handle:hover {
	background: @ACCENT@;
}

QProgressBar {
	border: 1px solid @BORDER@;
	border-radius: 4px;
	background: @INPUT@;
	text-align: center;
}

QProgressBar::chunk {
	background: @ACCENT@;
}
)QSS" );

	qss.replace( "@TEXT@", qss_color( theme.text ) );
	qss.replace( "@TEXT_MUTED@", qss_color( theme.textMuted ) );
	qss.replace( "@DISABLED_TEXT@", qss_color( theme.textMuted ) );
	qss.replace( "@WINDOW@", qss_color( theme.window ) );
	qss.replace( "@WINDOW_ALT@", qss_color( theme.windowAlt ) );
	qss.replace( "@SURFACE@", qss_color( theme.surface ) );
	qss.replace( "@INPUT@", qss_color( theme.input ) );
	qss.replace( "@INPUT_ALT@", qss_color( theme.inputAlt ) );
	qss.replace( "@BORDER@", qss_color( theme.border ) );
	qss.replace( "@BORDER_STRONG@", qss_color( theme.borderStrong ) );
	qss.replace( "@ACCENT@", qss_color( theme.accent ) );
	qss.replace( "@ACCENT_HOVER@", qss_color( accentHover ) );
	qss.replace( "@ACCENT_PRESSED@", qss_color( accentPressed ) );
	qss.replace( "@ACCENT_SUBTLE@", qss_color( accentSubtle ) );
	qss.replace( "@ACCENT_SUBTLE_HOVER@", qss_color( accentSubtleHover ) );
	qss.replace( "@BUTTON@", qss_color( button ) );
	qss.replace( "@BUTTON_HOVER@", qss_color( buttonHover ) );
	qss.replace( "@BUTTON_PRESSED@", qss_color( buttonPressed ) );
	qss.replace( "@DISABLED_BG@", qss_color( disabledBg ) );
	qss.replace( "@HEADER_BG@", qss_color( headerBg ) );
	qss.replace( "@HOVER_ROW@", qss_color( hoverRow ) );
	qss.replace( "@SCROLLBAR_BG@", qss_color( scrollbarBg ) );
	qss.replace( "@SCROLLBAR_HANDLE@", qss_color( scrollbarHandle ) );
	qss.replace( "@SCROLLBAR_HANDLE_HOVER@", qss_color( scrollbarHover ) );
	qss.replace( "@SELECTION_TEXT@", qss_color( theme.selectionText ) );
	qss.replace( "@FONT_PT@", QString::number( fontPt ) );
	qss.replace( "@MENUBAR_PAD_V@", QString::number( menuBarPadV ) );
	qss.replace( "@MENUBAR_PAD_H@", QString::number( menuBarPadH ) );
	qss.replace( "@MENUITEM_PAD_V@", QString::number( menuItemPadV ) );
	qss.replace( "@MENUITEM_PAD_H@", QString::number( menuItemPadH ) );
	qss.replace( "@TOOLBTN_MIN@", QString::number( toolButtonMin ) );
	qss.replace( "@TOOLBTN_PAD@", QString::number( toolButtonPad ) );
	qss.replace( "@BUTTON_PAD_V@", QString::number( buttonPadV ) );
	qss.replace( "@BUTTON_PAD_H@", QString::number( buttonPadH ) );
	qss.replace( "@INPUT_PAD_V@", QString::number( inputPadV ) );
	qss.replace( "@INPUT_PAD_H@", QString::number( inputPadH ) );
	qss.replace( "@LISTITEM_PAD@", QString::number( listItemPad ) );
	qss.replace( "@HEADER_PAD_V@", QString::number( headerPadV ) );
	qss.replace( "@HEADER_PAD_H@", QString::number( headerPadH ) );
	qss.replace( "@TAB_PAD_V@", QString::number( tabPadV ) );
	qss.replace( "@TAB_PAD_H@", QString::number( tabPadH ) );
	qss.replace( "@DOCKTITLE_PAD_V@", QString::number( dockTitlePadV ) );
	qss.replace( "@DOCKTITLE_PAD_H@", QString::number( dockTitlePadH ) );
	qss.replace( "@GROUP_MARGIN_TOP@", QString::number( groupMarginTop ) );
	qss.replace( "@GROUP_PADDING_TOP@", QString::number( groupPaddingTop ) );
	qss.replace( "@CHECK_SPACING@", QString::number( checkSpacing ) );
	qss.replace( "@CHECK_INDICATOR@", QString::number( checkIndicator ) );
	qss.replace( "@SCROLLBAR_SIZE@", QString::number( scrollbarSize ) );
	qss.replace( "@SCROLLBAR_MARGIN@", QString::number( scrollbarMargin ) );
	qss.replace( "@SCROLLBAR_MINHANDLE@", QString::number( scrollbarMinHandle ) );
	return qss;
}

void theme_set( ETheme theme ){
	s_theme = theme;
	const ThemeVisuals visuals = visuals_with_overrides( theme );
	const ThemeDensity& density = density_for( s_density );

	static bool defaultFontInitialised = false;
	static QFont defaultFont;
	if( !defaultFontInitialised ){
		defaultFontInitialised = true;
		defaultFont = qApp->font();
	}
	QFont themedFont = defaultFont;
	const double baseFontPt = std::max( 7.0, themedFont.pointSizeF() );
	themedFont.setPointSizeF( std::max( 7.0, baseFontPt + density.fontDeltaPt ) );

	set_icon_theme( visuals );
	qApp->setFont( themedFont );
	qApp->setStyle( "Fusion" );
	qApp->setPalette( palette_for_theme( visuals ) );
	qApp->setStyleSheet( stylesheet_for_theme( visuals, density, themedFont ) );
	Colors_applyThemePreset( visuals.viewportPreset );
	if( s_accentOverrideEnabled ){
		Colors_applyAccentOverride( visuals.accent );
	}
	s_themeApplied = true;
}

void theme_construct_menu( class QMenu *menu ){
	auto *m = menu->addMenu( i18n::tr( "Unified Theme" ) );
	m->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
	auto *group = new QActionGroup( m );

	const std::array themeMenuEntries{
		std::pair{ ETheme::System, "System (OS)" },
		std::pair{ ETheme::Light, "Light" },
		std::pair{ ETheme::Dark, "Dark" },
		std::pair{ ETheme::Darker, "Darker" },
		std::pair{ ETheme::Blender, "Blender" },
	};

	for( const auto& [ theme, name ] : themeMenuEntries )
	{
		auto *a = m->addAction( name );
		a->setCheckable( true );
		a->setData( static_cast<int>( theme ) );
		group->addAction( a );
	}
	// init radio
	for( QAction* action : group->actions() )
	{
		if( action->data().toInt() == static_cast<int>( s_theme ) ){
			action->setChecked( true );
			break;
		}
	}

	QObject::connect( group, &QActionGroup::triggered, []( QAction *action ){
		theme_set( static_cast<ETheme>( action->data().toInt() ) );
	} );

	auto *densityMenu = menu->addMenu( i18n::tr( "Interface Density" ) );
	densityMenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
	auto *densityGroup = new QActionGroup( densityMenu );

	const std::array densityEntries{
		std::pair{ EInterfaceDensity::Compact, "Compact" },
		std::pair{ EInterfaceDensity::Standard, "Standard" },
		std::pair{ EInterfaceDensity::Comfortable, "Comfortable" },
	};
	for( const auto& [ density, label ] : densityEntries )
	{
		auto *a = densityMenu->addAction( i18n::tr( label ) );
		a->setCheckable( true );
		a->setData( static_cast<int>( density ) );
		densityGroup->addAction( a );
	}
	for( QAction* action : densityGroup->actions() )
	{
		if( action->data().toInt() == static_cast<int>( s_density ) ){
			action->setChecked( true );
			break;
		}
	}
	QObject::connect( densityGroup, &QActionGroup::triggered, []( QAction *action ){
		s_density = static_cast<EInterfaceDensity>( action->data().toInt() );
		theme_set( s_theme );
	} );

	menu->addSeparator();
	menu->addAction( i18n::tr( "Accent Color..." ), [](){
		const QColor fallback = visuals_for_theme( s_theme ).accent;
		const QColor initial = s_accentOverrideEnabled ? s_accentOverride : fallback;
		const QColor picked = QColorDialog::getColor( initial, MainFrame_getWindow(), i18n::tr( "Choose Accent Color" ) );
		if( picked.isValid() ){
			s_accentOverrideEnabled = true;
			s_accentOverride = picked;
			theme_set( s_theme );
		}
	} );
	menu->addAction( i18n::tr( "Use Theme Default Accent" ), [](){
		if( s_accentOverrideEnabled ){
			s_accentOverrideEnabled = false;
			theme_set( s_theme );
		}
	} );
}

void ThemeImport( int value ){
	switch( value )
	{
	case 0: s_theme = ETheme::System; break;
	case 1: s_theme = ETheme::Light; break; // migrate old Fusion slot to Light
	case 2: s_theme = ETheme::Dark; break;
	case 3: s_theme = ETheme::Darker; break;
	case 4: s_theme = ETheme::Blender; break;
	default: s_theme = ETheme::System; break;
	}
}
typedef FreeCaller<void(int), ThemeImport> ThemeImportCaller;

void ThemeExport( const IntImportCallback& importer ){
	importer( static_cast<int>( s_theme ) );
}
typedef FreeCaller<void(const IntImportCallback&), ThemeExport> ThemeExportCaller;

void ThemeDensityImport( int value ){
	switch( value )
	{
	case 0: s_density = EInterfaceDensity::Compact; break;
	case 1: s_density = EInterfaceDensity::Standard; break;
	case 2: s_density = EInterfaceDensity::Comfortable; break;
	default: s_density = EInterfaceDensity::Standard; break;
	}
}
typedef FreeCaller<void(int), ThemeDensityImport> ThemeDensityImportCaller;

void ThemeDensityExport( const IntImportCallback& importer ){
	importer( static_cast<int>( s_density ) );
}
typedef FreeCaller<void(const IntImportCallback&), ThemeDensityExport> ThemeDensityExportCaller;

void ThemeAccentEnabledImport( bool value ){
	s_accentOverrideEnabled = value;
}
typedef FreeCaller<void(bool), ThemeAccentEnabledImport> ThemeAccentEnabledImportCaller;

void ThemeAccentEnabledExport( const BoolImportCallback& importer ){
	importer( s_accentOverrideEnabled );
}
typedef FreeCaller<void(const BoolImportCallback&), ThemeAccentEnabledExport> ThemeAccentEnabledExportCaller;

void ThemeAccentColorImport( int value ){
	const int rgb = std::clamp( value, 0, 0xFFFFFF );
	s_accentOverride = QColor( ( rgb >> 16 ) & 0xFF, ( rgb >> 8 ) & 0xFF, rgb & 0xFF );
}
typedef FreeCaller<void(int), ThemeAccentColorImport> ThemeAccentColorImportCaller;

void ThemeAccentColorExport( const IntImportCallback& importer ){
	importer( ( s_accentOverride.red() << 16 ) | ( s_accentOverride.green() << 8 ) | s_accentOverride.blue() );
}
typedef FreeCaller<void(const IntImportCallback&), ThemeAccentColorExport> ThemeAccentColorExportCaller;


void theme_construct(){
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
	static bool connected = false;
	if( !connected ){
		connected = true;
		QObject::connect( QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, []( Qt::ColorScheme ){
			if( s_themeApplied && s_theme == ETheme::System ){
				theme_set( s_theme );
			}
		} );
	}
#endif

	set_icon_theme( visuals_with_overrides( s_theme ) );
}

void theme_apply_startup(){
	if( !s_themeApplied ){
		theme_set( s_theme );
	}
}

void theme_reapply_active(){
	theme_set( s_theme );
}

void theme_registerGlobalPreference( class PreferenceSystem& preferences ){
	preferences.registerPreference( "GUITheme", makeIntStringImportCallback( ThemeImportCaller() ), makeIntStringExportCallback( ThemeExportCaller() ) );
	preferences.registerPreference( "GUIThemeDensity", makeIntStringImportCallback( ThemeDensityImportCaller() ), makeIntStringExportCallback( ThemeDensityExportCaller() ) );
	preferences.registerPreference( "GUIAccentOverrideEnabled", makeBoolStringImportCallback( ThemeAccentEnabledImportCaller() ), makeBoolStringExportCallback( ThemeAccentEnabledExportCaller() ) );
	preferences.registerPreference( "GUIAccentColor", makeIntStringImportCallback( ThemeAccentColorImportCaller() ), makeIntStringExportCallback( ThemeAccentColorExportCaller() ) );
}
