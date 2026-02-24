/*
   Copyright (C) 2026, VibeRadiant contributors.

   This file is part of VibeRadiant.
*/

#include "uvview.h"

#include "assetbrowser.h"
#include "commands.h"
#include "iselection.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstddef>

namespace
{
void run_command( const char* name ){
	GlobalCommands_find( name ).m_callback();
}

void run_toggle( const char* name ){
	GlobalToggles_find( name ).m_command.m_callback();
}

const char* manipulator_mode_name( SelectionSystem::EManipulatorMode mode ){
	switch ( mode )
	{
	case SelectionSystem::eTranslate:
		return "Translate";
	case SelectionSystem::eRotate:
		return "Rotate";
	case SelectionSystem::eScale:
		return "Scale";
	case SelectionSystem::eSkew:
		return "Transform";
	case SelectionSystem::eDrag:
		return "Drag";
	case SelectionSystem::eClip:
		return "Clipper";
	case SelectionSystem::eBuild:
		return "Build";
	case SelectionSystem::eUV:
		return "UV";
	}
	return "Unknown";
}

const char* component_mode_name( SelectionSystem::EComponentMode mode ){
	switch ( mode )
	{
	case SelectionSystem::eDefault:
		return "Default";
	case SelectionSystem::eVertex:
		return "Vertex";
	case SelectionSystem::eEdge:
		return "Edge";
	case SelectionSystem::eFace:
		return "Face";
	}
	return "Unknown";
}

const char* selection_mode_name( SelectionSystem::EMode mode ){
	switch ( mode )
	{
	case SelectionSystem::eEntity:
		return "Entity";
	case SelectionSystem::ePrimitive:
		return "Primitive";
	case SelectionSystem::eComponent:
		return "Component";
	}
	return "Unknown";
}

class UVViewPanel final : public QWidget
{
	QLabel* m_modeLabel = nullptr;
	QLabel* m_selectionLabel = nullptr;
	QLabel* m_hintLabel = nullptr;

	void refresh_status(){
		const SelectionSystem& selection = GlobalSelectionSystem();
		std::size_t brushes = 0;
		std::size_t patches = 0;
		std::size_t entities = 0;
		selection.countSelectedStuff( brushes, patches, entities );

		m_modeLabel->setText( QString( "Mode: %1 / %2 / %3" )
		                      .arg( selection_mode_name( selection.Mode() ) )
		                      .arg( component_mode_name( selection.ComponentMode() ) )
		                      .arg( manipulator_mode_name( selection.ManipulatorMode() ) ) );
		m_selectionLabel->setText( QString( "Selection: %1 brushes, %2 patches, %3 entities" )
		                           .arg( brushes )
		                           .arg( patches )
		                           .arg( entities ) );
	}

public:
	explicit UVViewPanel( QWidget* parent ) : QWidget( parent ){
		auto* mainLayout = new QVBoxLayout( this );
		mainLayout->setContentsMargins( 6, 6, 6, 6 );
		mainLayout->setSpacing( 8 );

		m_modeLabel = new QLabel( this );
		m_selectionLabel = new QLabel( this );
		m_hintLabel = new QLabel( "Dedicated UV workflow: switch to UV mode, then align with the controls below.", this );
		m_hintLabel->setWordWrap( true );
		mainLayout->addWidget( m_modeLabel );
		mainLayout->addWidget( m_selectionLabel );
		mainLayout->addWidget( m_hintLabel );

		{
			auto* group = new QGroupBox( "Workflow", this );
			auto* layout = new QHBoxLayout( group );

			auto* uvTool = new QPushButton( "UV Tool (G)", group );
			auto* translate = new QPushButton( "Translate (W)", group );
			auto* rotate = new QPushButton( "Rotate (R)", group );
			auto* scale = new QPushButton( "Scale", group );
			auto* surfaceInspector = new QPushButton( "Surface Inspector (S)", group );

			layout->addWidget( uvTool );
			layout->addWidget( translate );
			layout->addWidget( rotate );
			layout->addWidget( scale );
			layout->addWidget( surfaceInspector );

			QObject::connect( uvTool, &QPushButton::clicked, [this](){ run_toggle( "MouseUV" ); refresh_status(); } );
			QObject::connect( translate, &QPushButton::clicked, [this](){ run_toggle( "MouseTranslate" ); refresh_status(); } );
			QObject::connect( rotate, &QPushButton::clicked, [this](){ run_toggle( "MouseRotate" ); refresh_status(); } );
			QObject::connect( scale, &QPushButton::clicked, [this](){ run_toggle( "MouseScale" ); refresh_status(); } );
			QObject::connect( surfaceInspector, &QPushButton::clicked, [this](){ run_command( "SurfaceInspector" ); refresh_status(); } );

			mainLayout->addWidget( group );
		}

		{
			auto* group = new QGroupBox( "Projection / Fit", this );
			auto* layout = new QHBoxLayout( group );

			auto* fit = new QPushButton( "Fit", group );
			auto* fitW = new QPushButton( "Fit Width", group );
			auto* fitH = new QPushButton( "Fit Height", group );
			auto* axial = new QPushButton( "Project Axial", group );

			layout->addWidget( fit );
			layout->addWidget( fitW );
			layout->addWidget( fitH );
			layout->addWidget( axial );

			QObject::connect( fit, &QPushButton::clicked, [](){ run_command( "FitTexture" ); } );
			QObject::connect( fitW, &QPushButton::clicked, [](){ run_command( "FitTextureWidth" ); } );
			QObject::connect( fitH, &QPushButton::clicked, [](){ run_command( "FitTextureHeight" ); } );
			QObject::connect( axial, &QPushButton::clicked, [](){ run_command( "TextureProjectAxial" ); } );

			mainLayout->addWidget( group );
		}

		{
			auto* group = new QGroupBox( "UV Nudge", this );
			auto* grid = new QGridLayout( group );

			auto* shiftLeft = new QPushButton( "Shift U-", group );
			auto* shiftRight = new QPushButton( "Shift U+", group );
			auto* shiftUp = new QPushButton( "Shift V+", group );
			auto* shiftDown = new QPushButton( "Shift V-", group );
			auto* scaleLeft = new QPushButton( "Scale U-", group );
			auto* scaleRight = new QPushButton( "Scale U+", group );
			auto* scaleUp = new QPushButton( "Scale V+", group );
			auto* scaleDown = new QPushButton( "Scale V-", group );

			grid->addWidget( shiftLeft, 0, 0 );
			grid->addWidget( shiftRight, 0, 1 );
			grid->addWidget( shiftUp, 0, 2 );
			grid->addWidget( shiftDown, 0, 3 );
			grid->addWidget( scaleLeft, 1, 0 );
			grid->addWidget( scaleRight, 1, 1 );
			grid->addWidget( scaleUp, 1, 2 );
			grid->addWidget( scaleDown, 1, 3 );

			QObject::connect( shiftLeft, &QPushButton::clicked, [](){ run_command( "TexShiftLeft" ); } );
			QObject::connect( shiftRight, &QPushButton::clicked, [](){ run_command( "TexShiftRight" ); } );
			QObject::connect( shiftUp, &QPushButton::clicked, [](){ run_command( "TexShiftUp" ); } );
			QObject::connect( shiftDown, &QPushButton::clicked, [](){ run_command( "TexShiftDown" ); } );
			QObject::connect( scaleLeft, &QPushButton::clicked, [](){ run_command( "TexScaleLeft" ); } );
			QObject::connect( scaleRight, &QPushButton::clicked, [](){ run_command( "TexScaleRight" ); } );
			QObject::connect( scaleUp, &QPushButton::clicked, [](){ run_command( "TexScaleUp" ); } );
			QObject::connect( scaleDown, &QPushButton::clicked, [](){ run_command( "TexScaleDown" ); } );

			mainLayout->addWidget( group );
		}

		if( AssetBrowser_isEnabled() ){
			auto* openAssetBrowser = new QPushButton( "Open Asset Browser", this );
			QObject::connect( openAssetBrowser, &QPushButton::clicked, [](){ run_command( "ToggleTextures" ); } );
			mainLayout->addWidget( openAssetBrowser );
		}

		auto* timer = new QTimer( this );
		timer->setInterval( 250 );
		QObject::connect( timer, &QTimer::timeout, this, [this](){ refresh_status(); } );
		timer->start();

		refresh_status();
	}
};

UVViewPanel* g_uvViewPanel = nullptr;
}

QWidget* UVView_constructWindow( QWidget* toplevel ){
	if( g_uvViewPanel == nullptr ){
		g_uvViewPanel = new UVViewPanel( toplevel );
		QObject::connect( g_uvViewPanel, &QObject::destroyed, []( QObject* ){
			g_uvViewPanel = nullptr;
		} );
	}
	return g_uvViewPanel;
}

void UVView_destroyWindow(){
	g_uvViewPanel = nullptr;
}
