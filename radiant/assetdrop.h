#pragma once

#include "generic/vector.h"

class Entity;

extern const char* const kEntityBrowserMimeType;
extern const char* const kSoundBrowserMimeType;
extern const char* const kTextureBrowserMimeType;
extern const char* const kModelBrowserMimeType;

bool AssetDrop_handleEntityClass( const char* classname, const Vector3& point );
bool AssetDrop_handleSoundPath( const char* soundPath, const Vector3& point );
bool AssetDrop_handleTexture( const char* shader, const Vector3& point );
bool AssetDrop_handleModelPath( const char* modelPath, const Vector3& point );
