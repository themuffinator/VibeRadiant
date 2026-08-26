/*
   Copyright (C) 2026
*/

#pragma once

enum class UpdateCheckMode
{
	Automatic,
	Manual
};

void UpdateManager_Construct();
void UpdateManager_Destroy();
void UpdateManager_MaybeAutoCheck();
void UpdateManager_CheckForUpdates( UpdateCheckMode mode );
