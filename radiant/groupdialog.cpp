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
// Floating dialog that contains a notebook with at least Entities and Group tabs
// I merged the 2 MS Windows dialogs in a single class
//
// Leonardo Zide (leo@lokigames.com)
//

#include "groupdialog.h"

#include "debugging/debugging.h"

#include <vector>

#include <QWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QEasingCurve>

#include "gtkutil/guisettings.h"

class GroupDlg
{
public:
	QTabWidget* m_pNotebook;
	QWidget* m_window;

	GroupDlg();
	void Create( QWidget* parent );

	void Show(){
		m_window->show();
		m_window->raise();
		m_window->activateWindow();
	}
	void Hide(){
		m_window->hide();
	}
};

namespace
{
GroupDlg g_GroupDlg;

std::vector<StringExportCallback> g_pages;

void animate_group_dialog_visibility( bool shown ){
	QWidget* window = g_GroupDlg.m_window;
	if( window == nullptr ){
		return;
	}

	if( shown ){
		const QPoint target = window->pos();
		if( !window->isVisible() ){
			window->setWindowOpacity( 0.0 );
			window->move( target + QPoint( 0, 10 ) );
			g_GroupDlg.Show();
		}
		else{
			window->raise();
			window->activateWindow();
		}

		auto *group = new QParallelAnimationGroup( window );
		auto *fade = new QPropertyAnimation( window, "windowOpacity", group );
		fade->setDuration( 150 );
		fade->setStartValue( window->windowOpacity() );
		fade->setEndValue( 1.0 );
		fade->setEasingCurve( QEasingCurve::OutCubic );
		group->addAnimation( fade );

		auto *slide = new QPropertyAnimation( window, "pos", group );
		slide->setDuration( 150 );
		slide->setStartValue( window->pos() );
		slide->setEndValue( target );
		slide->setEasingCurve( QEasingCurve::OutCubic );
		group->addAnimation( slide );

		group->start( QAbstractAnimation::DeleteWhenStopped );
	}
	else
	{
		if( !window->isVisible() ){
			return;
		}
		auto *fade = new QPropertyAnimation( window, "windowOpacity", window );
		fade->setDuration( 110 );
		fade->setStartValue( window->windowOpacity() );
		fade->setEndValue( 0.0 );
		fade->setEasingCurve( QEasingCurve::OutCubic );
		QObject::connect( fade, &QPropertyAnimation::finished, [window](){
			window->hide();
			window->setWindowOpacity( 1.0 );
		} );
		fade->start( QAbstractAnimation::DeleteWhenStopped );
	}
}
}

void GroupDialog_updatePageTitle( QWidget* window, int pageIndex ){
	if ( pageIndex >= 0 && pageIndex < static_cast<int>( g_pages.size() ) ) {
		const auto la = [window]( const char *title ){ window->setWindowTitle( title ); };
		g_pages[pageIndex]( ConstMemberCaller<decltype( la ), void(const char*), &decltype( la )::operator()>( la ) );
	}
}

GroupDlg::GroupDlg() : m_window( 0 ){
}

void GroupDlg::Create( QWidget* parent ){
	ASSERT_MESSAGE( m_window == 0, "dialog already created" );

	m_window = new QWidget( parent, Qt::Dialog | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
	m_window->setWindowTitle( "Entities" );

//.	window_connect_focus_in_clear_focus_widget( m_window );

	g_guiSettings.addWindow( m_window, "GroupDlg/geometry", 444, 777 );

	{
		auto *box = new QVBoxLayout( m_window );
		box->setContentsMargins( 0, 0, 0, 0 );
		m_pNotebook = new QTabWidget;
		m_pNotebook->setTabPosition( QTabWidget::TabPosition::South );
		m_pNotebook->setFocusPolicy( Qt::FocusPolicy::NoFocus );
		box->addWidget( m_pNotebook );

		QObject::connect( m_pNotebook, &QTabWidget::currentChanged, [window = m_window]( int index ){
			GroupDialog_updatePageTitle( window, index );
		} );
	}
}


QWidget* GroupDialog_addPage( const char* tabLabel, QWidget* widget, const StringExportCallback& title ){
	g_GroupDlg.m_pNotebook->addTab( widget, tabLabel );
	g_pages.push_back( title );
	return widget;
}


bool GroupDialog_isShown(){
	return g_GroupDlg.m_window->isVisible();
}
void GroupDialog_setShown( bool shown ){
	animate_group_dialog_visibility( shown );
}
void GroupDialog_ToggleShow(){
	GroupDialog_setShown( !GroupDialog_isShown() );
}

void GroupDialog_constructWindow( QWidget* main_window ){
	g_GroupDlg.Create( main_window );
}
void GroupDialog_destroyWindow(){
	ASSERT_NOTNULL( g_GroupDlg.m_window );
	delete g_GroupDlg.m_window;
	g_GroupDlg.m_window = 0;
}


QWidget* GroupDialog_getWindow(){
	return g_GroupDlg.m_window;
}
void GroupDialog_show(){
	g_GroupDlg.Show();
}

QWidget* GroupDialog_getPage(){
	return g_GroupDlg.m_pNotebook->currentWidget();
}

void GroupDialog_presentPage( QWidget* page ){
	if( page == nullptr ){
		return;
	}

	g_GroupDlg.m_pNotebook->setCurrentWidget( page );
	GroupDialog_setShown( true );
}

void GroupDialog_showPage( QWidget* page ){
	if ( GroupDialog_getPage() == page && GroupDialog_isShown() ) {
		GroupDialog_setShown( false );
	}
	else
	{
		GroupDialog_presentPage( page );
	}
}

void GroupDialog_updatePageTitle( QWidget* page ){
	if ( GroupDialog_getPage() == page ) {
		GroupDialog_updatePageTitle( g_GroupDlg.m_window, g_GroupDlg.m_pNotebook->currentIndex() );
	}
}


#include "preferencesystem.h"

void GroupDialog_Construct(){
}
void GroupDialog_Destroy(){
}
