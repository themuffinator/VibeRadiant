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
#include "igl.h"
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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
struct SunInfo
{
	Vector3 colour;
	Vector3 direction;        // direction of light rays (sun -> scene)
	float intensity;
	float devianceRadians = 0.0f;
	int samples = 1;
};

struct SkyLightInfo
{
	float value = 0.0f;
	int iterations = 0;
	int horizonMin = 0;
	int horizonMax = 90;
	bool sampleColor = true;
};

struct ShaderLightInfo
{
	bool parsed = false;
	bool hasSurfaceLight = false;
	float surfaceLight = 0.0f;
	bool hasSurfaceLightColor = false;
	Vector3 surfaceLightColor = Vector3( 1, 1, 1 );
	std::vector<SunInfo> suns;
	std::vector<SkyLightInfo> skylights;
};

enum class PreviewLightKind
{
	Point,
	Directional,
};

struct PreviewLightSource
{
	PreviewLightKind kind = PreviewLightKind::Point;
	Vector3 origin = Vector3( 0, 0, 0 );      // point
	Vector3 direction = Vector3( 0, 0, -1 );  // directional: direction of light rays (light -> scene)
	Vector3 colour = Vector3( 1, 1, 1 );      // linear RGB, 0..n (will be clamped for preview)
	float radius = 0.0f;                      // point
	bool linearFalloff = false;               // point
	AABB influence = AABB();                  // coarse bounds for dirty marking/light culling
};

struct PreviewLightKey
{
	enum class Kind : std::uint8_t
	{
		Entity,
		SurfaceFace,
		SurfacePatch,
		WorldspawnSun,
		ShaderSun,
		ShaderSkyLight,
	};

	Kind kind = Kind::Entity;
	scene::Node* node = nullptr;          // for everything except shader-based lights
	std::uint32_t index = 0;              // face index for SurfaceFace
	std::uint64_t shaderNameHash = 0;     // for ShaderSun

	friend bool operator==( const PreviewLightKey& a, const PreviewLightKey& b ){
		return a.kind == b.kind && a.node == b.node && a.index == b.index && a.shaderNameHash == b.shaderNameHash;
	}
};

struct PreviewLightKeyHash
{
	std::size_t operator()( const PreviewLightKey& key ) const noexcept {
		std::size_t h = 0;
		auto hashCombine = [&]( std::size_t v ){
			h ^= v + 0x9e3779b97f4a7c15ull + ( h << 6 ) + ( h >> 2 );
		};
		hashCombine( std::size_t( key.kind ) );
		hashCombine( std::size_t( reinterpret_cast<std::uintptr_t>( key.node ) ) );
		hashCombine( std::size_t( key.index ) );
		hashCombine( std::size_t( key.shaderNameHash ) );
		return h;
	}
};

struct PreviewLightEntry
{
	std::uint64_t hash = 0;
	PreviewLightSource light;
};

struct FaceLightmap
{
	GLuint texture = 0;
	int width = 0;
	int height = 0;
	Vector4 planeS = Vector4( 0, 0, 0, 0 );
	Vector4 planeT = Vector4( 0, 0, 0, 0 );
};

struct BrushLightingCache
{
	BrushInstance* instance = nullptr;
	std::uint64_t hash = 0;
	AABB worldAabb = AABB();
	std::vector<FaceLightmap> faces;
};

struct PatchLightingCache
{
	PatchInstance* instance = nullptr;
	std::uint64_t hash = 0;
	AABB worldAabb = AABB();
	std::vector<unsigned char> coloursRGBA; // 4 * tess vertex count
};

struct Triangle
{
	Vector3 v0;
	Vector3 v1;
	Vector3 v2;
	AABB aabb;
};

struct BvhNode
{
	AABB aabb;
	std::uint32_t left = 0;
	std::uint32_t right = 0;
	std::uint32_t firstTri = 0;
	std::uint32_t triCount = 0;

	bool isLeaf() const {
		return triCount != 0;
	}
};

struct PreviewLightingState
{
	bool active = false;
	bool sceneDirty = true;
	bool callbackRegistered = false;
	int model = PREVIEW_LIGHTING_MODEL_BAKED_OVERLAY;

	std::map<CopiedString, ShaderLightInfo> shaderCache;

	std::unordered_map<PreviewLightKey, PreviewLightEntry, PreviewLightKeyHash> lights;
	std::unordered_map<scene::Node*, BrushLightingCache> brushes;
	std::unordered_map<scene::Node*, PatchLightingCache> patches;

	// Shadow ray acceleration.
	std::vector<Triangle> triangles;
	std::vector<std::uint32_t> triIndices;
	std::vector<BvhNode> bvh;
	bool geometryDirty = true;
	AABB mapBounds = AABB();
	bool hasMapBounds = false;

	// Work queue.
	std::deque<scene::Node*> dirtyBrushes;
	std::deque<scene::Node*> dirtyPatches;
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
				sun.direction = -vector3_for_spherical( degrees_to_radians( degrees ), degrees_to_radians( elevation ) );
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

inline bool Tokeniser_tryGetFloat( Tokeniser& tokeniser, float& f ){
	const char* token = tokeniser.getToken();
	if ( token != nullptr && string_parse_float( token, f ) ) {
		return true;
	}
	if ( token != nullptr ) {
		tokeniser.ungetToken();
	}
	return false;
}

inline bool Tokeniser_tryGetInteger( Tokeniser& tokeniser, int& i ){
	const char* token = tokeniser.getToken();
	if ( token != nullptr && string_parse_int( token, i ) ) {
		return true;
	}
	if ( token != nullptr ) {
		tokeniser.ungetToken();
	}
	return false;
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
	tokeniser.nextLine();
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

		if ( string_equal_nocase( token, "q3map_skyLight" ) || string_equal_nocase( token, "q3map_skylight" ) ) {
			float value = 0.0f;
			int iterations = 0;
			if ( Tokeniser_getFloat( tokeniser, value ) && Tokeniser_getInteger( tokeniser, iterations ) ) {
				SkyLightInfo sky;
				sky.value = std::max( value, 0.0f );
				sky.iterations = std::max( iterations, 2 );

				int horizonMin = 0;
				if ( Tokeniser_tryGetInteger( tokeniser, horizonMin ) ) {
					sky.horizonMin = std::clamp( horizonMin, -90, 90 );

					int horizonMax = 0;
					if ( Tokeniser_tryGetInteger( tokeniser, horizonMax ) ) {
						sky.horizonMax = std::clamp( horizonMax, -90, 90 );

						int sampleColour = 0;
						if ( Tokeniser_tryGetInteger( tokeniser, sampleColour ) ) {
							sky.sampleColor = sampleColour != 0;
						}
					}
				}

				info.skylights.push_back( sky );
			}
			continue;
		}

		if ( string_equal_nocase( token, "sun" )
		  || string_equal_nocase( token, "q3map_sun" )
		  || string_equal_nocase( token, "q3map_sunExt" ) ) {
			const bool ext = string_equal_nocase( token, "q3map_sunExt" );
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
				sun.direction = -vector3_for_spherical( degrees_to_radians( degrees ), degrees_to_radians( elevation ) );

				if ( ext ) {
					float devianceDegrees = 0.0f;
					if ( Tokeniser_tryGetFloat( tokeniser, devianceDegrees ) ) {
						sun.devianceRadians = degrees_to_radians( std::max( devianceDegrees, 0.0f ) );

						int samples = 0;
						if ( Tokeniser_tryGetInteger( tokeniser, samples ) ) {
							sun.samples = std::max( samples, 1 );
						}
					}
				}

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

inline bool preview_node_participates( const scene::Node* node ){
	return node != nullptr && node->visible();
}

inline bool brush_face_participates_in_preview( const scene::Node* node, const Face& face ){
	return preview_node_participates( node ) && face.contributes() && !face.isFiltered();
}

inline bool patch_participates_in_preview( const scene::Node* node, Patch& patch ){
	return preview_node_participates( node ) && !patch_filtered( patch );
}

inline bool shader_behaves_like_sky( int flags, const ShaderLightInfo& info ){
	return ( flags & QER_SKY ) != 0 || !info.suns.empty() || !info.skylights.empty();
}

inline bool brush_face_receives_preview_lighting( const scene::Node* node, const Face& face ){
	if ( !brush_face_participates_in_preview( node, face ) ) {
		return false;
	}

	const int flags = face.getShader().shaderFlags();
	return ( flags & QER_NODRAW ) == 0 && ( flags & QER_SKY ) == 0;
}

inline bool patch_receives_preview_lighting( const scene::Node* node, Patch& patch ){
	// Keep receiver semantics aligned with the baked overlay patch path.
	return patch_participates_in_preview( node, patch );
}

#if 0
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
#endif

// --------------------------------------------------------------------------------------
// Shadowed "baked-ish" light preview:
// - CPU lightmap for brush faces (texgen onto per-face textures)
// - Per-vertex lighting for patches (using patch tesselation vertices)
// - Incremental: rescan on scene changes, then time-slice lightmap rebuilds
// --------------------------------------------------------------------------------------

namespace preview_lighting_impl
{
constexpr float kAmbient = 0.12f;
constexpr float kShadowBias = 0.5f;
constexpr float kLuxelSize = 24.0f; // world units per preview luxel (brush faces)
constexpr int kMinLightmapRes = 4;
constexpr int kMaxLightmapRes = 64;
constexpr std::uint32_t kBvhLeafSize = 8;
constexpr double kWorkBudgetMs = 6.0;
constexpr float kLightCutoff = 0.002f;

inline std::uint64_t hash_combine_u64( std::uint64_t seed, std::uint64_t value ){
	seed ^= value + 0x9e3779b97f4a7c15ull + ( seed << 6 ) + ( seed >> 2 );
	return seed;
}

inline void hash_u32( std::uint64_t& seed, std::uint32_t value ){
	seed = hash_combine_u64( seed, value );
}

inline void hash_u64( std::uint64_t& seed, std::uint64_t value ){
	seed = hash_combine_u64( seed, value );
}

inline void hash_float( std::uint64_t& seed, float value ){
	static_assert( sizeof( float ) == sizeof( std::uint32_t ) );
	std::uint32_t bits = 0;
	std::memcpy( &bits, &value, sizeof( bits ) );
	hash_u32( seed, bits );
}

inline void hash_vec3( std::uint64_t& seed, const Vector3& v ){
	hash_float( seed, v.x() );
	hash_float( seed, v.y() );
	hash_float( seed, v.z() );
}

inline void hash_string( std::uint64_t& seed, const char* s ){
	hash_u64( seed, std::hash<std::string_view>{}( s != nullptr ? std::string_view( s ) : std::string_view() ) );
}

inline Vector3 aabb_min( const AABB& aabb ){
	return aabb.origin - aabb.extents;
}

inline Vector3 aabb_max( const AABB& aabb ){
	return aabb.origin + aabb.extents;
}

inline AABB aabb_union( const AABB& a, const AABB& b ){
	AABB out = a;
	aabb_extend_by_aabb_safe( out, b );
	return out;
}

inline AABB aabb_from_min_max( const Vector3& mins, const Vector3& maxs ){
	const Vector3 extents = ( maxs - mins ) * 0.5f;
	return AABB( mins + extents, extents );
}

inline bool ray_intersects_aabb( const Vector3& origin, const Vector3& dir, const AABB& aabb, float maxDistance ){
	const Vector3 mins = aabb_min( aabb );
	const Vector3 maxs = aabb_max( aabb );

	float tmin = 0.0f;
	float tmax = maxDistance;

	for ( int axis = 0; axis < 3; ++axis )
	{
		const float o = origin[axis];
		const float d = dir[axis];
		if ( std::fabs( d ) < 1e-8f ) {
			if ( o < mins[axis] || o > maxs[axis] ) {
				return false;
			}
			continue;
		}

		const float inv = 1.0f / d;
		float t1 = ( mins[axis] - o ) * inv;
		float t2 = ( maxs[axis] - o ) * inv;
		if ( t1 > t2 ) {
			std::swap( t1, t2 );
		}
		tmin = std::max( tmin, t1 );
		tmax = std::min( tmax, t2 );
		if ( tmin > tmax ) {
			return false;
		}
	}

	return tmax > 0.0f;
}

inline bool ray_intersects_triangle( const Vector3& origin, const Vector3& dir, const Triangle& tri, float maxDistance ){
	constexpr float eps = 1e-6f;

	const Vector3 e1 = tri.v1 - tri.v0;
	const Vector3 e2 = tri.v2 - tri.v0;
	const Vector3 pvec = vector3_cross( dir, e2 );
	const float det = vector3_dot( e1, pvec );
	if ( std::fabs( det ) < eps ) {
		return false;
	}

	const float invDet = 1.0f / det;
	const Vector3 tvec = origin - tri.v0;
	const float u = vector3_dot( tvec, pvec ) * invDet;
	if ( u < 0.0f || u > 1.0f ) {
		return false;
	}

	const Vector3 qvec = vector3_cross( tvec, e1 );
	const float v = vector3_dot( dir, qvec ) * invDet;
	if ( v < 0.0f || ( u + v ) > 1.0f ) {
		return false;
	}

	const float t = vector3_dot( e2, qvec ) * invDet;
	return t > eps && t < maxDistance;
}

inline AABB triangle_aabb( const Vector3& a, const Vector3& b, const Vector3& c ){
	Vector3 mins = a;
	Vector3 maxs = a;
	for ( const Vector3& v : { b, c } )
	{
		for ( int i = 0; i < 3; ++i )
		{
			mins[i] = std::min( mins[i], v[i] );
			maxs[i] = std::max( maxs[i], v[i] );
		}
	}
	return aabb_from_min_max( mins, maxs );
}

inline Vector3 triangle_centroid( const Triangle& tri ){
	return ( tri.v0 + tri.v1 + tri.v2 ) * ( 1.0f / 3.0f );
}

std::uint32_t bvh_build( std::vector<BvhNode>& outNodes, const std::vector<Triangle>& triangles, std::vector<std::uint32_t>& triIndices, std::uint32_t begin, std::uint32_t end ){
	BvhNode node;

	bool hasBounds = false;
	AABB bounds;
	Vector3 centroidMin( 0, 0, 0 );
	Vector3 centroidMax( 0, 0, 0 );
	bool hasCentroids = false;

	for ( std::uint32_t i = begin; i < end; ++i )
	{
		const Triangle& tri = triangles[triIndices[i]];
		if ( !hasBounds ) {
			bounds = tri.aabb;
			hasBounds = true;
		}
		else
		{
			aabb_extend_by_aabb_safe( bounds, tri.aabb );
		}

		const Vector3 c = triangle_centroid( tri );
		if ( !hasCentroids ) {
			centroidMin = centroidMax = c;
			hasCentroids = true;
		}
		else
		{
			for ( int axis = 0; axis < 3; ++axis )
			{
				centroidMin[axis] = std::min( centroidMin[axis], c[axis] );
				centroidMax[axis] = std::max( centroidMax[axis], c[axis] );
			}
		}
	}

	node.aabb = hasBounds ? bounds : AABB();

	const std::uint32_t nodeIndex = std::uint32_t( outNodes.size() );
	outNodes.push_back( node );

	const std::uint32_t count = end - begin;
	if ( count <= kBvhLeafSize ) {
		outNodes[nodeIndex].firstTri = begin;
		outNodes[nodeIndex].triCount = count;
		return nodeIndex;
	}

	const Vector3 centroidExtents = centroidMax - centroidMin;
	int axis = 0;
	if ( centroidExtents.y() > centroidExtents.x() ) {
		axis = 1;
	}
	if ( centroidExtents.z() > centroidExtents[axis] ) {
		axis = 2;
	}

	const std::uint32_t mid = begin + count / 2;
	std::nth_element(
	    triIndices.begin() + begin,
	    triIndices.begin() + mid,
	    triIndices.begin() + end,
	    [&]( std::uint32_t a, std::uint32_t b ){
		    return triangle_centroid( triangles[a] )[axis] < triangle_centroid( triangles[b] )[axis];
	    }
	);

	outNodes[nodeIndex].left = bvh_build( outNodes, triangles, triIndices, begin, mid );
	outNodes[nodeIndex].right = bvh_build( outNodes, triangles, triIndices, mid, end );
	return nodeIndex;
}

bool bvh_shadowed( const Vector3& origin, const Vector3& dir, float maxDistance ){
	if ( g_previewLighting.bvh.empty() || g_previewLighting.triangles.empty() ) {
		return false;
	}

	std::vector<std::uint32_t> stack;
	stack.reserve( 64 );
	stack.push_back( 0 );

	while ( !stack.empty() )
	{
		const std::uint32_t nodeIndex = stack.back();
		stack.pop_back();
		const BvhNode& node = g_previewLighting.bvh[nodeIndex];

		if ( !ray_intersects_aabb( origin, dir, node.aabb, maxDistance ) ) {
			continue;
		}

		if ( node.isLeaf() ) {
			for ( std::uint32_t i = 0; i < node.triCount; ++i )
			{
				const Triangle& tri = g_previewLighting.triangles[g_previewLighting.triIndices[node.firstTri + i]];
				if ( ray_intersects_triangle( origin, dir, tri, maxDistance ) ) {
					return true;
				}
			}
			continue;
		}

		stack.push_back( node.left );
		stack.push_back( node.right );
	}

	return false;
}

void delete_brush_textures( BrushLightingCache& cache ){
	std::vector<GLuint> textures;
	textures.reserve( cache.faces.size() );
	for ( const auto& face : cache.faces )
	{
		if ( face.texture != 0 ) {
			textures.push_back( face.texture );
		}
	}

	if ( !textures.empty() ) {
		gl().glDeleteTextures( GLsizei( textures.size() ), textures.data() );
	}

	cache.faces.clear();
}

void clear_all_gl(){
	for ( auto& [ node, brush ] : g_previewLighting.brushes )
	{
		delete_brush_textures( brush );
	}

	g_previewLighting.brushes.clear();
	g_previewLighting.patches.clear();
	g_previewLighting.lights.clear();
	g_previewLighting.triangles.clear();
	g_previewLighting.triIndices.clear();
	g_previewLighting.bvh.clear();
	g_previewLighting.dirtyBrushes.clear();
	g_previewLighting.dirtyPatches.clear();
	g_previewLighting.geometryDirty = true;
	g_previewLighting.mapBounds = AABB();
	g_previewLighting.hasMapBounds = false;
	g_previewLighting.sceneDirty = true;
}

std::uint64_t hash_brush_instance( BrushInstance& brush, scene::Node* node ){
	std::uint64_t seed = 0;

	const Matrix4& localToWorld = brush.localToWorld();
	const auto* m = reinterpret_cast<const float*>( &localToWorld );
	for ( int i = 0; i < 16; ++i )
	{
		hash_float( seed, m[i] );
	}

	hash_u32( seed, preview_node_participates( node ) ? 1u : 0u );

	std::uint32_t faceCount = 0;
	Brush_ForEachFaceInstance( brush, [&]( FaceInstance& faceInstance ){
		Face& face = faceInstance.getFace();
		hash_u32( seed, face.contributes() ? 1u : 0u );
		hash_u32( seed, face.isFiltered() ? 1u : 0u );

		const Plane3& p = face.plane3();
		hash_vec3( seed, p.normal() );
		hash_float( seed, p.dist() );

		hash_string( seed, face.GetShader() );
		hash_u32( seed, std::uint32_t( face.getShader().shaderFlags() ) );

		++faceCount;
	} );

	hash_u32( seed, faceCount );
	return seed;
}

void hash_patch_tesselation( std::uint64_t& seed, const PatchTesselation& tess ){
	hash_u32( seed, std::uint32_t( tess.m_vertices.size() ) );
	hash_u32( seed, std::uint32_t( tess.m_indices.size() ) );
	hash_u32( seed, std::uint32_t( tess.m_numStrips ) );
	hash_u32( seed, std::uint32_t( tess.m_lenStrips ) );

	for ( const ArbitraryMeshVertex& v : tess.m_vertices )
	{
		hash_vec3( seed, Vector3( v.vertex ) );
		hash_vec3( seed, Vector3( v.normal ) );
	}

	for ( const RenderIndex index : tess.m_indices )
	{
		hash_u32( seed, std::uint32_t( index ) );
	}
}

std::uint64_t hash_patch_instance( PatchInstance& patch, scene::Node* node ){
	std::uint64_t seed = 0;

	const Matrix4& localToWorld = patch.localToWorld();
	const auto* m = reinterpret_cast<const float*>( &localToWorld );
	for ( int i = 0; i < 16; ++i )
	{
		hash_float( seed, m[i] );
	}

	hash_u32( seed, preview_node_participates( node ) ? 1u : 0u );

	Patch& patchRef = patch.getPatch();
	hash_string( seed, patchRef.GetShader() );
	hash_u32( seed, std::uint32_t( patchRef.getShaderFlags() ) );
	hash_u32( seed, patch_filtered( patchRef ) ? 1u : 0u );

	hash_patch_tesselation( seed, patchRef.getTesselation() );

	return seed;
}

void rebuild_bvh_from_scene(){
	g_previewLighting.triangles.clear();

	for ( auto& [ node, brushCache ] : g_previewLighting.brushes )
	{
		if ( !preview_node_participates( node ) ) {
			continue;
		}

		BrushInstance* brush = brushCache.instance;
		if ( brush == nullptr ) {
			continue;
		}

		brush->getBrush().evaluateBRep();
		const Matrix4& localToWorld = brush->localToWorld();

		Brush_ForEachFaceInstance( *brush, [&]( FaceInstance& faceInstance ){
			Face& face = faceInstance.getFace();
			if ( !brush_face_participates_in_preview( node, face ) ) {
				return;
			}

			const FaceShader& faceShader = face.getShader();
			const int flags = faceShader.shaderFlags();
			const char* shaderName = face.GetShader();
			ShaderLightInfo& info = shader_light_info( shaderName );
			const bool isSky = shader_behaves_like_sky( flags, info );
			if ( isSky ) {
				return;
			}

			const Winding& w = face.getWinding();
			if ( w.numpoints < 3 ) {
				return;
			}

			const Vector3 v0 = matrix4_transformed_point( localToWorld, Vector3( w[0].vertex ) );
			for ( std::size_t i = 1; i + 1 < w.numpoints; ++i )
			{
				const Vector3 v1 = matrix4_transformed_point( localToWorld, Vector3( w[i].vertex ) );
				const Vector3 v2 = matrix4_transformed_point( localToWorld, Vector3( w[i + 1].vertex ) );

				Triangle tri;
				tri.v0 = v0;
				tri.v1 = v1;
				tri.v2 = v2;
				tri.aabb = triangle_aabb( v0, v1, v2 );
				g_previewLighting.triangles.push_back( tri );
			}
		} );
	}

	for ( auto& [ node, patchCache ] : g_previewLighting.patches )
	{
		PatchInstance* patch = patchCache.instance;
		if ( patch == nullptr ) {
			continue;
		}

		Patch& patchRef = patch->getPatch();
		if ( !patch_participates_in_preview( node, patchRef ) ) {
			continue;
		}

		const int flags = patchRef.getShaderFlags();
		const char* shaderName = patchRef.GetShader();
		ShaderLightInfo& info = shader_light_info( shaderName );
		const bool isSky = shader_behaves_like_sky( flags, info );
		if ( isSky ) {
			continue;
		}
		const PatchTesselation& tess = patchRef.getTesselation();
		if ( tess.m_numStrips == 0 || tess.m_lenStrips < 4 ) {
			continue;
		}

		const Matrix4& localToWorld = patch->localToWorld();
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

				{
					Triangle tri;
					tri.v0 = v0;
					tri.v1 = v1;
					tri.v2 = v2;
					tri.aabb = triangle_aabb( v0, v1, v2 );
					g_previewLighting.triangles.push_back( tri );
				}
				{
					Triangle tri;
					tri.v0 = v2;
					tri.v1 = v1;
					tri.v2 = v3;
					tri.aabb = triangle_aabb( tri.v0, tri.v1, tri.v2 );
					g_previewLighting.triangles.push_back( tri );
				}
			}
		}
	}

	g_previewLighting.triIndices.resize( g_previewLighting.triangles.size() );
	for ( std::size_t i = 0; i < g_previewLighting.triIndices.size(); ++i )
	{
		g_previewLighting.triIndices[i] = std::uint32_t( i );
	}

	g_previewLighting.bvh.clear();
	if ( !g_previewLighting.triangles.empty() ) {
		g_previewLighting.bvh.reserve( g_previewLighting.triangles.size() * 2 );
		bvh_build( g_previewLighting.bvh, g_previewLighting.triangles, g_previewLighting.triIndices, 0, std::uint32_t( g_previewLighting.triIndices.size() ) );
	}

	g_previewLighting.geometryDirty = false;
}

inline float point_attenuation( const PreviewLightSource& light, float dist ){
	if ( light.radius <= 0.0f ) {
		return 0.0f;
	}
	const float x = std::clamp( 1.0f - ( dist / light.radius ), 0.0f, 1.0f );
	if ( light.linearFalloff ) {
		return x;
	}
	return x * x;
}

void gather_affecting_lights( const AABB& bounds, std::vector<const PreviewLightSource*>& out ){
	out.clear();
	out.reserve( g_previewLighting.lights.size() );

	for ( const auto& lightPair : g_previewLighting.lights )
	{
		const PreviewLightEntry& entry = lightPair.second;
		if ( aabb_intersects_aabb( entry.light.influence, bounds ) ) {
			out.push_back( &entry.light );
		}
	}
}

Vector3 compute_lighting( const Vector3& worldPos, const Vector3& worldNormal, const std::vector<const PreviewLightSource*>& lights, float directionalDistance, bool includeShadows ){
	Vector3 result( kAmbient, kAmbient, kAmbient );

	const Vector3 normal = vector3_normalised( worldNormal );
	const Vector3 biasedOrigin = worldPos + normal * kShadowBias;

	for ( const PreviewLightSource* light : lights )
	{
		if ( light == nullptr ) {
			continue;
		}

		if ( light->kind == PreviewLightKind::Directional ) {
			const Vector3 L = vector3_normalised( -light->direction );
			const float ndotl = std::max( 0.0f, static_cast<float>( vector3_dot( normal, L ) ) );
			if ( ndotl <= 0.0f ) {
				continue;
			}
			if ( includeShadows && bvh_shadowed( biasedOrigin, L, directionalDistance ) ) {
				continue;
			}
			result += light->colour * ndotl;
			continue;
		}

		const Vector3 toLight = light->origin - worldPos;
		const float dist = vector3_length( toLight );
		if ( dist <= 1e-4f || dist > light->radius ) {
			continue;
		}

		const Vector3 L = toLight / dist;
		const float ndotl = std::max( 0.0f, static_cast<float>( vector3_dot( normal, L ) ) );
		if ( ndotl <= 0.0f ) {
			continue;
		}

		const float atten = point_attenuation( *light, dist );
		if ( atten <= kLightCutoff ) {
			continue;
		}

		if ( includeShadows && bvh_shadowed( biasedOrigin, L, std::max( dist - kShadowBias, 0.0f ) ) ) {
			continue;
		}

		result += light->colour * ( ndotl * atten );
	}

	result[0] = std::clamp( result[0], 0.0f, 1.0f );
	result[1] = std::clamp( result[1], 0.0f, 1.0f );
	result[2] = std::clamp( result[2], 0.0f, 1.0f );
	return result;
}

void update_face_lightmap_texture( FaceLightmap& out, const std::vector<unsigned char>& rgb, int width, int height ){
	if ( width <= 0 || height <= 0 ) {
		return;
	}

	bool created = false;
	if ( out.texture == 0 ) {
		gl().glGenTextures( 1, &out.texture );
		created = true;
	}

	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glBindTexture( GL_TEXTURE_2D, out.texture );

	if ( created ) {
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		gl().glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}

	gl().glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );

	if ( out.width != width || out.height != height ) {
		gl().glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr );
		out.width = width;
		out.height = height;
	}

	gl().glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data() );
}

inline PreviewLightEntry make_point_light( const Vector3& origin, const Vector3& colour, float radius, bool linearFalloff ){
	PreviewLightEntry entry;
	entry.light.kind = PreviewLightKind::Point;
	entry.light.origin = origin;
	entry.light.colour = colour;
	entry.light.radius = std::max( radius, 0.0f );
	entry.light.linearFalloff = linearFalloff;
	entry.light.influence = AABB( origin, Vector3( entry.light.radius, entry.light.radius, entry.light.radius ) );

	hash_u32( entry.hash, std::uint32_t( entry.light.kind ) );
	hash_vec3( entry.hash, entry.light.origin );
	hash_vec3( entry.hash, entry.light.colour );
	hash_float( entry.hash, entry.light.radius );
	hash_u32( entry.hash, entry.light.linearFalloff ? 1u : 0u );

	return entry;
}

inline PreviewLightEntry make_directional_light( const Vector3& direction, const Vector3& colour, const AABB& influence ){
	PreviewLightEntry entry;
	entry.light.kind = PreviewLightKind::Directional;
	entry.light.direction = vector3_normalised( direction );
	entry.light.colour = colour;
	entry.light.influence = influence;

	hash_u32( entry.hash, std::uint32_t( entry.light.kind ) );
	hash_vec3( entry.hash, entry.light.direction );
	hash_vec3( entry.hash, entry.light.colour );

	return entry;
}

void build_brush_lightmaps( BrushLightingCache& cache, const AABB& mapBounds, bool hasBounds ){
	BrushInstance* brush = cache.instance;
	if ( brush == nullptr ) {
		return;
	}

	const float directionalDistance = hasBounds ? std::max( 4096.0f, static_cast<float>( vector3_length( mapBounds.extents ) ) * 4.0f ) : 65536.0f;

	std::vector<const PreviewLightSource*> affectingLights;
	gather_affecting_lights( cache.worldAabb, affectingLights );

	brush->getBrush().evaluateBRep();
	const Matrix4& localToWorld = brush->localToWorld();
	scene::Node* node = &brush->path().top().get();

	// Ensure we have a face slot per visible face instance.
	std::size_t faceCount = 0;
	Brush_ForEachFaceInstance( *brush, [&]( FaceInstance& ){
		++faceCount;
	} );
	cache.faces.resize( faceCount );

	std::size_t faceIndex = 0;
	Brush_ForEachFaceInstance( *brush, [&]( FaceInstance& faceInstance ){
		Face& face = faceInstance.getFace();
		FaceLightmap& out = cache.faces[faceIndex++];

		if ( !brush_face_receives_preview_lighting( node, face ) ) {
			return;
		}

		const Winding& w = face.getWinding();
		if ( w.numpoints < 3 ) {
			return;
		}

		const Plane3& plane = face.plane3();
		const Vector3 normalLocal = Vector3( plane.normal() );

		Vector3 uAxis = vector3_cross( normalLocal, Vector3( 0, 0, 1 ) );
		if ( vector3_length( uAxis ) < 1e-4f ) {
			uAxis = vector3_cross( normalLocal, Vector3( 0, 1, 0 ) );
		}
		uAxis = vector3_normalised( uAxis );
		const Vector3 vAxis = vector3_normalised( vector3_cross( uAxis, normalLocal ) );

		const Vector3 p0 = Vector3( w[0].vertex );

		float minU = 0.0f;
		float maxU = 0.0f;
		float minV = 0.0f;
		float maxV = 0.0f;
		bool first = true;

		for ( std::size_t i = 0; i < w.numpoints; ++i )
		{
			const Vector3 p = Vector3( w[i].vertex );
			const Vector3 d = p - p0;
			const float u = vector3_dot( d, uAxis );
			const float v = vector3_dot( d, vAxis );
			if ( first ) {
				minU = maxU = u;
				minV = maxV = v;
				first = false;
			}
			else
			{
				minU = std::min( minU, u );
				maxU = std::max( maxU, u );
				minV = std::min( minV, v );
				maxV = std::max( maxV, v );
			}
		}

		const float rangeU = maxU - minU;
		const float rangeV = maxV - minV;
		if ( rangeU <= 1e-3f || rangeV <= 1e-3f ) {
			return;
		}

		const int width = std::clamp( int( std::ceil( rangeU / kLuxelSize ) ), kMinLightmapRes, kMaxLightmapRes );
		const int height = std::clamp( int( std::ceil( rangeV / kLuxelSize ) ), kMinLightmapRes, kMaxLightmapRes );

		const float stepU = rangeU / float( width );
		const float stepV = rangeV / float( height );

		{
			const float invU = 1.0f / rangeU;
			const float invV = 1.0f / rangeV;
			const float dU = ( -vector3_dot( p0, uAxis ) - minU ) * invU;
			const float dV = ( -vector3_dot( p0, vAxis ) - minV ) * invV;
			out.planeS = Vector4( uAxis.x() * invU, uAxis.y() * invU, uAxis.z() * invU, dU );
			out.planeT = Vector4( vAxis.x() * invV, vAxis.y() * invV, vAxis.z() * invV, dV );
		}

		std::vector<unsigned char> rgb;
		rgb.resize( std::size_t( width ) * std::size_t( height ) * 3 );

		const Vector3 normalWorld = matrix4_transformed_normal( localToWorld, normalLocal );

		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				const float u = minU + ( float( x ) + 0.5f ) * stepU;
				const float v = minV + ( float( y ) + 0.5f ) * stepV;
				const Vector3 localPos = p0 + uAxis * u + vAxis * v;
				const Vector3 worldPos = matrix4_transformed_point( localToWorld, localPos );

				const Vector3 lit = compute_lighting( worldPos, normalWorld, affectingLights, directionalDistance, true );

				const std::size_t idx = ( std::size_t( y ) * std::size_t( width ) + std::size_t( x ) ) * 3;
				rgb[idx + 0] = static_cast<unsigned char>( std::clamp( lit.x() * 255.0f, 0.0f, 255.0f ) );
				rgb[idx + 1] = static_cast<unsigned char>( std::clamp( lit.y() * 255.0f, 0.0f, 255.0f ) );
				rgb[idx + 2] = static_cast<unsigned char>( std::clamp( lit.z() * 255.0f, 0.0f, 255.0f ) );
			}
		}

		update_face_lightmap_texture( out, rgb, width, height );
	} );
}

void build_patch_colours( PatchLightingCache& cache, const AABB& mapBounds, bool hasBounds ){
	PatchInstance* patch = cache.instance;
	if ( patch == nullptr ) {
		return;
	}

	const float directionalDistance = hasBounds ? std::max( 4096.0f, static_cast<float>( vector3_length( mapBounds.extents ) ) * 4.0f ) : 65536.0f;

	std::vector<const PreviewLightSource*> affectingLights;
	gather_affecting_lights( cache.worldAabb, affectingLights );

	Patch& patchRef = patch->getPatch();
	scene::Node* node = &patch->path().top().get();
	if ( !patch_receives_preview_lighting( node, patchRef ) ) {
		cache.coloursRGBA.clear();
		return;
	}

	const PatchTesselation& tess = patchRef.getTesselation();
	if ( tess.m_vertices.empty() ) {
		cache.coloursRGBA.clear();
		return;
	}

	const Matrix4& localToWorld = patch->localToWorld();
	cache.coloursRGBA.resize( tess.m_vertices.size() * 4 );

	for ( std::size_t i = 0; i < tess.m_vertices.size(); ++i )
	{
		const auto& v = tess.m_vertices[i];
		const Vector3 worldPos = matrix4_transformed_point( localToWorld, v.vertex );
		const Vector3 worldNormal = matrix4_transformed_normal( localToWorld, v.normal );
		const Vector3 lit = compute_lighting( worldPos, worldNormal, affectingLights, directionalDistance, true );

		cache.coloursRGBA[i * 4 + 0] = static_cast<unsigned char>( std::clamp( lit.x() * 255.0f, 0.0f, 255.0f ) );
		cache.coloursRGBA[i * 4 + 1] = static_cast<unsigned char>( std::clamp( lit.y() * 255.0f, 0.0f, 255.0f ) );
		cache.coloursRGBA[i * 4 + 2] = static_cast<unsigned char>( std::clamp( lit.z() * 255.0f, 0.0f, 255.0f ) );
		cache.coloursRGBA[i * 4 + 3] = 255;
	}
}

inline float radical_inverse_vdc( std::uint32_t bits ){
	bits = ( bits << 16 ) | ( bits >> 16 );
	bits = ( ( bits & 0x55555555u ) << 1 ) | ( ( bits & 0xAAAAAAAAu ) >> 1 );
	bits = ( ( bits & 0x33333333u ) << 2 ) | ( ( bits & 0xCCCCCCCCu ) >> 2 );
	bits = ( ( bits & 0x0F0F0F0Fu ) << 4 ) | ( ( bits & 0xF0F0F0F0u ) >> 4 );
	bits = ( ( bits & 0x00FF00FFu ) << 8 ) | ( ( bits & 0xFF00FF00u ) >> 8 );
	return float( bits ) * 2.3283064365386963e-10f; // / 2^32
}

Vector3 jitter_direction( const Vector3& baseDirection, float devianceRadians, int sampleIndex, int sampleCount, std::uint32_t seed ){
	if ( sampleCount <= 1 || sampleIndex <= 0 || devianceRadians <= 0.0f ) {
		return vector3_normalised( baseDirection );
	}

	const double d = std::sqrt( double( baseDirection.x() ) * double( baseDirection.x() ) + double( baseDirection.y() ) * double( baseDirection.y() ) );
	double angle = std::atan2( double( baseDirection.y() ), double( baseDirection.x() ) );
	double elevation = std::atan2( double( baseDirection.z() ), d );

	const float u = ( float( sampleIndex ) + 0.5f ) / float( sampleCount );
	const float v = radical_inverse_vdc( std::uint32_t( sampleIndex ) ^ seed );
	const float r = std::sqrt( std::clamp( u, 0.0f, 1.0f ) ) * devianceRadians;
	const float phi = float( c_2pi ) * v;

	angle += std::cos( phi ) * r;
	elevation += std::sin( phi ) * r;

	return vector3_normalised( vector3_for_spherical( angle, elevation ) );
}

struct SkyLightSample
{
	Vector3 direction; // direction of light rays (sky -> scene)
	float intensity = 0.0f;
};

std::vector<SkyLightSample> make_skylight_samples( const SkyLightInfo& sky ){
	std::vector<SkyLightSample> samples;

	if ( sky.value <= 0.0f || sky.iterations < 2 || sky.horizonMin > sky.horizonMax ) {
		return samples;
	}

	const int iterations = std::max( sky.iterations, 2 );
	const int horizonMin = std::clamp( sky.horizonMin, -90, 90 );
	const int horizonMax = std::clamp( sky.horizonMax, -90, 90 );

	const int doBot = horizonMin == -90 ? 1 : 0;
	const int doTop = horizonMax == 90 ? 1 : 0;

	const int angleSteps = std::max( 1, ( iterations - 1 ) * 4 );
	const float eleStep = 90.0f / float( iterations );
	const float elevationStep = float( degrees_to_radians( eleStep ) );
	const float angleStep = float( degrees_to_radians( 360.0f / float( angleSteps ) ) );

	const float eleMin = doBot ? -90.0f + eleStep * 1.5f : float( horizonMin ) + eleStep * 0.5f;
	const float eleMax = doTop ? 90.0f - eleStep * 1.5f : float( horizonMax ) - eleStep * 0.5f;

	const float stepsF = 1.0f + std::max( 0.0f, ( eleMax - eleMin ) / eleStep );
	const int elevationSteps = std::max( 1, int( std::floor( stepsF + 0.5f ) ) );

	const int numSuns = angleSteps * elevationSteps + doBot + doTop;
	const float horizonScale = std::max( 0.25f, float( horizonMax - horizonMin ) / 90.0f );
	const float intensity = sky.value / float( std::max( numSuns, 1 ) ) * horizonScale;

	samples.reserve( std::size_t( numSuns ) );

	float elevation = float( degrees_to_radians( std::min( eleMin, float( horizonMax ) ) ) );
	float angle = 0.0f;
	for ( int i = 0; i < elevationSteps; ++i )
	{
		for ( int j = 0; j < angleSteps; ++j )
		{
			const Vector3 toSky = vector3_for_spherical( angle, elevation );
			SkyLightSample s;
			s.direction = vector3_normalised( -toSky );
			s.intensity = intensity;
			samples.push_back( s );

			angle += angleStep;
		}

		elevation += elevationStep;
		angle += angleStep / float( elevationSteps );
	}

	if ( doBot ) {
		SkyLightSample s;
		s.direction = g_vector3_axis_z;
		s.intensity = intensity;
		samples.push_back( s );
	}
	if ( doTop ) {
		SkyLightSample s;
		s.direction = -g_vector3_axis_z;
		s.intensity = intensity;
		samples.push_back( s );
	}

	return samples;
}

template<typename Functor>
class BrushInstanceAllWalker final : public scene::Graph::Walker
{
	const Functor& m_functor;

public:
	explicit BrushInstanceAllWalker( const Functor& functor )
		: m_functor( functor ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( BrushInstance* brush = Instance_getBrush( instance ) ) {
			m_functor( *brush );
		}
		return true;
	}
};

template<typename Functor>
inline const Functor& Scene_forEachBrushInstanceAll( scene::Graph& graph, const Functor& functor ){
	graph.traverse( BrushInstanceAllWalker<Functor>( functor ) );
	return functor;
}

template<typename Functor>
class PatchInstanceAllWalker final : public scene::Graph::Walker
{
	const Functor& m_functor;

public:
	explicit PatchInstanceAllWalker( const Functor& functor )
		: m_functor( functor ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( PatchInstance* patch = Instance_getPatch( instance ) ) {
			m_functor( *patch );
		}
		return true;
	}
};

template<typename Functor>
inline const Functor& Scene_forEachPatchInstanceAll( scene::Graph& graph, const Functor& functor ){
	graph.traverse( PatchInstanceAllWalker<Functor>( functor ) );
	return functor;
}

struct RescanResult
{
	std::unordered_map<PreviewLightKey, PreviewLightEntry, PreviewLightKeyHash> lights;
	std::unordered_map<scene::Node*, BrushLightingCache> brushes;
	std::unordered_map<scene::Node*, PatchLightingCache> patches;

	AABB mapBounds = AABB();
	bool hasBounds = false;

	std::vector<AABB> changedLightInfluences;
	std::vector<AABB> changedOccluderAabbs;

	std::unordered_set<scene::Node*> dirtyBrushes;
	std::unordered_set<scene::Node*> dirtyPatches;

	bool geometryDirty = false;
};

RescanResult rescan_scene();
void apply_rescan( RescanResult& scan );
void update();
void render_overlay();

RescanResult rescan_scene(){
	RescanResult out;

	auto oldLights = std::move( g_previewLighting.lights );
	auto oldBrushes = std::move( g_previewLighting.brushes );
	auto oldPatches = std::move( g_previewLighting.patches );

	std::map<CopiedString, Vector3> targets;
	Entity* worldspawnEntity = nullptr;
	scene::Node* worldspawnNode = nullptr;

	struct LightEntityCandidate
	{
		scene::Node* node = nullptr;
		Entity* entity = nullptr;
		AABB worldAabb = AABB();
	};
	std::vector<LightEntityCandidate> lightEntities;

	Scene_forEachEntity( [&]( scene::Instance& instance ){
		scene::Node& node = instance.path().top().get();
		Entity* entity = Node_getEntity( node );
		if ( entity == nullptr ) {
			return;
		}

		if ( string_equal_nocase( entity->getClassName(), "worldspawn" ) ) {
			worldspawnEntity = entity;
			worldspawnNode = &node;
		}

		const char* targetname = entity->getKeyValue( "targetname" );
		if ( !string_empty( targetname ) ) {
			Vector3 origin( 0, 0, 0 );
			if ( !parse_vec3_key( *entity, "origin", origin ) ) {
				origin = instance.worldAABB().origin;
			}
			targets.emplace( CopiedString( targetname ), origin );
		}

		const char* classname = entity->getClassName();
		if ( string_equal_nocase_n( classname, "light", 5 ) && !string_equal_nocase( classname, "worldspawn" ) ) {
			lightEntities.push_back( LightEntityCandidate{ &node, entity, instance.worldAABB() } );
		}
	} );

	auto add_bounds = [&]( const AABB& aabb ){
		if ( !out.hasBounds ) {
			out.mapBounds = aabb;
			out.hasBounds = true;
		}
		else
		{
			aabb_extend_by_aabb_safe( out.mapBounds, aabb );
		}
	};

	std::vector<SunInfo> worldSuns;
	struct ShaderSunCandidate
	{
		std::uint64_t shaderHash = 0;
		std::uint32_t sunIndex = 0;
		SunInfo sun;
	};
	std::vector<ShaderSunCandidate> shaderSuns;

	struct ShaderSkyLightCandidate
	{
		std::uint64_t shaderHash = 0;
		std::uint32_t skyLightIndex = 0;
		SkyLightInfo skylight;
		Vector3 colour = Vector3( 1, 1, 1 );
	};
	std::vector<ShaderSkyLightCandidate> shaderSkyLights;

	const bool suppressShaderSun = worldspawnEntity != nullptr && key_bool( worldspawnEntity->getKeyValue( "_noshadersun" ) );
	std::set<CopiedString> seenSkyShaders;

	Scene_forEachBrushInstanceAll( GlobalSceneGraph(), [&]( BrushInstance& brush ){
		add_bounds( brush.worldAABB() );

		scene::Node* node = &brush.path().top().get();
		const std::uint64_t hash = hash_brush_instance( brush, node );

		BrushLightingCache cache;
		cache.instance = &brush;
		cache.hash = hash;
		cache.worldAabb = brush.worldAABB();

		auto oldIt = oldBrushes.find( node );
		if ( oldIt != oldBrushes.end() && oldIt->second.hash == hash ) {
			cache.faces = std::move( oldIt->second.faces );
			oldBrushes.erase( oldIt );
		}
		else
		{
			out.geometryDirty = true;
			out.dirtyBrushes.insert( node );
			if ( oldIt != oldBrushes.end() ) {
				out.changedOccluderAabbs.push_back( oldIt->second.worldAabb );
				delete_brush_textures( oldIt->second );
				oldBrushes.erase( oldIt );
			}
			out.changedOccluderAabbs.push_back( cache.worldAabb );
		}

		out.brushes.emplace( node, std::move( cache ) );

		const Matrix4& localToWorld = brush.localToWorld();
		std::uint32_t faceIndex = 0;
		Brush_ForEachFaceInstance( brush, [&]( FaceInstance& faceInstance ){
			Face& face = faceInstance.getFace();
			const FaceShader& faceShader = face.getShader();
			const int flags = faceShader.shaderFlags();
			const char* shaderName = face.GetShader();
			const std::uint64_t shaderNameHash = std::hash<std::string_view>{}( shaderName != nullptr ? std::string_view( shaderName ) : std::string_view() );

			if ( !brush_face_participates_in_preview( node, face ) ) {
				++faceIndex;
				return;
			}

			ShaderLightInfo& info = shader_light_info( shaderName );
			const bool isSky = shader_behaves_like_sky( flags, info );

			if ( isSky ) {
				if ( seenSkyShaders.insert( shaderName ).second ) {
					if ( !suppressShaderSun ) {
						for ( std::size_t i = 0; i < info.suns.size(); ++i )
						{
							ShaderSunCandidate c;
							c.shaderHash = shaderNameHash;
							c.sunIndex = std::uint32_t( i );
							c.sun = info.suns[i];
							shaderSuns.push_back( c );
						}
					}

					if ( !info.skylights.empty() ) {
						Vector3 colour = info.hasSurfaceLightColor ? info.surfaceLightColor : faceShader.state()->getTexture().color;
						colour = normalize_colour( colour );

						for ( std::size_t i = 0; i < info.skylights.size(); ++i )
						{
							ShaderSkyLightCandidate c;
							c.shaderHash = shaderNameHash;
							c.skyLightIndex = std::uint32_t( i );
							c.skylight = info.skylights[i];
							c.colour = colour;
							shaderSkyLights.push_back( c );
						}
					}
				}
				++faceIndex;
				return;
			}

			if ( !info.hasSurfaceLight ) {
				++faceIndex;
				return;
			}

			float area = 0.0f;
			Vector3 centroid( 0, 0, 0 );
			if ( !winding_area_centroid( face.getWinding(), localToWorld, area, centroid ) ) {
				++faceIndex;
				return;
			}

			const float areaScale = clamped_area_scale( area );
			const float intensity = std::fabs( info.surfaceLight ) * areaScale;
			const float radius = light_radius( intensity, 1.0f );
			if ( radius <= 0.0f ) {
				++faceIndex;
				return;
			}

			Vector3 colour = info.hasSurfaceLightColor ? info.surfaceLightColor : faceShader.state()->getTexture().color;
			colour = normalize_colour( colour );
			colour = scaled_colour( colour, intensity, 300.0f );

			PreviewLightKey key;
			key.kind = PreviewLightKey::Kind::SurfaceFace;
			key.node = node;
			key.index = faceIndex;

			out.lights.emplace( key, make_point_light( centroid, colour, radius, false ) );

			++faceIndex;
		} );
	} );

	Scene_forEachPatchInstanceAll( GlobalSceneGraph(), [&]( PatchInstance& patch ){
		add_bounds( patch.worldAABB() );

		scene::Node* node = &patch.path().top().get();
		const std::uint64_t hash = hash_patch_instance( patch, node );

		PatchLightingCache cache;
		cache.instance = &patch;
		cache.hash = hash;
		cache.worldAabb = patch.worldAABB();

		auto oldIt = oldPatches.find( node );
		if ( oldIt != oldPatches.end() && oldIt->second.hash == hash ) {
			cache.coloursRGBA = std::move( oldIt->second.coloursRGBA );
			oldPatches.erase( oldIt );
		}
		else
		{
			out.geometryDirty = true;
			out.dirtyPatches.insert( node );
			if ( oldIt != oldPatches.end() ) {
				out.changedOccluderAabbs.push_back( oldIt->second.worldAabb );
				oldPatches.erase( oldIt );
			}
			out.changedOccluderAabbs.push_back( cache.worldAabb );
		}

		out.patches.emplace( node, std::move( cache ) );

		Patch& patchRef = patch.getPatch();
		if ( !patch_participates_in_preview( node, patchRef ) ) {
			return;
		}

		const Shader* shaderState = patchRef.getShader();
		const int flags = patchRef.getShaderFlags();
		const char* shaderName = patchRef.GetShader();
		const std::uint64_t shaderNameHash = std::hash<std::string_view>{}( shaderName != nullptr ? std::string_view( shaderName ) : std::string_view() );

		ShaderLightInfo& info = shader_light_info( shaderName );
		const bool isSky = shader_behaves_like_sky( flags, info );
		if ( isSky ) {
			if ( seenSkyShaders.insert( shaderName ).second ) {
				if ( !suppressShaderSun ) {
					for ( std::size_t i = 0; i < info.suns.size(); ++i )
					{
						ShaderSunCandidate c;
						c.shaderHash = shaderNameHash;
						c.sunIndex = std::uint32_t( i );
						c.sun = info.suns[i];
						shaderSuns.push_back( c );
					}
				}

				if ( !info.skylights.empty() ) {
					Vector3 colour = info.hasSurfaceLightColor ? info.surfaceLightColor : shaderState->getTexture().color;
					colour = normalize_colour( colour );

					for ( std::size_t i = 0; i < info.skylights.size(); ++i )
					{
						ShaderSkyLightCandidate c;
						c.shaderHash = shaderNameHash;
						c.skyLightIndex = std::uint32_t( i );
						c.skylight = info.skylights[i];
						c.colour = colour;
						shaderSkyLights.push_back( c );
					}
				}
			}
			return;
		}

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

		PreviewLightKey key;
		key.kind = PreviewLightKey::Kind::SurfacePatch;
		key.node = node;

		out.lights.emplace( key, make_point_light( centroid, colour, radius, false ) );
	} );

	// Removed brushes/paches.
	for ( auto& [ node, cache ] : oldBrushes )
	{
		out.geometryDirty = true;
		out.changedOccluderAabbs.push_back( cache.worldAabb );
		delete_brush_textures( cache );
	}
	for ( auto& [ node, cache ] : oldPatches )
	{
		out.geometryDirty = true;
		out.changedOccluderAabbs.push_back( cache.worldAabb );
	}

	// World/Shader suns.
	const Vector3 mapCenter = out.hasBounds ? out.mapBounds.origin : Vector3( 0, 0, 0 );
	if ( worldspawnEntity != nullptr ) {
		SunInfo sun;
		if ( parse_worldspawn_sun( *worldspawnEntity, targets, mapCenter, sun ) ) {
			worldSuns.push_back( sun );
		}
	}

	const bool allowShaderSuns = worldSuns.empty() && !suppressShaderSun;
	const AABB sunInfluence = out.hasBounds ? out.mapBounds : AABB( Vector3( 0, 0, 0 ), Vector3( 8192, 8192, 8192 ) );

	// Shader skylights (q3map_skyLight / q3map_skylight).
	{
		constexpr std::size_t kMaxSkyLightSamples = 64;
		for ( const ShaderSkyLightCandidate& c : shaderSkyLights )
		{
			if ( c.skylight.value <= 0.0f ) {
				continue;
			}

			std::vector<SkyLightSample> samples = make_skylight_samples( c.skylight );
			if ( samples.size() > kMaxSkyLightSamples ) {
				const std::size_t step = std::max( std::size_t( 1 ), ( samples.size() + kMaxSkyLightSamples - 1 ) / kMaxSkyLightSamples );

				std::vector<SkyLightSample> reduced;
				reduced.reserve( ( samples.size() + step - 1 ) / step );
				for ( std::size_t i = 0; i < samples.size(); i += step )
				{
					reduced.push_back( samples[i] );
				}

				if ( !reduced.empty() ) {
					const float scale = float( samples.size() ) / float( reduced.size() );
					for ( SkyLightSample& s : reduced )
					{
						s.intensity *= scale;
					}
				}

				samples.swap( reduced );
			}

			for ( std::size_t i = 0; i < samples.size(); ++i )
			{
				const Vector3 colour = scaled_colour( c.colour, samples[i].intensity, 100.0f );

				PreviewLightKey key;
				key.kind = PreviewLightKey::Kind::ShaderSkyLight;
				key.shaderNameHash = c.shaderHash;
				key.index = ( c.skyLightIndex << 16 ) | std::uint32_t( i & 0xFFFFu );

				out.lights.emplace( key, make_directional_light( samples[i].direction, colour, sunInfluence ) );
			}
		}
	}

	if ( !worldSuns.empty() ) {
		const SunInfo& sun = worldSuns.front();
		const int sampleCount = std::clamp( sun.samples, 1, 64 );
		const Vector3 colour = scaled_colour( sun.colour, sun.intensity, 100.0f ) * ( 1.0f / float( sampleCount ) );

		for ( int i = 0; i < sampleCount; ++i )
		{
			PreviewLightKey key;
			key.kind = PreviewLightKey::Kind::WorldspawnSun;
			key.node = worldspawnNode;
			key.index = std::uint32_t( i );

			const Vector3 direction = jitter_direction( sun.direction, std::max( sun.devianceRadians, 0.0f ), i, sampleCount, 0x9e3779b9u );
			out.lights.emplace( key, make_directional_light( direction, colour, sunInfluence ) );
		}
	}
	else if ( allowShaderSuns && !shaderSuns.empty() ) {
		for ( const ShaderSunCandidate& c : shaderSuns )
		{
			const int sampleCount = std::clamp( c.sun.samples, 1, 64 );
			const Vector3 colour = scaled_colour( c.sun.colour, c.sun.intensity, 100.0f ) * ( 1.0f / float( sampleCount ) );
			const std::uint32_t seed = std::uint32_t( c.shaderHash ) ^ ( c.sunIndex * 0x9e3779b9u );

			for ( int i = 0; i < sampleCount; ++i )
			{
				PreviewLightKey key;
				key.kind = PreviewLightKey::Kind::ShaderSun;
				key.shaderNameHash = c.shaderHash;
				key.index = ( c.sunIndex << 16 ) | std::uint32_t( i & 0xFFFFu );

				const Vector3 direction = jitter_direction( c.sun.direction, std::max( c.sun.devianceRadians, 0.0f ), i, sampleCount, seed );
				out.lights.emplace( key, make_directional_light( direction, colour, sunInfluence ) );
			}
		}
	}

	// Light entities (point + directional).
	for ( const auto& c : lightEntities )
	{
		if ( c.node == nullptr || c.entity == nullptr ) {
			continue;
		}

		Vector3 origin( 0, 0, 0 );
		if ( !parse_vec3_key( *c.entity, "origin", origin ) ) {
			origin = c.worldAabb.origin;
		}

		Vector3 colour( 1, 1, 1 );
		bool colourFromKey = false;
		parse_vec3_key( *c.entity, "_color", colour );

		float intensity = 300.0f;
		parse_light_intensity( *c.entity, colour, colourFromKey, intensity );

		Vector3 radiusVector( 0, 0, 0 );
		const bool hasRadius = parse_light_radius( *c.entity, radiusVector );

		float scale = 1.0f;
		parse_float_key( *c.entity, "scale", scale );
		if ( scale <= 0.0f ) {
			scale = 1.0f;
		}

		int spawnflags = 0;
		parse_int_key( *c.entity, "spawnflags", spawnflags );
		const bool linear = spawnflags_linear( spawnflags );

		const float intensityScaled = std::fabs( intensity * scale );
		Vector3 colourScaled = normalize_colour( colour );
		colourScaled = scaled_colour( colourScaled, intensityScaled, 300.0f );

		const char* classname = c.entity->getClassName();

		PreviewLightKey key;
		key.kind = PreviewLightKey::Kind::Entity;
		key.node = c.node;

		if ( string_equal_nocase( classname, "light_environment" ) || key_bool( c.entity->getKeyValue( "_sun" ) ) ) {
			Vector3 direction( 0, 0, 1 );
			const char* target = c.entity->getKeyValue( "target" );
			auto it = targets.find( target );
			if ( !string_empty( target ) && it != targets.end() ) {
				direction = origin - it->second;
			}
			else
			{
				float yaw = 0.0f;
				float pitch = 0.0f;
				parse_entity_angles( *c.entity, yaw, pitch );
				direction = vector3_for_spherical( degrees_to_radians( yaw ), degrees_to_radians( pitch ) );
			}
			if ( vector3_length( direction ) == 0.0f ) {
				direction = Vector3( 0, 0, 1 );
			}
			direction = vector3_normalised( direction );

			out.lights.emplace( key, make_directional_light( direction, colourScaled, sunInfluence ) );
			continue;
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
			continue;
		}

		out.lights.emplace( key, make_point_light( origin, colourScaled, radius, linear ) );
	}

	// Diff lights.
	for ( const auto& [ key, entry ] : out.lights )
	{
		auto oldIt = oldLights.find( key );
		if ( oldIt == oldLights.end() ) {
			out.changedLightInfluences.push_back( entry.light.influence );
			continue;
		}

		if ( oldIt->second.hash != entry.hash ) {
			out.changedLightInfluences.push_back( aabb_union( oldIt->second.light.influence, entry.light.influence ) );
		}
		oldLights.erase( oldIt );
	}
	for ( const auto& [ key, entry ] : oldLights )
	{
		out.changedLightInfluences.push_back( entry.light.influence );
	}

	return out;
}

void apply_rescan( RescanResult& scan ){
	// Dirty marking: if a light changes, relight receivers in its influence.
	for ( const AABB& influence : scan.changedLightInfluences )
	{
		for ( const auto& [ node, brush ] : scan.brushes )
		{
			if ( aabb_intersects_aabb( influence, brush.worldAabb ) ) {
				scan.dirtyBrushes.insert( node );
			}
		}
		for ( const auto& [ node, patch ] : scan.patches )
		{
			if ( aabb_intersects_aabb( influence, patch.worldAabb ) ) {
				scan.dirtyPatches.insert( node );
			}
		}
	}

	// Shadowing changes: if geometry changes, relight receivers for lights that overlap it.
	for ( const AABB& occluder : scan.changedOccluderAabbs )
	{
		for ( const auto& [ key, light ] : scan.lights )
		{
			if ( !aabb_intersects_aabb( occluder, light.light.influence ) ) {
				continue;
			}
			for ( const auto& [ node, brush ] : scan.brushes )
			{
				if ( aabb_intersects_aabb( light.light.influence, brush.worldAabb ) ) {
					scan.dirtyBrushes.insert( node );
				}
			}
			for ( const auto& [ node, patch ] : scan.patches )
			{
				if ( aabb_intersects_aabb( light.light.influence, patch.worldAabb ) ) {
					scan.dirtyPatches.insert( node );
				}
			}
		}
	}

	// Replace caches.
	g_previewLighting.lights = std::move( scan.lights );
	g_previewLighting.brushes = std::move( scan.brushes );
	g_previewLighting.patches = std::move( scan.patches );
	g_previewLighting.geometryDirty = g_previewLighting.geometryDirty || scan.geometryDirty;
	g_previewLighting.mapBounds = scan.mapBounds;
	g_previewLighting.hasMapBounds = scan.hasBounds;

	// Merge dirty queues.
	std::unordered_set<scene::Node*> brushQueueSet( scan.dirtyBrushes.begin(), scan.dirtyBrushes.end() );
	for ( scene::Node* node : g_previewLighting.dirtyBrushes )
	{
		brushQueueSet.insert( node );
	}
	std::unordered_set<scene::Node*> patchQueueSet( scan.dirtyPatches.begin(), scan.dirtyPatches.end() );
	for ( scene::Node* node : g_previewLighting.dirtyPatches )
	{
		patchQueueSet.insert( node );
	}

	g_previewLighting.dirtyBrushes.clear();
	for ( scene::Node* node : brushQueueSet )
	{
		if ( g_previewLighting.brushes.find( node ) != g_previewLighting.brushes.end() ) {
			g_previewLighting.dirtyBrushes.push_back( node );
		}
	}

	g_previewLighting.dirtyPatches.clear();
	for ( scene::Node* node : patchQueueSet )
	{
		if ( g_previewLighting.patches.find( node ) != g_previewLighting.patches.end() ) {
			g_previewLighting.dirtyPatches.push_back( node );
		}
	}
}

void update(){
	if ( g_previewLighting.sceneDirty && g_previewLighting.active ) {
		g_previewLighting.sceneDirty = false;
		RescanResult scan = rescan_scene();
		apply_rescan( scan );
	}

	if ( !g_previewLighting.active ) {
		return;
	}

	if ( g_previewLighting.model != PREVIEW_LIGHTING_MODEL_FAST_INTERACTION && g_previewLighting.geometryDirty ) {
		rebuild_bvh_from_scene();
	}

	if ( g_previewLighting.model == PREVIEW_LIGHTING_MODEL_FAST_INTERACTION ) {
		return;
	}

	// Map bounds for directional shadow rays (cached from the latest scene rescan).
	const AABB mapBounds = g_previewLighting.mapBounds;
	const bool hasBounds = g_previewLighting.hasMapBounds;

	// Time-sliced updates.
	const auto start = std::chrono::steady_clock::now();
	auto within_budget = [&](){
		const auto now = std::chrono::steady_clock::now();
		return std::chrono::duration<double, std::milli>( now - start ).count() < kWorkBudgetMs;
	};

	while ( !g_previewLighting.dirtyBrushes.empty() && within_budget() )
	{
		scene::Node* node = g_previewLighting.dirtyBrushes.front();
		g_previewLighting.dirtyBrushes.pop_front();

		auto it = g_previewLighting.brushes.find( node );
		if ( it == g_previewLighting.brushes.end() ) {
			continue;
		}

		build_brush_lightmaps( it->second, mapBounds, hasBounds );
	}

	while ( !g_previewLighting.dirtyPatches.empty() && within_budget() )
	{
		scene::Node* node = g_previewLighting.dirtyPatches.front();
		g_previewLighting.dirtyPatches.pop_front();

		auto it = g_previewLighting.patches.find( node );
		if ( it == g_previewLighting.patches.end() ) {
			continue;
		}

		build_patch_colours( it->second, mapBounds, hasBounds );
	}
}

void render_overlay_fast_interaction(){
	// DarkRadiant-style fast mode: direct interaction approximation without shadow volumes.
	gl().glUseProgram( 0 );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glEnableClientState( GL_VERTEX_ARRAY );
	gl().glEnableClientState( GL_COLOR_ARRAY );
	gl().glColor4f( 1, 1, 1, 1 );

	gl().glDepthMask( GL_FALSE );
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LEQUAL );
	gl().glDisable( GL_LIGHTING );

	gl().glDisable( GL_TEXTURE_2D );
	gl().glDisable( GL_TEXTURE_GEN_S );
	gl().glDisable( GL_TEXTURE_GEN_T );

	gl().glEnable( GL_BLEND );
	gl().glBlendFunc( GL_ZERO, GL_SRC_COLOR );

	const float directionalDistance = g_previewLighting.hasMapBounds
	                                  ? std::max( 4096.0f, static_cast<float>( vector3_length( g_previewLighting.mapBounds.extents ) ) * 4.0f )
	                                  : 65536.0f;

	std::vector<const PreviewLightSource*> affectingLights;
	std::vector<unsigned char> coloursRGBA;

	for ( auto& [ node, cache ] : g_previewLighting.brushes )
	{
		if ( !preview_node_participates( node ) ) {
			continue;
		}

		BrushInstance* brush = cache.instance;
		if ( brush == nullptr ) {
			continue;
		}

		gather_affecting_lights( cache.worldAabb, affectingLights );

		brush->getBrush().evaluateBRep();
		const Matrix4& localToWorld = brush->localToWorld();

		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &localToWorld ) );

		Brush_ForEachFaceInstance( *brush, [&]( FaceInstance& faceInstance ){
			Face& face = faceInstance.getFace();
			if ( !brush_face_receives_preview_lighting( node, face ) ) {
				return;
			}

			const Winding& w = face.getWinding();
			if ( w.numpoints < 3 ) {
				return;
			}

			const Plane3& plane = face.plane3();
			const Vector3 normalWorld = matrix4_transformed_normal( localToWorld, Vector3( plane.normal() ) );

			coloursRGBA.resize( w.numpoints * 4 );
			for ( std::size_t i = 0; i < w.numpoints; ++i )
			{
				const Vector3 worldPos = matrix4_transformed_point( localToWorld, Vector3( w[i].vertex ) );
				const Vector3 lit = compute_lighting( worldPos, normalWorld, affectingLights, directionalDistance, false );
				coloursRGBA[i * 4 + 0] = static_cast<unsigned char>( std::clamp( lit.x() * 255.0f, 0.0f, 255.0f ) );
				coloursRGBA[i * 4 + 1] = static_cast<unsigned char>( std::clamp( lit.y() * 255.0f, 0.0f, 255.0f ) );
				coloursRGBA[i * 4 + 2] = static_cast<unsigned char>( std::clamp( lit.z() * 255.0f, 0.0f, 255.0f ) );
				coloursRGBA[i * 4 + 3] = 255;
			}

			gl().glVertexPointer( 3, GL_DOUBLE, sizeof( WindingVertex ), &w.points.data()->vertex );
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, 0, coloursRGBA.data() );
			gl().glDrawArrays( GL_POLYGON, 0, GLsizei( w.numpoints ) );
		} );

		gl().glPopMatrix();
	}

	for ( auto& [ node, cache ] : g_previewLighting.patches )
	{
		PatchInstance* patch = cache.instance;
		if ( patch == nullptr ) {
			continue;
		}

		Patch& patchRef = patch->getPatch();
		if ( !patch_receives_preview_lighting( node, patchRef ) ) {
			continue;
		}

		const PatchTesselation& tess = patchRef.getTesselation();
		if ( tess.m_vertices.empty() ) {
			continue;
		}

		gather_affecting_lights( cache.worldAabb, affectingLights );
		coloursRGBA.resize( tess.m_vertices.size() * 4 );

		const Matrix4& localToWorld = patch->localToWorld();
		for ( std::size_t i = 0; i < tess.m_vertices.size(); ++i )
		{
			const auto& v = tess.m_vertices[i];
			const Vector3 worldPos = matrix4_transformed_point( localToWorld, v.vertex );
			const Vector3 worldNormal = matrix4_transformed_normal( localToWorld, v.normal );
			const Vector3 lit = compute_lighting( worldPos, worldNormal, affectingLights, directionalDistance, false );

			coloursRGBA[i * 4 + 0] = static_cast<unsigned char>( std::clamp( lit.x() * 255.0f, 0.0f, 255.0f ) );
			coloursRGBA[i * 4 + 1] = static_cast<unsigned char>( std::clamp( lit.y() * 255.0f, 0.0f, 255.0f ) );
			coloursRGBA[i * 4 + 2] = static_cast<unsigned char>( std::clamp( lit.z() * 255.0f, 0.0f, 255.0f ) );
			coloursRGBA[i * 4 + 3] = 255;
		}

		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &localToWorld ) );

		gl().glVertexPointer( 3, GL_FLOAT, sizeof( ArbitraryMeshVertex ), &tess.m_vertices.data()->vertex );
		gl().glColorPointer( 4, GL_UNSIGNED_BYTE, 0, coloursRGBA.data() );

		const RenderIndex* strip_indices = tess.m_indices.data();
		for ( std::size_t i = 0; i < tess.m_numStrips; ++i, strip_indices += tess.m_lenStrips )
		{
			gl().glDrawElements( GL_QUAD_STRIP, GLsizei( tess.m_lenStrips ), RenderIndexTypeID, strip_indices );
		}

		gl().glPopMatrix();
	}

	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisable( GL_BLEND );
	gl().glDepthMask( GL_TRUE );
	gl().glColor4f( 1, 1, 1, 1 );
}

void render_overlay(){
	if ( !g_previewLighting.active ) {
		return;
	}

	if ( g_previewLighting.model == PREVIEW_LIGHTING_MODEL_FAST_INTERACTION ) {
		render_overlay_fast_interaction();
		return;
	}

	gl().glEnableClientState( GL_VERTEX_ARRAY );

	// Ensure fixed-function state for the overlay pass.
	gl().glUseProgram( 0 );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glColor4f( 1, 1, 1, 1 );

	gl().glDepthMask( GL_FALSE );
	gl().glEnable( GL_DEPTH_TEST );
	gl().glDepthFunc( GL_LEQUAL );
	gl().glDisable( GL_LIGHTING );

	// Multiplicative blend: dst = dst * src
	gl().glEnable( GL_BLEND );
	gl().glBlendFunc( GL_ZERO, GL_SRC_COLOR );

	// --- Brush lightmaps (textured) ---
	gl().glActiveTexture( GL_TEXTURE0 );
	gl().glClientActiveTexture( GL_TEXTURE0 );
	gl().glEnable( GL_TEXTURE_2D );
	gl().glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

	gl().glEnable( GL_TEXTURE_GEN_S );
	gl().glEnable( GL_TEXTURE_GEN_T );
	gl().glTexGeni( GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );
	gl().glTexGeni( GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );

	for ( auto& [ node, cache ] : g_previewLighting.brushes )
	{
		if ( node != nullptr && !node->visible() ) {
			continue;
		}
		BrushInstance* brush = cache.instance;
		if ( brush == nullptr ) {
			continue;
		}

		brush->getBrush().evaluateBRep();
		const Matrix4& localToWorld = brush->localToWorld();

		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &localToWorld ) );

		std::size_t faceIndex = 0;
		Brush_ForEachFaceInstance( *brush, [&]( FaceInstance& faceInstance ){
			if ( faceIndex >= cache.faces.size() ) {
				++faceIndex;
				return;
			}

			Face& face = faceInstance.getFace();
			if ( !brush_face_receives_preview_lighting( node, face ) ) {
				++faceIndex;
				return;
			}

			FaceLightmap& lm = cache.faces[faceIndex++];
			if ( lm.texture == 0 ) {
				return;
			}

			const Winding& w = face.getWinding();
			if ( w.numpoints < 3 ) {
				return;
			}

			gl().glBindTexture( GL_TEXTURE_2D, lm.texture );

			const float planeS[4] = { lm.planeS.x(), lm.planeS.y(), lm.planeS.z(), lm.planeS.w() };
			const float planeT[4] = { lm.planeT.x(), lm.planeT.y(), lm.planeT.z(), lm.planeT.w() };
			gl().glTexGenfv( GL_S, GL_OBJECT_PLANE, planeS );
			gl().glTexGenfv( GL_T, GL_OBJECT_PLANE, planeT );

			gl().glVertexPointer( 3, GL_DOUBLE, sizeof( WindingVertex ), &w.points.data()->vertex );
			gl().glDrawArrays( GL_POLYGON, 0, GLsizei( w.numpoints ) );
		} );

		gl().glPopMatrix();
	}

	gl().glDisable( GL_TEXTURE_GEN_S );
	gl().glDisable( GL_TEXTURE_GEN_T );

	// --- Patch lighting (vertex colours) ---
	gl().glDisable( GL_TEXTURE_2D );
	gl().glEnableClientState( GL_COLOR_ARRAY );

	for ( auto& [ node, cache ] : g_previewLighting.patches )
	{
		if ( node != nullptr && !node->visible() ) {
			continue;
		}
		PatchInstance* patch = cache.instance;
		if ( patch == nullptr ) {
			continue;
		}

		Patch& patchRef = patch->getPatch();
		if ( !patch_receives_preview_lighting( node, patchRef ) ) {
			continue;
		}
		const PatchTesselation& tess = patchRef.getTesselation();
		if ( tess.m_vertices.empty() ) {
			continue;
		}
		if ( cache.coloursRGBA.size() != tess.m_vertices.size() * 4 ) {
			continue;
		}

		const Matrix4& localToWorld = patch->localToWorld();
		gl().glPushMatrix();
		gl().glMultMatrixf( reinterpret_cast<const float*>( &localToWorld ) );

		gl().glVertexPointer( 3, GL_FLOAT, sizeof( ArbitraryMeshVertex ), &tess.m_vertices.data()->vertex );
		gl().glColorPointer( 4, GL_UNSIGNED_BYTE, 0, cache.coloursRGBA.data() );

		const RenderIndex* strip_indices = tess.m_indices.data();
		for ( std::size_t i = 0; i < tess.m_numStrips; ++i, strip_indices += tess.m_lenStrips )
		{
			gl().glDrawElements( GL_QUAD_STRIP, GLsizei( tess.m_lenStrips ), RenderIndexTypeID, strip_indices );
		}

		gl().glPopMatrix();
	}

	gl().glDisableClientState( GL_COLOR_ARRAY );

	gl().glDisable( GL_BLEND );
	gl().glDepthMask( GL_TRUE );
}

} // namespace preview_lighting_impl

void preview_lighting_mark_dirty(){
	g_previewLighting.sceneDirty = true;
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
	if ( enable ) {
		g_previewLighting.sceneDirty = true;
	}
}

void PreviewLighting_SetModel( int model ){
	if ( model < 0 || model >= PREVIEW_LIGHTING_MODEL_COUNT ) {
		model = PREVIEW_LIGHTING_MODEL_BAKED_OVERLAY;
	}

	if ( g_previewLighting.model == model ) {
		return;
	}

	g_previewLighting.model = model;
	g_previewLighting.sceneDirty = true;
}

int PreviewLighting_GetModel(){
	return g_previewLighting.model;
}

void PreviewLighting_UpdateIfNeeded(){
	if ( game_is_doom3() ) {
		return;
	}
	preview_lighting_impl::update();
}

void PreviewLighting_RenderOverlay(){
	if ( game_is_doom3() ) {
		return;
	}
	preview_lighting_impl::render_overlay();
}
