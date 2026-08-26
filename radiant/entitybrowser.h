#pragma once

class QWidget;

void EntityBrowser_Construct();
void EntityBrowser_Destroy();

QWidget* EntityBrowser_constructWindow( QWidget* toplevel );
void EntityBrowser_destroyWindow();
void EntityBrowser_EnsureTree();
void EntityBrowser_flushReferences();
void EntityBrowser_mapFree();
void EntityBrowser_mapReady();
void EntityBrowser_prepareForEntityClassReload();
void EntityBrowser_entityClassesReady();
void EntityBrowser_prepareForModuleReload();
void EntityBrowser_dependenciesReady();
void EntityBrowser_beginReferenceRefresh();
void EntityBrowser_endReferenceRefresh();
bool EntityBrowser_canRefreshReferences();
bool EntityBrowser_canRestart();
void EntityBrowser_resumeDeferredWork();
