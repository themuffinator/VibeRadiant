#pragma once

class QWidget;

void SoundBrowser_Construct();
void SoundBrowser_Destroy();

QWidget* SoundBrowser_constructWindow( QWidget* toplevel );
void SoundBrowser_destroyWindow();

void SoundBrowser_PrecacheWorldSounds();
void SoundBrowser_ReloadSounds();
