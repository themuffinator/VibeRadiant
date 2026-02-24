/*
   Copyright (C) 2026
*/

#pragma once

class QWidget;

enum class UpdateCheckMode
{
	Automatic,
	Manual
};

void UpdateManager_Construct();
void UpdateManager_Destroy();
void UpdateManager_MaybeAutoCheck();
void UpdateManager_CheckForUpdates( UpdateCheckMode mode );
void UpdateManager_CheckForUpdatesBlocking( UpdateCheckMode mode, QWidget* parent_override = nullptr );
bool UpdateManager_QuitRequested();
