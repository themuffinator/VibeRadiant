#pragma once

class QWidget;

QWidget* AssetBrowser_constructWindow( QWidget* toplevel );
void AssetBrowser_destroyWindow();
bool AssetBrowser_isEnabled();
void AssetBrowser_Construct();
void AssetBrowser_Destroy();
void AssetBrowser_selectModelsTab();
