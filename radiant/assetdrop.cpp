#include "assetdrop.h"

#include <cmath>
#include <limits>

#include "entity.h"
#include "brush.h"
#include "brushmanip.h"
#include "eclasslib.h"
#include "ientity.h"
#include "ieclass.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "iundo.h"
#include "map.h"
#include "math/aabb.h"
#include "math/vector.h"
#include "grid.h"
#include "filterbar.h"
#include "string/string.h"

const char* const kEntityBrowserMimeType = "application/x-viberadiant-entityclass";
const char* const kSoundBrowserMimeType = "application/x-viberadiant-soundpath";
const char* const kTextureBrowserMimeType = "application/x-viberadiant-texture";
const char* const kModelBrowserMimeType = "application/x-viberadiant-modelpath";

namespace {
class EntityAtPointFinder final : public scene::Graph::Walker
{
	const Vector3& m_point;
	mutable Entity* m_bestEntity = nullptr;
	mutable float m_bestDistance2 = std::numeric_limits<float>::max();

public:
	explicit EntityAtPointFinder( const Vector3& point )
		: m_point( point ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( !Node_isEntity( path.top() ) ) {
			return true;
		}

		Entity* entity = Node_getEntity( path.top() );
		if ( entity == nullptr ) {
			return false;
		}

		if ( classname_equal( entity->getClassName(), "worldspawn" ) ) {
			return false;
		}

		AABB bounds = instance.worldAABB();
		const float margin = std::max( 8.0f, GetGridSize() );
		bounds.extents += Vector3( margin, margin, margin );

		if ( !aabb_intersects_point( bounds, m_point ) ) {
			return false;
		}

		const float distance2 = vector3_length_squared( m_point - bounds.origin );
		if ( distance2 < m_bestDistance2 ) {
			m_bestDistance2 = distance2;
			m_bestEntity = entity;
		}

		return false;
	}

	Entity* bestEntity() const {
		return m_bestEntity;
	}
};

Entity* findEntityAtPoint( const Vector3& point ){
	EntityAtPointFinder finder( point );
	GlobalSceneGraph().traverse( finder );
	return finder.bestEntity();
}

BrushInstance* findBrushAtPoint( const Vector3& point ){
	BrushInstance* bestBrush = nullptr;
	float bestDistance2 = std::numeric_limits<float>::max();

	Scene_forEachVisibleBrush( GlobalSceneGraph(), [&]( BrushInstance& brush ){
		AABB bounds = brush.worldAABB();
		const float margin = std::max( 8.0f, GetGridSize() );
		bounds.extents += Vector3( margin, margin, margin );
		if ( !aabb_intersects_point( bounds, point ) ) {
			return;
		}

		const float distance2 = vector3_length_squared( point - bounds.origin );
		if ( distance2 < bestDistance2 ) {
			bestDistance2 = distance2;
			bestBrush = &brush;
		}
	} );

	return bestBrush;
}

bool selectWorldBrushAtPoint( const Vector3& point ){
	BrushInstance* brush = findBrushAtPoint( point );
	if ( brush == nullptr ) {
		return false;
	}
	Entity* entity = Node_getEntity( brush->path().parent() );
	if ( entity == nullptr || !classname_equal( entity->getClassName(), "worldspawn" ) ) {
		return false;
	}
	GlobalSelectionSystem().setSelectedAll( false );
	selectPath( brush->path(), true );
	return true;
}

Vector3 snappedPoint( const Vector3& point ){
	Vector3 snapped = point;
	vector3_snap( snapped, GetSnapGridSize() );
	return snapped;
}

void applyShaderToBrush( Brush& brush, const char* shader ){
	for ( const auto& face : brush ) {
		face->SetShader( shader );
	}
}

 bool createTexturedBrushAtPoint( const Vector3& point, const char* shader, bool alignToSurfaceZ ){
	const Vector3 extents( 32.0f, 32.0f, 32.0f );
	Vector3 origin = point;
	if ( alignToSurfaceZ ) {
		origin.z() += extents.z();
	}
	const AABB bounds( origin, extents );

	scene::Node* node = Scene_BrushCreate_Cuboid( bounds, shader );
	if ( node == nullptr ) {
		return false;
	}

	scene::Node& worldspawn = Map_FindOrInsertWorldspawn( g_map );

	scene::Path brushpath( makeReference( GlobalSceneGraph().root() ) );
	brushpath.push( makeReference( worldspawn ) );
	brushpath.push( makeReference( *node ) );

	GlobalSelectionSystem().setSelectedAll( false );
	selectPath( brushpath, true );
	return true;
}

bool createTargetSpeakerAtPoint( const Vector3& point, const char* soundPath ){
	EntityClass* entityClass = GlobalEntityClassManager().findOrInsert( "target_speaker", true );
	if ( entityClass == nullptr ) {
		return false;
	}

	NodeSmartReference node( GlobalEntityCreator().createEntity( entityClass ) );
	Node_getTraversable( GlobalSceneGraph().root() )->insert( node );

	scene::Path entitypath( makeReference( GlobalSceneGraph().root() ) );
	entitypath.push( makeReference( node.get() ) );
	scene::Instance& instance = findInstance( entitypath );

	if ( Transformable* transform = Instance_getTransformable( instance ) ) {
		transform->setType( TRANSFORM_PRIMITIVE );
		transform->setTranslation( point );
		transform->freezeTransform();

		const AABB bounds = instance.worldAABB();
		const float boundsMinZ = bounds.origin.z() - bounds.extents.z();
		const float deltaZ = point.z() - boundsMinZ;
		if ( std::isfinite( deltaZ ) && std::fabs( deltaZ ) > 1e-4f ) {
			Vector3 placed = point;
			placed.z() += deltaZ;
			transform->setTranslation( placed );
			transform->freezeTransform();
		}
	}

	if ( Entity* entity = Node_getEntity( node ) ) {
		entity->setKeyValue( "noise", soundPath );
	}

	GlobalSelectionSystem().setSelectedAll( false );
	Instance_setSelected( instance, true );
	return true;
}
} // namespace

bool AssetDrop_handleEntityClass( const char* classname, const Vector3& point ){
	if ( string_empty( classname ) ) {
		return false;
	}

	const Vector3 snapped = snappedPoint( point );
	EntityClass* entityClass = GlobalEntityClassManager().findOrInsert( classname, true );
	if ( entityClass != nullptr && !entityClass->fixedsize && !entityClass->miscmodel_is ) {
		bool createdBrush = false;
		if ( !selectWorldBrushAtPoint( snapped ) ) {
			const CopiedString shader = GetCommonShader( "notex" );
			if ( !createTexturedBrushAtPoint( snapped, shader.c_str(), true ) ) {
				return false;
			}
			createdBrush = true;
		}
		Entity_createFromSelection( classname, snapped, true );
		if ( createdBrush && string_equal_nocase_n( classname, "trigger_", 8 ) ) {
			const CopiedString shader = GetCommonShader( "notex" );
			Scene_forEachSelectedBrush( [&]( BrushInstance& brush ){
				applyShaderToBrush( brush.getBrush(), shader.c_str() );
			} );
		}
		return true;
	}

	Entity_createFromSelection( classname, snapped, true );
	return true;
}

bool AssetDrop_handleSoundPath( const char* soundPath, const Vector3& point ){
	if ( string_empty( soundPath ) ) {
		return false;
	}

	const Vector3 snapped = snappedPoint( point );
	UndoableCommand undo( "entityAssignSound" );

	if ( Entity* entity = findEntityAtPoint( snapped ) ) {
		entity->setKeyValue( "noise", soundPath );
		return true;
	}

	return createTargetSpeakerAtPoint( snapped, soundPath );
}

bool AssetDrop_handleTexture( const char* shader, const Vector3& point ){
	if ( string_empty( shader ) ) {
		return false;
	}

	const Vector3 snapped = snappedPoint( point );
	UndoableCommand undo( "textureDrop" );

	if ( BrushInstance* brush = findBrushAtPoint( snapped ) ) {
		applyShaderToBrush( brush->getBrush(), shader );
		return true;
	}

	return createTexturedBrushAtPoint( snapped, shader, false );
}

bool AssetDrop_handleModelPath( const char* modelPath, const Vector3& point ){
	if ( string_empty( modelPath ) ) {
		return false;
	}

	const Vector3 snapped = snappedPoint( point );
	UndoableCommand undo( "insertModel" );

	EntityClass* entityClass = GlobalEntityClassManager().findOrInsert( "misc_model", false );
	if ( entityClass == nullptr ) {
		return false;
	}

	NodeSmartReference node( GlobalEntityCreator().createEntity( entityClass ) );
	Node_getTraversable( GlobalSceneGraph().root() )->insert( node );

	scene::Path entitypath( makeReference( GlobalSceneGraph().root() ) );
	entitypath.push( makeReference( node.get() ) );
	scene::Instance& instance = findInstance( entitypath );

	if ( Entity* entity = Node_getEntity( node ) ) {
		entity->setKeyValue( entityClass->miscmodel_key(), modelPath );
	}

	if ( Transformable* transform = Instance_getTransformable( instance ) ) {
		transform->setType( TRANSFORM_PRIMITIVE );
		transform->setTranslation( snapped );
		transform->freezeTransform();

		const AABB bounds = instance.worldAABB();
		const float boundsMinZ = bounds.origin.z() - bounds.extents.z();
		const float originToMinZ = snapped.z() - boundsMinZ;
		if ( std::isfinite( originToMinZ ) && std::fabs( originToMinZ ) > 1e-4f ) {
			Vector3 placed = snapped;
			placed.z() += originToMinZ;
			transform->setTranslation( placed );
			transform->freezeTransform();
		}
	}

	GlobalSelectionSystem().setSelectedAll( false );
	Instance_setSelected( instance, true );
	return true;
}
