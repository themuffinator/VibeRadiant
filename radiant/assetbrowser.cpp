#include "assetbrowser.h"

#include <QTabWidget>

#include "texwindow.h"
#include "entitybrowser.h"
#include "soundbrowser.h"

static QTabWidget* g_assetBrowserTabs = nullptr;

namespace {
constexpr bool kAssetBrowserEnabled = true;
}

bool AssetBrowser_isEnabled(){
	return kAssetBrowserEnabled;
}

QWidget* AssetBrowser_constructWindow( QWidget* toplevel ){
	if ( !AssetBrowser_isEnabled() ) {
		g_assetBrowserTabs = nullptr;
		return new QWidget( toplevel );
	}

	auto* tabs = new QTabWidget;
	g_assetBrowserTabs = tabs;
	tabs->setTabPosition( QTabWidget::North );

	tabs->addTab( TextureBrowser_constructWindow( toplevel ), "Textures" );
	tabs->addTab( EntityBrowser_constructWindow( toplevel ), "Entities" );
	tabs->addTab( SoundBrowser_constructWindow( toplevel ), "Sounds" );

	return tabs;
}

void AssetBrowser_destroyWindow(){
	g_assetBrowserTabs = nullptr;
}
