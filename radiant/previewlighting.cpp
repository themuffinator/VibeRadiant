/*
   Copyright (C) 2026

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 */

#include "previewlighting.h"

#include "preferences.h"

#include "iscenegraph.h"
#include "ientity.h"
#include "irender.h"
#include "ishaders.h"
#include "ifilesystem.h"
#include "iarchive.h"
#include "idatastream.h"
#include "iscriplib.h"
#include "signal/isignal.h"

#include "string/string.h"
#include "stringio.h"
#include "scenelib.h"

#include "brush.h"
#include "patch.h"

#include "math/aabb.h"
#include "math/matrix.h"
#include "math/pi.h"
#include "math/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace
{
struct SunInfo
{
	Vector3 colour;
	Vector3 direction;
	float intensity;
};

struct ShaderLightInfo
{
	bool parsed = false;
	bool hasSurfaceLight = false;
	float surfaceLight = 0.0f;
	bool hasSurfaceLightColor = false;
	Vector3 surfaceLightColor = Vector3( 1, 1, 1 );
	std::vector<SunInfo> suns;
};

class PreviewLight : public RendererLight
{
	AABB m_aabb;
	Vector3 m_colour;
	Vector3 m_offset;
	Matrix4 m_rotation;
	Matrix4 m_projection;
	bool m_projected;
public:
	PreviewLight( const AABB& aabb, const Vector3& colour )
		: m_aabb( aabb ),
		  m_colour( colour ),
		  m_offset( 0, 0, 0 ),
		  m_rotation( g_matrix4_identity ),
		  m_projection( g_matrix4_identity ),
		  m_projected( false ){
	}

	Shader* getShader() const override {
		return nullptr;
	}
	const AABB& aabb() const override {
		return m_aabb;
	}
	bool testAABB( const AABB& other ) const override {
		return aabb_intersects_aabb( m_aabb, other );
	}
	const Matrix4& rotation() const override {
		return m_rotation;
	}
	const Vector3& offset() const override {
		return m_offset;
	}
	const Vector3& colour() const override {
		return m_colour;
	}
	bool isProjected() const override {
		return m_projected;
	}
	const Matrix4& projection() const override {
		return m_projection;
	}
};

struct PreviewLightingState
{
	bool active = false;
	bool dirty = true;
	bool callbackRegistered = false;
	std::vector<std::unique_ptr<PreviewLight>> lights;
	std::map<CopiedString, ShaderLightInfo> shaderCache;
};

PreviewLightingState g_previewLighting;

const float c_pointScale = 7500.0f;
const float c_linearScale = 1.0f / 8000.0f;

inline float light_radius_linear( float intensity, float falloffTolerance ){
	return ( intensity * c_pointScale * c_linearScale ) - falloffTolerance;
}

inline float light_radius( float intensity, float falloffTolerance ){
	return std::sqrt( intensity * c_pointScale / falloffTolerance );
}

bool game_is_doom3(){
	return g_pGameDescription->mGameType == "doom3";
}

bool key_bool( const char* value ){
	if ( string_empty( value ) ) {
		return false;
	}
	if ( string_equal_nocase( value, "true" ) || string_equal_nocase( value, "yes" ) ) {
		return true;
	}
	int asInt = 0;
	if ( string_parse_int( value, asInt ) ) {
		return asInt != 0;
	}
	return false;
}

bool parse_float_key( const Entity& entity, const char* key, float& out ){
	const char* value = entity.getKeyValue( key );
	return !string_empty( value ) && string_parse_float( value, out );
}

bool parse_vec3_key( const Entity& entity, const char* key, Vector3& out ){
	const char* value = entity.getKeyValue( key );
	return !string_empty( value ) && string_parse_vector3( value, out );
}

bool parse_int_key( const Entity& entity, const char* key, int& out ){
	const char* value = entity.getKeyValue( key );
	return !string_empty( value ) && string_parse_int( value, out );
}

bool parse_yaw_pitch( const char* value, float& yaw, float& pitch ){
	if ( string_empty( value ) ) {
		return false;
	}
	float a = 0.0f;
	float b = 0.0f;
	float c = 0.0f;
	const int count = std::sscanf( value, "%f %f %f", &a, &b, &c );
	if ( count >= 2 ) {
		yaw = a;
		pitch = b;
		return true;
	}
	return false;
}

bool parse_entity_angles( const Entity& entity, float& yaw, float& pitch ){
	Vector3 angles( 0, 0, 0 );
	bool hasAngles = false;
	if ( parse_vec3_key( entity, "angles", angles ) ) {
		yaw = angles.y();
		pitch = angles.x();
		hasAngles = true;
	}

	float value = 0.0f;
	if ( parse_float_key( entity, "angle", value ) ) {
		yaw = value;
		hasAngles = true;
	}
	if ( parse_float_key( entity, "pitch", value ) ) {
		pitch = value;
		hasAngles = true;
	}

	if ( !hasAngles ) {
		return parse_yaw_pitch( entity.getKeyValue( "angles" ), yaw, pitch );
	}

	return true;
}

Vector3 normalize_colour( Vector3 colour ){
	if ( colour.x() > 1.0f || colour.y() > 1.0f || colour.z() > 1.0f ) {
		colour /= 255.0f;
	}
	const float maxComponent = vector3_max_component( colour );
	if ( maxComponent > 1.0f ) {
		colour /= maxComponent;
	}
	return colour;
}

Vector3 scaled_colour( const Vector3& colour, float intensity, float reference ){
	if ( reference <= 0.0f ) {
		return colour;
	}
	const float scale = intensity / reference;
	return colour * scale;
}

float clamped_area_scale( float area ){
	const float scale = std::sqrt( std::max( area, 0.0f ) ) / 128.0f;
	return std::clamp( scale, 0.25f, 4.0f );
}

void preview_lighting_clear(){
	for ( const auto& light : g_previewLighting.lights )
	{
		GlobalShaderCache().detach( *light );
	}
	g_previewLighting.lights.clear();
}

void preview_lighting_add( const AABB& aabb, const Vector3& colour ){
	auto light = std::make_unique<PreviewLight>( aabb, colour );
	GlobalShaderCache().attach( *light );
	g_previewLighting.lights.push_back( std::move( light ) );
}

bool spawnflags_linear( int flags ){
	if ( g_pGameDescription->mGameType == "wolf" ) {
		return ( flags & 1 ) == 0;
	}
	return ( flags & 1 ) != 0;
}

bool parse_light_key( const Entity& entity, Vector3& colour, bool& colourFromKey, float& intensity ){
	const char* value = entity.getKeyValue( "_light" );
	if ( string_empty( value ) ) {
		return false;
	}

	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float i = 0.0f;
	const int count = std::sscanf( value, "%f %f %f %f", &r, &g, &b, &i );
	if ( count >= 3 ) {
		colour = Vector3( r, g, b );
		colourFromKey = true;
	}
	if ( count >= 4 ) {
		intensity = i;
		return true;
	}
	if ( count == 1 ) {
		intensity = r;
		return true;
	}
	return false;
}

bool parse_light_intensity( const Entity& entity, Vector3& colour, bool& colourFromKey, float& intensity ){
	if ( parse_light_key( entity, colour, colourFromKey, intensity ) ) {
		return true;
	}
	if ( parse_float_key( entity, "_light", intensity ) ) {
		return true;
	}
	return parse_float_key( entity, "light", intensity );
}

bool parse_light_radius( const Entity& entity, Vector3& radius ){
	return parse_vec3_key( entity, "light_radius", radius );
}

bool parse_sun_direction( const Entity& worldspawn, const std::map<CopiedString, Vector3>& targets, const Vector3& mapCenter, Vector3& direction ){
	Vector3 value( 0, 0, 0 );
	if ( parse_vec3_key( worldspawn, "_sun_vector", value )
	  || parse_vec3_key( worldspawn, "sun_vector", value )
	  || parse_vec3_key( worldspawn, "sunlight_vector", value )
	  || parse_vec3_key( worldspawn, "sunlight_dir", value ) ) {
		direction = value;
		return true;
	}

	if ( parse_vec3_key( worldspawn, "_sunlight_mangle", value )
	  || parse_vec3_key( worldspawn, "sunlight_mangle", value )
	  || parse_vec3_key( worldspawn, "_sun_mangle", value )
	  || parse_vec3_key( worldspawn, "sun_mangle", value ) ) {
		const float yaw = value.y();
		const float pitch = value.x();
		direction = vector3_for_spherical( degrees_to_radians( yaw ), degrees_to_radians( pitch ) );
		return true;
	}

	float yaw = 0.0f;
	float pitch = 0.0f;
	if ( parse_yaw_pitch( worldspawn.getKeyValue( "_sun_angle" ), yaw, pitch )
	  || parse_yaw_pitch( worldspawn.getKeyValue( "sun_angle" ), yaw, pitch )
	  || parse_yaw_pitch( worldspawn.getKeyValue( "sunlight_angle" ), yaw, pitch ) ) {
		direction = vector3_for_spherical( degrees_to_radians( yaw ), degrees_to_radians( pitch ) );
		return true;
	}

	const char* targetName = worldspawn.getKeyValue( "_sun_target" );
	if ( string_empty( targetName ) ) {
		targetName = worldspawn.getKeyValue( "sun_target" );
	}
	if ( !string_empty( targetName ) ) {
		auto it = targets.find( targetName );
		if ( it != targets.end() ) {
			direction = it->second - mapCenter;
			return true;
		}
	}

	return false;
}

bool parse_worldspawn_sun( const Entity& worldspawn, const std::map<CopiedString, Vector3>& targets, const Vector3& mapCenter, SunInfo& sun ){
	{
		const char* value = worldspawn.getKeyValue( "_sun" );
		if ( string_empty( value ) ) {
			value = worldspawn.getKeyValue( "sun" );
		}
		if ( !string_empty( value ) ) {
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			float intensity = 0.0f;
			float degrees = 0.0f;
			float elevation = 0.0f;
			if ( std::sscanf( value, "%f %f %f %f %f %f", &r, &g, &b, &intensity, &degrees, &elevation ) == 6 ) {
				sun.colour = normalize_colour( Vector3( r, g, b ) );
				sun.intensity = intensity;
				sun.direction = vector3_for_spherical( degrees_to_radians( degrees ), degrees_to_radians( elevation ) );
				return true;
			}
		}
	}

	float intensity = 0.0f;
	if ( !parse_float_key( worldspawn, "_sunlight", intensity )
	  && !parse_float_key( worldspawn, "sunlight", intensity )
	  && !parse_float_key( worldspawn, "_sun_light", intensity )
	  && !parse_float_key( worldspawn, "sun_light", intensity ) ) {
		return false;
	}

	Vector3 colour( 1, 1, 1 );
	Vector3 colourKey( 1, 1, 1 );
	if ( parse_vec3_key( worldspawn, "_sunlight_color", colourKey )
	  || parse_vec3_key( worldspawn, "sunlight_color", colourKey )
	  || parse_vec3_key( worldspawn, "_sun_color", colourKey )
	  || parse_vec3_key( worldspawn, "sun_color", colourKey ) ) {
		colour = colourKey;
	}

	Vector3 direction( 0, 0, 1 );
	parse_sun_direction( worldspawn, targets, mapCenter, direction );
	if ( vector3_length( direction ) == 0.0f ) {
		direction = Vector3( 0, 0, 1 );
	}
	direction = vector3_normalised( direction );

	sun.colour = normalize_colour( colour );
	sun.direction = direction;
	sun.intensity = intensity;
	return true;
}

void parse_shader_light_info( const char* shaderName, ShaderLightInfo& info ){
	info.parsed = true;

	IShader* shader = QERApp_Shader_ForName( shaderName );
	if ( shader == nullptr || shader->IsDefault() ) {
		return;
	}

	const char* shaderFile = shader->getShaderFileName();
	if ( string_empty( shaderFile ) ) {
		return;
	}

	ArchiveTextFile* file = GlobalFileSystem().openTextFile( shaderFile );
	if ( file == nullptr ) {
		return;
	}

	Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewScriptTokeniser( file->getInputStream() );
	bool inBlock = false;
	int depth = 0;

	while ( const char* token = tokeniser.getToken() )
	{
		if ( !inBlock ) {
			if ( string_equal_nocase( token, shaderName ) ) {
				const char* brace = tokeniser.getToken();
				if ( brace != nullptr && string_equal( brace, "{" ) ) {
					inBlock = true;
					depth = 1;
				}
			}
			continue;
		}

		if ( string_equal( token, "{" ) ) {
			++depth;
			continue;
		}
		if ( string_equal( token, "}" ) ) {
			if ( --depth == 0 ) {
				break;
			}
			continue;
		}

		if ( string_equal_nocase( token, "q3map_surfacelight" ) || string_equal_nocase( token, "q3map_surfaceLight" ) ) {
			float value = 0.0f;
			if ( Tokeniser_getFloat( tokeniser, value ) ) {
				info.hasSurfaceLight = true;
				info.surfaceLight = value;
			}
			continue;
		}

		if ( string_equal_nocase( token, "q3map_lightRGB" ) ) {
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			if ( Tokeniser_getFloat( tokeniser, r )
			  && Tokeniser_getFloat( tokeniser, g )
			  && Tokeniser_getFloat( tokeniser, b ) ) {
				info.hasSurfaceLightColor = true;
				info.surfaceLightColor = normalize_colour( Vector3( r, g, b ) );
			}
			continue;
		}

		if ( string_equal_nocase( token, "sun" )
		  || string_equal_nocase( token, "q3map_sun" )
		  || string_equal_nocase( token, "q3map_sunExt" ) ) {
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			float intensity = 0.0f;
			float degrees = 0.0f;
			float elevation = 0.0f;
			if ( Tokeniser_getFloat( tokeniser, r )
			  && Tokeniser_getFloat( tokeniser, g )
			  && Tokeniser_getFloat( tokeniser, b )
			  && Tokeniser_getFloat( tokeniser, intensity )
			  && Tokeniser_getFloat( tokeniser, degrees )
			  && Tokeniser_getFloat( tokeniser, elevation ) ) {
				SunInfo sun;
				sun.colour = normalize_colour( Vector3( r, g, b ) );
				sun.intensity = intensity;
				sun.direction = vector3_for_spherical( degrees_to_radians( degrees ), degrees_to_radians( elevation ) );
				info.suns.push_back( sun );
			}
			continue;
		}
	}

	tokeniser.release();
	file->release();
}

ShaderLightInfo& shader_light_info( const char* shaderName ){
	auto it = g_previewLighting.shaderCache.find( shaderName );
	if ( it == g_previewLighting.shaderCache.end() ) {
		it = g_previewLighting.shaderCache.emplace( CopiedString( shaderName ), ShaderLightInfo() ).first;
	}
	if ( !it->second.parsed ) {
		parse_shader_light_info( shaderName, it->second );
	}
	return it->second;
}

void accumulate_triangle( const Vector3& a, const Vector3& b, const Vector3& c, float& area, Vector3& centroid ){
	const Vector3 cross = vector3_cross( b - a, c - a );
	const float triArea = 0.5f * vector3_length( cross );
	if ( triArea <= 0.0f ) {
		return;
	}
	centroid += ( a + b + c ) * ( triArea / 3.0f );
	area += triArea;
}

bool winding_area_centroid( const Winding& winding, const Matrix4& localToWorld, float& area, Vector3& centroid ){
	if ( winding.numpoints < 3 ) {
		return false;
	}
	Vector3 v0 = matrix4_transformed_point( localToWorld, Vector3( winding[0].vertex ) );
	for ( std::size_t i = 1; i + 1 < winding.numpoints; ++i )
	{
		const Vector3 v1 = matrix4_transformed_point( localToWorld, Vector3( winding[i].vertex ) );
		const Vector3 v2 = matrix4_transformed_point( localToWorld, Vector3( winding[i + 1].vertex ) );
		accumulate_triangle( v0, v1, v2, area, centroid );
	}
	return area > 0.0f;
}

bool patch_area_centroid( const PatchTesselation& tess, const Matrix4& localToWorld, float& area, Vector3& centroid ){
	if ( tess.m_numStrips == 0 || tess.m_lenStrips < 4 ) {
		return false;
	}

	const RenderIndex* strip = tess.m_indices.data();
	for ( std::size_t s = 0; s < tess.m_numStrips; ++s, strip += tess.m_lenStrips )
	{
		for ( std::size_t i = 0; i + 3 < tess.m_lenStrips; i += 2 )
		{
			const RenderIndex i0 = strip[i];
			const RenderIndex i1 = strip[i + 1];
			const RenderIndex i2 = strip[i + 2];
			const RenderIndex i3 = strip[i + 3];

			const Vector3 v0 = matrix4_transformed_point( localToWorld, tess.m_vertices[i0].vertex );
			const Vector3 v1 = matrix4_transformed_point( localToWorld, tess.m_vertices[i1].vertex );
			const Vector3 v2 = matrix4_transformed_point( localToWorld, tess.m_vertices[i2].vertex );
			const Vector3 v3 = matrix4_transformed_point( localToWorld, tess.m_vertices[i3].vertex );

			accumulate_triangle( v0, v1, v2, area, centroid );
			accumulate_triangle( v2, v1, v3, area, centroid );
		}
	}

	return area > 0.0f;
}

void add_sun_lights( const std::vector<SunInfo>& suns, const AABB& mapBounds, bool hasBounds, float reference ){
	Vector3 center( 0, 0, 0 );
	Vector3 extents( 2048, 2048, 2048 );
	if ( hasBounds ) {
		center = mapBounds.origin;
		const float maxExtent = std::max( mapBounds.extents.x(), std::max( mapBounds.extents.y(), mapBounds.extents.z() ) );
		const float distance = std::max( maxExtent * 2.0f, 2048.0f );
		extents = Vector3( distance, distance, distance );
		for ( const auto& sun : suns )
		{
			const Vector3 origin = center - sun.direction * distance;
			const Vector3 colour = scaled_colour( sun.colour, sun.intensity, reference );
			preview_lighting_add( AABB( origin, extents ), colour );
		}
		return;
	}

	for ( const auto& sun : suns )
	{
		const Vector3 origin = center - sun.direction * extents.x();
		const Vector3 colour = scaled_colour( sun.colour, sun.intensity, reference );
		preview_lighting_add( AABB( origin, extents ), colour );
	}
}

void preview_lighting_rebuild(){
	preview_lighting_clear();
	g_previewLighting.shaderCache.clear();

	if ( game_is_doom3() ) {
		return;
	}

	std::map<CopiedString, Vector3> targets;
	Entity* worldspawn = nullptr;

	Scene_forEachEntity( [&]( scene::Instance& instance ){
		Entity* entity = Node_getEntity( instance.path().top() );
		if ( entity == nullptr ) {
			return;
		}
		if ( string_equal_nocase( entity->getClassName(), "worldspawn" ) ) {
			worldspawn = entity;
		}

		const char* targetname = entity->getKeyValue( "targetname" );
		if ( !string_empty( targetname ) ) {
			Vector3 origin( 0, 0, 0 );
			if ( !parse_vec3_key( *entity, "origin", origin ) ) {
				origin = instance.worldAABB().origin;
			}
			targets.emplace( CopiedString( targetname ), origin );
		}
	} );

	AABB mapBounds;
	bool hasBounds = false;
	auto add_bounds = [&]( const AABB& aabb ){
		if ( !hasBounds ) {
			mapBounds = aabb;
			hasBounds = true;
		}
		else
		{
			aabb_extend_by_aabb_safe( mapBounds, aabb );
		}
	};

	std::vector<SunInfo> worldSuns;
	std::vector<SunInfo> shaderSuns;

	const bool suppressShaderSun = worldspawn != nullptr && key_bool( worldspawn->getKeyValue( "_noshadersun" ) );
	std::set<CopiedString> seenSkyShaders;

	Scene_forEachVisibleBrush( GlobalSceneGraph(), [&]( BrushInstance& brush ){
		add_bounds( brush.worldAABB() );

		const Matrix4& localToWorld = brush.localToWorld();

		Brush_ForEachFaceInstance( brush, [&]( FaceInstance& faceInstance ){
			Face& face = faceInstance.getFace();
			if ( !face.contributes() || face.isFiltered() ) {
				return;
			}

			const FaceShader& faceShader = face.getShader();
			const int flags = faceShader.shaderFlags();
			const char* shaderName = face.GetShader();

			if ( ( flags & QER_NODRAW ) != 0 ) {
				return;
			}

			if ( ( flags & QER_SKY ) != 0 ) {
				if ( !suppressShaderSun && seenSkyShaders.insert( shaderName ).second ) {
					ShaderLightInfo& info = shader_light_info( shaderName );
					for ( const auto& sun : info.suns )
					{
						shaderSuns.push_back( sun );
					}
				}
				return;
			}

			ShaderLightInfo& info = shader_light_info( shaderName );
			if ( !info.hasSurfaceLight ) {
				return;
			}

			float area = 0.0f;
			Vector3 centroid( 0, 0, 0 );
			if ( !winding_area_centroid( face.getWinding(), localToWorld, area, centroid ) ) {
				return;
			}

			const float areaScale = clamped_area_scale( area );
			const float intensity = std::fabs( info.surfaceLight ) * areaScale;
			const float radius = light_radius( intensity, 1.0f );
			if ( radius <= 0.0f ) {
				return;
			}

			Vector3 colour = info.hasSurfaceLightColor ? info.surfaceLightColor : faceShader.state()->getTexture().color;
			colour = normalize_colour( colour );
			colour = scaled_colour( colour, intensity, 300.0f );

			preview_lighting_add( AABB( centroid, Vector3( radius, radius, radius ) ), colour );
		} );
	} );

	Scene_forEachVisiblePatchInstance( [&]( PatchInstance& patch ){
		add_bounds( patch.worldAABB() );

		Patch& patchRef = patch.getPatch();
		const Shader* shaderState = patchRef.getShader();
		const int flags = patchRef.getShaderFlags();
		const char* shaderName = patchRef.GetShader();

		if ( ( flags & QER_NODRAW ) != 0 ) {
			return;
		}

		if ( ( flags & QER_SKY ) != 0 ) {
			if ( !suppressShaderSun && seenSkyShaders.insert( shaderName ).second ) {
				ShaderLightInfo& info = shader_light_info( shaderName );
				for ( const auto& sun : info.suns )
				{
					shaderSuns.push_back( sun );
				}
			}
			return;
		}

		ShaderLightInfo& info = shader_light_info( shaderName );
		if ( !info.hasSurfaceLight ) {
			return;
		}

		float area = 0.0f;
		Vector3 centroid( 0, 0, 0 );
		if ( !patch_area_centroid( patchRef.getTesselation(), patch.localToWorld(), area, centroid ) ) {
			return;
		}

		const float areaScale = clamped_area_scale( area );
		const float intensity = std::fabs( info.surfaceLight ) * areaScale;
		const float radius = light_radius( intensity, 1.0f );
		if ( radius <= 0.0f ) {
			return;
		}

		Vector3 colour = info.hasSurfaceLightColor ? info.surfaceLightColor : shaderState->getTexture().color;
		colour = normalize_colour( colour );
		colour = scaled_colour( colour, intensity, 300.0f );

		preview_lighting_add( AABB( centroid, Vector3( radius, radius, radius ) ), colour );
	} );

	const Vector3 mapCenter = hasBounds ? mapBounds.origin : Vector3( 0, 0, 0 );
	if ( worldspawn != nullptr ) {
		SunInfo sun;
		if ( parse_worldspawn_sun( *worldspawn, targets, mapCenter, sun ) ) {
			worldSuns.push_back( sun );
		}
	}

	const bool allowShaderSuns = worldSuns.empty() && !suppressShaderSun;
	if ( !worldSuns.empty() ) {
		add_sun_lights( worldSuns, mapBounds, hasBounds, 100.0f );
	}
	else if ( allowShaderSuns && !shaderSuns.empty() ) {
		add_sun_lights( shaderSuns, mapBounds, hasBounds, 100.0f );
	}

	Scene_forEachEntity( [&]( scene::Instance& instance ){
		Entity* entity = Node_getEntity( instance.path().top() );
		if ( entity == nullptr ) {
			return;
		}
		if ( string_equal_nocase( entity->getClassName(), "worldspawn" ) ) {
			return;
		}

		const char* classname = entity->getClassName();
		if ( !string_equal_nocase_n( classname, "light", 5 ) ) {
			return;
		}

		Vector3 origin( 0, 0, 0 );
		if ( !parse_vec3_key( *entity, "origin", origin ) ) {
			origin = instance.worldAABB().origin;
		}

		Vector3 colour( 1, 1, 1 );
		bool colourFromKey = false;
		parse_vec3_key( *entity, "_color", colour );

		float intensity = 300.0f;
		parse_light_intensity( *entity, colour, colourFromKey, intensity );

		Vector3 radiusVector( 0, 0, 0 );
		const bool hasRadius = parse_light_radius( *entity, radiusVector );

		float scale = 1.0f;
		parse_float_key( *entity, "scale", scale );
		if ( scale <= 0.0f ) {
			scale = 1.0f;
		}

		int spawnflags = 0;
		parse_int_key( *entity, "spawnflags", spawnflags );

		const bool linear = spawnflags_linear( spawnflags );
		const float intensityScaled = std::fabs( intensity * scale );

		Vector3 colourScaled = normalize_colour( colour );
		colourScaled = scaled_colour( colourScaled, intensityScaled, 300.0f );

		if ( string_equal_nocase( classname, "light_environment" ) || key_bool( entity->getKeyValue( "_sun" ) ) ) {
			Vector3 direction( 0, 0, 1 );
			const char* target = entity->getKeyValue( "target" );
			auto it = targets.find( target );
			if ( !string_empty( target ) && it != targets.end() ) {
				direction = origin - it->second;
			}
			else
			{
				float yaw = 0.0f;
				float pitch = 0.0f;
				parse_entity_angles( *entity, yaw, pitch );
				direction = vector3_for_spherical( degrees_to_radians( yaw ), degrees_to_radians( pitch ) );
			}
			if ( vector3_length( direction ) == 0.0f ) {
				direction = Vector3( 0, 0, 1 );
			}
			direction = vector3_normalised( direction );

			std::vector<SunInfo> suns;
			suns.push_back( SunInfo{ normalize_colour( colour ), direction, intensityScaled } );
			add_sun_lights( suns, mapBounds, hasBounds, 300.0f );
			return;
		}

		float radius = 0.0f;
		Vector3 extents( 0, 0, 0 );
		if ( hasRadius ) {
			extents = Vector3( std::fabs( radiusVector.x() ), std::fabs( radiusVector.y() ), std::fabs( radiusVector.z() ) );
			radius = std::max( extents.x(), std::max( extents.y(), extents.z() ) );
		}
		if ( radius <= 0.0f ) {
			radius = linear ? light_radius_linear( intensityScaled, 1.0f ) : light_radius( intensityScaled, 1.0f );
			extents = Vector3( radius, radius, radius );
		}

		if ( radius <= 0.0f ) {
			return;
		}

		preview_lighting_add( AABB( origin, extents ), colourScaled );
	} );
}

void preview_lighting_mark_dirty(){
	g_previewLighting.dirty = true;
}
using PreviewLightingChangedCaller = BindFirstOpaque<detail::FreeCallerWrapper<void()>>;
}

void PreviewLighting_Enable( bool enable ){
	if ( game_is_doom3() ) {
		return;
	}
	if ( enable && !g_previewLighting.callbackRegistered ) {
		AddSceneChangeCallback( makeSignalHandler( PreviewLightingChangedCaller( reinterpret_cast<void*>( preview_lighting_mark_dirty ) ) ) );
		g_previewLighting.callbackRegistered = true;
	}

	if ( g_previewLighting.active == enable ) {
		return;
	}

	g_previewLighting.active = enable;
	g_previewLighting.dirty = true;

	if ( !enable ) {
		preview_lighting_clear();
	}
}

void PreviewLighting_UpdateIfNeeded(){
	if ( !g_previewLighting.active || game_is_doom3() ) {
		return;
	}

	if ( g_previewLighting.dirty ) {
		g_previewLighting.dirty = false;
		preview_lighting_rebuild();
	}
}
