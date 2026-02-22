/*
   Copyright (C) 2026

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 */

#pragma once

enum PreviewLightingModel
{
	PREVIEW_LIGHTING_MODEL_BAKED_OVERLAY = 0,
	PREVIEW_LIGHTING_MODEL_FAST_INTERACTION = 1,
	// Legacy alias kept for compatibility with older references.
	PREVIEW_LIGHTING_MODEL_STENCIL_SHADOWS = PREVIEW_LIGHTING_MODEL_FAST_INTERACTION,
	PREVIEW_LIGHTING_MODEL_COUNT
};

void PreviewLighting_SetModel( int model );
int PreviewLighting_GetModel();

void PreviewLighting_Enable( bool enable );
void PreviewLighting_UpdateIfNeeded();
void PreviewLighting_RenderOverlay();
