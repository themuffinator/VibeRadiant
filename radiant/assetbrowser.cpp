#include "assetbrowser.h"

#include <QTabWidget>

#include "assetbrowserprefs.h"
#include "texwindow.h"
#include "entitybrowser.h"
#include "soundbrowser.h"
#include "modelwindow.h"

static QTabWidget* g_assetBrowserTabs = nullptr;
static int g_assetBrowserModelsTab = -1;

namespace {
constexpr bool kAssetBrowserEnabled = true;
Vector3 g_assetBrowserDefaultAngles( 0.0f, 40.0f, -60.0f );
}

bool AssetBrowser_isEnabled(){
	return kAssetBrowserEnabled;
}

Vector3& AssetBrowser_defaultAngles(){
	return g_assetBrowserDefaultAngles;
}

QWidget* AssetBrowser_constructWindow( QWidget* toplevel ){
	if ( !AssetBrowser_isEnabled() ) {
		g_assetBrowserTabs = nullptr;
		g_assetBrowserModelsTab = -1;
		return new QWidget( toplevel );
	}

	auto* tabs = new QTabWidget;
	g_assetBrowserTabs = tabs;
	g_assetBrowserModelsTab = -1;
	tabs->setTabPosition( QTabWidget::North );

	tabs->addTab( TextureBrowser_constructWindow( toplevel ), "Materials" );
	tabs->addTab( EntityBrowser_constructWindow( toplevel ), "Entities" );
	tabs->addTab( SoundBrowser_constructWindow( toplevel ), "Sounds" );
	g_assetBrowserModelsTab = tabs->addTab( ModelBrowser_constructWindow( toplevel ), "Models" );
	SoundBrowser_PrecacheWorldSounds();

	return tabs;
}

void AssetBrowser_destroyWindow(){
	g_assetBrowserTabs = nullptr;
	g_assetBrowserModelsTab = -1;
}

void AssetBrowser_selectModelsTab(){
	if ( g_assetBrowserTabs == nullptr || g_assetBrowserModelsTab < 0 ) {
		return;
	}
	g_assetBrowserTabs->setCurrentIndex( g_assetBrowserModelsTab );
}
