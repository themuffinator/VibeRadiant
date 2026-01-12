#pragma once

class QWidget;

void EntityBrowser_Construct();
void EntityBrowser_Destroy();

QWidget* EntityBrowser_constructWindow( QWidget* toplevel );
void EntityBrowser_destroyWindow();
void EntityBrowser_flushReferences();
