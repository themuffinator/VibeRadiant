/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "select.h"

#include "debugging/debugging.h"

#include "ientity.h"
#include "iselection.h"
#include "iundo.h"
#include "linkedgroups.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "stream/stringstream.h"
#include "signal/isignal.h"
#include "shaderlib.h"
#include "scenelib.h"
#include "string/string.h"
#include "os/path.h"

#include "gtkutil/idledraw.h"
#include "gtkutil/dialog.h"
#include "gtkutil/widget.h"
#include "gtkutil/clipboard.h"
#include "brushmanip.h"
#include "brush.h"
#include "patch.h"
#include "patchmanip.h"
#include "patchdialog.h"
#include "surfacedialog.h"
#include "texwindow.h"
#include "mainframe.h"
#include "camwindow.h"
#include "tools.h"
#include "grid.h"
#include "map.h"
#include "csg.h"



select_workzone_t g_select_workzone;


/**
   Loops over all selected brushes and stores their
   world AABBs in the specified array.
 */
class CollectSelectedBrushesBounds : public SelectionSystem::Visitor
{
	AABB* m_bounds;     // array of AABBs
	Unsigned m_max;     // max AABB-elements in array
	Unsigned& m_count;  // count of valid AABBs stored in array

public:
	CollectSelectedBrushesBounds( AABB* bounds, Unsigned max, Unsigned& count ) :
		m_bounds( bounds ),
		m_max( max ),
		m_count( count ){
		m_count = 0;
	}

	void visit( scene::Instance& instance ) const override {
		ASSERT_MESSAGE( m_count <= m_max, "Invalid m_count in CollectSelectedBrushesBounds" );

		// stop if the array is already full
		if ( m_count == m_max ) {
			return;
		}

		if ( Instance_isSelected( instance ) ) {
			// brushes only
			if ( Instance_getBrush( instance ) != 0 ) {
				m_bounds[m_count] = instance.worldAABB();
				++m_count;
			}
		}
	}
};

/**
   Selects all objects that intersect one of the bounding AABBs.
   The exact intersection-method is specified through TSelectionPolicy
 */
template<class TSelectionPolicy>
class SelectByBounds : public scene::Graph::Walker
{
	AABB* m_aabbs;             // selection aabbs
	Unsigned m_count;          // number of aabbs in m_aabbs
	TSelectionPolicy policy;   // type that contains a custom intersection method aabb<->aabb

public:
	SelectByBounds( AABB* aabbs, Unsigned count ) :
		m_aabbs( aabbs ),
		m_count( count ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( path.top().get().visible() ){
			Selectable* selectable = Instance_getSelectable( instance );

			// ignore worldspawn
			Entity* entity = Node_getEntity( path.top() );
			if ( entity != nullptr && string_equal( entity->getClassName(), "worldspawn" ) ) {
				return true;
			}

			if ( path.size() > 1
			  && !path.top().get().isRoot()
			  && selectable != 0
			  && !node_is_group( path.top() ) ) {
				for ( Unsigned i = 0; i < m_count; ++i )
				{
					if ( policy.Evaluate( m_aabbs[i], instance ) ) {
						selectable->setSelected( true );
					}
				}
			}
		}
		else{
			return false;
		}

		return true;
	}

	/**
	   Performs selection operation on the global scenegraph.
	   If delete_bounds_src is true, then the objects which were
	   used as source for the selection aabbs will be deleted.
	 */
	static void DoSelection( bool delete_bounds_src = true ){
		if ( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive ) {
			// we may not need all AABBs since not all selected objects have to be brushes
			const Unsigned max = (Unsigned)GlobalSelectionSystem().countSelected();
			AABB* aabbs = new AABB[max];

			Unsigned count;
			CollectSelectedBrushesBounds collector( aabbs, max, count );
			GlobalSelectionSystem().foreachSelected( collector );

			// nothing usable in selection
			if ( !count ) {
				delete[] aabbs;
				return;
			}

			// delete selected objects
			if ( delete_bounds_src ) { // see deleteSelection
				UndoableCommand undo( "deleteSelected" );
				Select_Delete();
			}

			// select objects with bounds
			GlobalSceneGraph().traverse( SelectByBounds<TSelectionPolicy>( aabbs, count ) );

			SceneChangeNotify();
			delete[] aabbs;
		}
	}
};

/**
   SelectionPolicy for SelectByBounds
   Returns true if box and the AABB of instance intersect
 */
class SelectionPolicy_Touching
{
public:
	bool Evaluate( const AABB& box, scene::Instance& instance ) const {
		const AABB& other( instance.worldAABB() );
		for ( Unsigned i = 0; i < 3; ++i )
		{
			if ( std::fabs( box.origin[i] - other.origin[i] ) > ( box.extents[i] + other.extents[i] ) ) {
				return false;
			}
		}
		return true;
	}
};

/**
   SelectionPolicy for SelectByBounds
   Returns true if the AABB of instance is inside box
 */
class SelectionPolicy_Inside
{
public:
	bool Evaluate( const AABB& box, scene::Instance& instance ) const {
		const AABB& other( instance.worldAABB() );
		for ( Unsigned i = 0; i < 3; ++i )
		{
			if ( std::fabs( box.origin[i] - other.origin[i] ) > ( box.extents[i] - other.extents[i] ) ) {
				return false;
			}
		}
		return true;
	}
};

/**
   SelectionPolicy for SelectByBounds
   Returns true if box and the AABB of instance intersect in 2D (height ignored)
 */
class SelectionPolicy_TouchingTall
{
public:
	bool Evaluate( const AABB& box, scene::Instance& instance ) const {
		const AABB& other( instance.worldAABB() );
		for ( Unsigned i = 0; i < 2; ++i )
		{
			if ( std::fabs( box.origin[i] - other.origin[i] ) > ( box.extents[i] + other.extents[i] ) ) {
				return false;
			}
		}
		return true;
	}
};

class DeleteSelected : public scene::Graph::Walker
{
	mutable bool m_remove;
	mutable bool m_removedChild;
public:
	DeleteSelected()
		: m_remove( false ), m_removedChild( false ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		m_removedChild = false;

		if ( Instance_isSelected( instance )
		     && path.size() > 1
		     && !path.top().get().isRoot() ) {
			m_remove = true;

			return false; // dont traverse into child elements
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {

		if ( m_removedChild ) {
			m_removedChild = false;

			// delete empty entities
			if ( Node_isEntity( path.top() )
			     && path.top().get_pointer() != Map_FindWorldspawn( g_map ) // direct worldspawn deletion is permitted, so do find it each time
			     && Node_getTraversable( path.top() )->empty() ) {
				Path_deleteTop( path );
			}
		}

		// node should be removed
		if ( m_remove ) {
			if ( Node_isEntity( path.parent() ) ) {
				m_removedChild = true;
			}

			m_remove = false;
			Path_deleteTop( path );
		}
	}
};

void Scene_DeleteSelected( scene::Graph& graph ){
	graph.traverse( DeleteSelected() );
	SceneChangeNotify();
}

void Select_Delete(){
	Scene_DeleteSelected( GlobalSceneGraph() );
}

class InvertSelectionWalker : public scene::Graph::Walker
{
	SelectionSystem::EMode m_mode;
	SelectionSystem::EComponentMode m_compmode;
	mutable Selectable* m_selectable;
public:
	InvertSelectionWalker( SelectionSystem::EMode mode, SelectionSystem::EComponentMode compmode )
		: m_mode( mode ), m_compmode( compmode ), m_selectable( 0 ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( !path.top().get().visible() ){
			m_selectable = 0;
			return false;
		}
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable ) {
			switch ( m_mode )
			{
			case SelectionSystem::eEntity:
				if ( Node_isEntity( path.top() ) != 0 ) {
					m_selectable = path.top().get().visible() ? selectable : 0;
				}
				break;
			case SelectionSystem::ePrimitive:
				m_selectable = path.top().get().visible() ? selectable : 0;
				break;
			case SelectionSystem::eComponent:
				BrushInstance* brushinstance = Instance_getBrush( instance );
				if( brushinstance != 0 ){
					if( brushinstance->isSelected() )
						brushinstance->invertComponentSelection( m_compmode );
				}
				else{
					PatchInstance* patchinstance = Instance_getPatch( instance );
					if( patchinstance != 0 && m_compmode == SelectionSystem::eVertex ){
						if( patchinstance->isSelected() )
							patchinstance->invertComponentSelection();
					}
				}
				break;
			}
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		if ( m_selectable != 0 ) {
			m_selectable->setSelected( !m_selectable->isSelected() );
			m_selectable = 0;
		}
	}
};

void Scene_Invert_Selection( scene::Graph& graph ){
	graph.traverse( InvertSelectionWalker( GlobalSelectionSystem().Mode(), GlobalSelectionSystem().ComponentMode() ) );
}

void Select_Invert(){
	Scene_Invert_Selection( GlobalSceneGraph() );
}

#if 0
//interesting printings
class ExpandSelectionToEntitiesWalker_dbg : public scene::Graph::Walker
{
	mutable std::size_t m_depth = 0;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const {
		++m_depth;
		globalOutputStream() << "pre depth_" << m_depth;
		globalOutputStream() << " path.size()_" << path.size();
		if ( path.top().get_pointer() == m_world )
			globalOutputStream() << " worldspawn";
		if( path.top().get().isRoot() )
			globalOutputStream() << " path.top().get().isRoot()";
		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ){
			globalOutputStream() << " entity!=0";
			if( entity->isContainer() ){
				globalOutputStream() << " entity->isContainer()";
			}
			globalOutputStream() << " classname_" << entity->getKeyValue( "classname" );
		}
		globalOutputStream() << '\n';
//	globalOutputStream() << "" <<  ;
//	globalOutputStream() << "" <<  ;
//	globalOutputStream() << "" <<  ;
//	globalOutputStream() << "" <<  ;
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const {
		globalOutputStream() << "post depth_" << m_depth;
		globalOutputStream() << " path.size()_" << path.size();
		if ( path.top().get_pointer() == m_world )
			globalOutputStream() << " worldspawn";
		if( path.top().get().isRoot() )
			globalOutputStream() << " path.top().get().isRoot()";
		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ){
			globalOutputStream() << " entity!=0";
			if( entity->isContainer() ){
				globalOutputStream() << " entity->isContainer()";
			}
			globalOutputStream() << " classname_" << entity->getKeyValue( "classname" );
		}
		globalOutputStream() << '\n';
		--m_depth;
	}
};
#endif

class ExpandSelectionToPrimitivesWalker : public scene::Graph::Walker
{
	mutable std::size_t m_depth = 0;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		++m_depth;

		if( !path.top().get().visible() )
			return false;

//		if ( path.top().get_pointer() == m_world ) // ignore worldspawn
//			return false;

		if ( m_depth == 2 ) { // entity depth
			// traverse and select children if any one is selected
			bool beselected = false;
			const bool isContainer = Node_getEntity( path.top() )->isContainer();
			if ( instance.childSelected() || instance.isSelected() ) {
				beselected = true;
				Instance_setSelected( instance, !isContainer );
			}
			return isContainer && beselected;
		}
		else if ( m_depth == 3 ) { // primitive depth
			Instance_setSelected( instance, true );
			return false;
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		--m_depth;
	}
};

void Scene_ExpandSelectionToPrimitives(){
	GlobalSceneGraph().traverse( ExpandSelectionToPrimitivesWalker() );
}

class ExpandSelectionToEntitiesWalker : public scene::Graph::Walker
{
	mutable std::size_t m_depth = 0;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		++m_depth;

		if( !path.top().get().visible() )
			return false;

//		if ( path.top().get_pointer() == m_world ) // ignore worldspawn
//			return false;

		if ( m_depth == 2 ) { // entity depth
			// traverse and select children if any one is selected
			bool beselected = false;
			if ( instance.childSelected() || instance.isSelected() ) {
				beselected = true;
				if( path.top().get_pointer() != m_world ){ //avoid selecting world node
					Instance_setSelected( instance, true );
				}
			}
			return Node_getEntity( path.top() )->isContainer() && beselected;
		}
		else if ( m_depth == 3 ) { // primitive depth
			Instance_setSelected( instance, true );
			return false;
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		--m_depth;
	}
};

void Scene_ExpandSelectionToEntities(){
	GlobalSceneGraph().traverse( ExpandSelectionToEntitiesWalker() );
}


namespace
{
void Selection_UpdateWorkzone(){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		Select_GetBounds( g_select_workzone.d_work_min, g_select_workzone.d_work_max );
	}
}
typedef FreeCaller<void(), Selection_UpdateWorkzone> SelectionUpdateWorkzoneCaller;

IdleDraw g_idleWorkzone = IdleDraw( SelectionUpdateWorkzoneCaller() );
}

const select_workzone_t& Select_getWorkZone(){
	g_idleWorkzone.flush();
	return g_select_workzone;
}

void UpdateWorkzone_ForSelection(){
	g_idleWorkzone.queueDraw();
}

// update the workzone to the current selection
void UpdateWorkzone_ForSelectionChanged( const Selectable& selectable ){
	//if ( selectable.isSelected() ) {
		UpdateWorkzone_ForSelection();
	//}
}

void Select_SetShader( const char* shader ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushSetShader_Selected( GlobalSceneGraph(), shader );
		Scene_PatchSetShader_Selected( GlobalSceneGraph(), shader );
	}
	Scene_BrushSetShader_Component_Selected( GlobalSceneGraph(), shader );
}

void Select_SetShader_Undo( const char* shader ){
	if ( GlobalSelectionSystem().countSelectedComponents() != 0 || GlobalSelectionSystem().countSelected() != 0 ) {
		UndoableCommand undo( "textureNameSetSelected" );
		Select_SetShader( shader );
	}
}

void Select_SetTexdef( const TextureProjection& projection, bool setBasis /*= true*/, bool resetBasis /*= false*/ ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushSetTexdef_Selected( GlobalSceneGraph(), projection, setBasis, resetBasis );
	}
	Scene_BrushSetTexdef_Component_Selected( GlobalSceneGraph(), projection, setBasis, resetBasis );
}

void Select_SetTexdef( const float* hShift, const float* vShift, const float* hScale, const float* vScale, const float* rotation ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushSetTexdef_Selected( GlobalSceneGraph(), hShift, vShift, hScale, vScale, rotation );
	}
	Scene_BrushSetTexdef_Component_Selected( GlobalSceneGraph(), hShift, vShift, hScale, vScale, rotation );
}

void Select_SetFlags( const ContentsFlagsValue& flags ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushSetFlags_Selected( GlobalSceneGraph(), flags );
	}
	Scene_BrushSetFlags_Component_Selected( GlobalSceneGraph(), flags );
}

void Select_GetBounds( Vector3& mins, Vector3& maxs ){
	const AABB bounds = GlobalSelectionSystem().getBoundsSelected();
	maxs = vector3_added( bounds.origin, bounds.extents );
	mins = vector3_subtracted( bounds.origin, bounds.extents );
}


void Select_FlipAxis( int axis ){
	Vector3 flip( 1, 1, 1 );
	flip[axis] = -1;
	GlobalSelectionSystem().scaleSelected( flip, true );
}


void Select_Scale( float x, float y, float z ){
	GlobalSelectionSystem().scaleSelected( Vector3( x, y, z ) );
}

enum axis_t
{
	eAxisX = 0,
	eAxisY = 1,
	eAxisZ = 2,
};

enum sign_t
{
	eSignPositive = 1,
	eSignNegative = -1,
};

inline Matrix4 matrix4_rotation_for_axis90( axis_t axis, sign_t sign ){
	switch ( axis )
	{
	case eAxisX:
		if ( sign == eSignPositive ) {
			return matrix4_rotation_for_sincos_x( 1, 0 );
		}
		else
		{
			return matrix4_rotation_for_sincos_x( -1, 0 );
		}
	case eAxisY:
		if ( sign == eSignPositive ) {
			return matrix4_rotation_for_sincos_y( 1, 0 );
		}
		else
		{
			return matrix4_rotation_for_sincos_y( -1, 0 );
		}
	default: //case eAxisZ:
		if ( sign == eSignPositive ) {
			return matrix4_rotation_for_sincos_z( 1, 0 );
		}
		else
		{
			return matrix4_rotation_for_sincos_z( -1, 0 );
		}
	}
}

inline void matrix4_rotate_by_axis90( Matrix4& matrix, axis_t axis, sign_t sign ){
	matrix4_multiply_by_matrix4( matrix, matrix4_rotation_for_axis90( axis, sign ) );
}

inline void matrix4_pivoted_rotate_by_axis90( Matrix4& matrix, axis_t axis, sign_t sign, const Vector3& pivotpoint ){
	matrix4_translate_by_vec3( matrix, pivotpoint );
	matrix4_rotate_by_axis90( matrix, axis, sign );
	matrix4_translate_by_vec3( matrix, vector3_negated( pivotpoint ) );
}

inline Quaternion quaternion_for_axis90( axis_t axis, sign_t sign ){
#if 1
	switch ( axis )
	{
	case eAxisX:
		if ( sign == eSignPositive ) {
			return Quaternion( c_half_sqrt2f, 0, 0, c_half_sqrt2f );
		}
		else
		{
			return Quaternion( -c_half_sqrt2f, 0, 0, -c_half_sqrt2f );
		}
	case eAxisY:
		if ( sign == eSignPositive ) {
			return Quaternion( 0, c_half_sqrt2f, 0, c_half_sqrt2f );
		}
		else
		{
			return Quaternion( 0, -c_half_sqrt2f, 0, -c_half_sqrt2f );
		}
	default: //case eAxisZ:
		if ( sign == eSignPositive ) {
			return Quaternion( 0, 0, c_half_sqrt2f, c_half_sqrt2f );
		}
		else
		{
			return Quaternion( 0, 0, -c_half_sqrt2f, -c_half_sqrt2f );
		}
	}
#else
	quaternion_for_matrix4_rotation( matrix4_rotation_for_axis90( (axis_t)axis, ( deg > 0 ) ? eSignPositive : eSignNegative ) );
#endif
}

void Select_RotateAxis( int axis, float deg ){
	if ( std::fabs( deg ) == 90.f ) {
		GlobalSelectionSystem().rotateSelected( quaternion_for_axis90( (axis_t)axis, ( deg > 0 ) ? eSignPositive : eSignNegative ), true );
	}
	else
	{
		switch ( axis )
		{
		case 0:
			GlobalSelectionSystem().rotateSelected( quaternion_for_matrix4_rotation( matrix4_rotation_for_x_degrees( deg ) ) );
			break;
		case 1:
			GlobalSelectionSystem().rotateSelected( quaternion_for_matrix4_rotation( matrix4_rotation_for_y_degrees( deg ) ) );
			break;
		case 2:
			GlobalSelectionSystem().rotateSelected( quaternion_for_matrix4_rotation( matrix4_rotation_for_z_degrees( deg ) ) );
			break;
		}
	}
}


void Select_ShiftTexture( float x, float y ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushShiftTexdef_Selected( GlobalSceneGraph(), x, y );
		Scene_PatchTranslateTexture_Selected( GlobalSceneGraph(), x, y );
	}
	//globalOutputStream() << "shift selected face textures: s=" << x << " t=" << y << '\n';
	Scene_BrushShiftTexdef_Component_Selected( GlobalSceneGraph(), x, y );
}

void Select_ScaleTexture( float x, float y ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushScaleTexdef_Selected( GlobalSceneGraph(), x, y );
		Scene_PatchScaleTexture_Selected( GlobalSceneGraph(), x, y );
	}
	Scene_BrushScaleTexdef_Component_Selected( GlobalSceneGraph(), x, y );
}

void Select_RotateTexture( float amt ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushRotateTexdef_Selected( GlobalSceneGraph(), amt );
		Scene_PatchRotateTexture_Selected( GlobalSceneGraph(), amt );
	}
	Scene_BrushRotateTexdef_Component_Selected( GlobalSceneGraph(), amt );
}

namespace
{

struct TextureFindReplaceState
{
	TextureFindMatchMode matchMode = TextureFindMatchMode::Exact;
	TextureReplaceMode replaceMode = TextureReplaceMode::ReplaceFull;
	TextureFindScope scope = TextureFindScope::All;
	TextureShaderFilter shaderFilter = TextureShaderFilter::Any;
	TextureUsageFilter usageFilter = TextureUsageFilter::Any;
	bool caseSensitive = false;
	bool matchNameOnly = false;
	bool autoPrefix = true;
	bool visibleOnly = true;
	bool includeBrushes = true;
	bool includePatches = true;
	int minWidth = 0;
	int maxWidth = 0;
	int minHeight = 0;
	int maxHeight = 0;
	unsigned int surfaceFlagsRequire = 0;
	unsigned int surfaceFlagsExclude = 0;
	unsigned int contentFlagsRequire = 0;
	unsigned int contentFlagsExclude = 0;
	bool useSurfaceFlagsRequire = false;
	bool useSurfaceFlagsExclude = false;
	bool useContentFlagsRequire = false;
	bool useContentFlagsExclude = false;
	bool useShaderFilters = false;
	std::string findPattern;
	std::string replaceRaw;
	std::string replaceFull;
	std::vector<std::string> includeFilters;
	std::vector<std::string> excludeFilters;
	std::regex regex;
	bool regexReady = false;
	bool doReplace = false;
};

struct FindReplacePatternState
{
	TextureFindMatchMode matchMode = TextureFindMatchMode::Exact;
	TextureReplaceMode replaceMode = TextureReplaceMode::ReplaceFull;
	bool caseSensitive = false;
	std::string findPattern;
	std::string replaceRaw;
	std::regex regex;
	bool regexReady = false;
};

struct EntityFindReplaceState
{
	FindReplacePatternState pattern;
	EntityFindScope scope = EntityFindScope::All;
	bool visibleOnly = true;
	bool searchKeys = false;
	bool searchValues = true;
	bool replaceKeys = false;
	bool replaceValues = true;
	bool includeWorldspawn = false;
	bool doReplace = false;
	std::vector<std::string> keyFilters;
	std::vector<std::string> classFilters;
};

struct ShaderParts
{
	std::string full;
	std::string prefix;
	std::string leaf;
};

struct MatchResult
{
	bool matched = false;
	bool replacementValid = false;
	std::string replacement;
};

struct ShaderInfo
{
	bool isDefault = false;
	bool inUse = false;
	unsigned int surfaceFlags = 0;
	unsigned int contentFlags = 0;
	int width = 0;
	int height = 0;
};

class ShaderInfoCache
{
	std::unordered_map<std::string, ShaderInfo> m_cache;
public:
	const ShaderInfo& get( const char* shaderName ){
		const std::string key = shaderName ? shaderName : "";
		auto [it, inserted] = m_cache.emplace( key, ShaderInfo{} );
		if ( !inserted ) {
			return it->second;
		}
		ShaderInfo& info = it->second;
		if ( shaderName != nullptr && *shaderName != '\0' ) {
			if ( IShader* shader = QERApp_Shader_ForName( shaderName ) ) {
				info.isDefault = shader->IsDefault();
				info.inUse = shader->IsInUse();
				if ( qtexture_t* texture = shader->getTexture() ) {
					info.surfaceFlags = texture->surfaceFlags;
					info.contentFlags = texture->contentFlags;
					info.width = static_cast<int>( texture->width );
					info.height = static_cast<int>( texture->height );
				}
			}
		}
		return info;
	}
};

static std::string TrimAscii( const std::string& text ){
	std::size_t start = 0;
	while ( start < text.size() && std::isspace( static_cast<unsigned char>( text[start] ) ) ) {
		++start;
	}
	std::size_t end = text.size();
	while ( end > start && std::isspace( static_cast<unsigned char>( text[end - 1] ) ) ) {
		--end;
	}
	return text.substr( start, end - start );
}

static std::string ToLowerCopy( const std::string& text ){
	std::string out = text;
	std::transform( out.begin(), out.end(), out.begin(), []( unsigned char ch ){
		return static_cast<char>( std::tolower( ch ) );
	} );
	return out;
}

static std::string CleanShaderPath( const std::string& text ){
	if ( text.empty() ) {
		return text;
	}
	const auto cleaned = StringStream<256>( PathCleaned( text.c_str() ) );
	return std::string( cleaned.c_str() );
}

static std::string NormalizeFullPath( const std::string& text, bool autoPrefix ){
	std::string trimmed = TrimAscii( text );
	if ( trimmed.empty() ) {
		return trimmed;
	}
	if ( autoPrefix && !shader_equal_prefix( trimmed.c_str(), GlobalTexturePrefix_get() ) ) {
		return std::string( GlobalTexturePrefix_get() ) + trimmed;
	}
	return trimmed;
}

static ShaderParts SplitShaderParts( const char* shader ){
	ShaderParts parts;
	if ( shader == nullptr ) {
		return parts;
	}
	parts.full = shader;
	const std::size_t slash = parts.full.find_last_of( '/' );
	if ( slash == std::string::npos ) {
		parts.leaf = parts.full;
		return parts;
	}
	parts.prefix = parts.full.substr( 0, slash + 1 );
	parts.leaf = parts.full.substr( slash + 1 );
	return parts;
}

static bool WildcardMatchInternal( const char* pattern, const char* text ){
	const char* star = nullptr;
	const char* starText = nullptr;

	while ( *text ) {
		if ( *pattern == '*' ) {
			star = pattern++;
			starText = text;
			continue;
		}
		if ( *pattern == '?' || *pattern == *text ) {
			++pattern;
			++text;
			continue;
		}
		if ( star ) {
			pattern = star + 1;
			text = ++starText;
			continue;
		}
		return false;
	}

	while ( *pattern == '*' ) {
		++pattern;
	}
	return *pattern == '\0';
}

static bool WildcardMatch( const std::string& pattern, const std::string& text, bool caseSensitive ){
	if ( caseSensitive ) {
		return WildcardMatchInternal( pattern.c_str(), text.c_str() );
	}
	const std::string foldedPattern = ToLowerCopy( pattern );
	const std::string foldedText = ToLowerCopy( text );
	return WildcardMatchInternal( foldedPattern.c_str(), foldedText.c_str() );
}

static bool WildcardMatchCaptureRecursive( const std::string& patternCmp, const std::string& textCmp, const std::string& textOriginal,
                                           std::size_t pIndex, std::size_t tIndex, std::vector<std::string>& captures, std::size_t captureIndex ){
	while ( pIndex < patternCmp.size() ) {
		const char pch = patternCmp[pIndex];
		if ( pch == '*' ) {
			if ( captureIndex >= captures.size() ) {
				return false;
			}
			for ( std::size_t i = tIndex; i <= textCmp.size(); ++i ) {
				captures[captureIndex] = textOriginal.substr( tIndex, i - tIndex );
				if ( WildcardMatchCaptureRecursive( patternCmp, textCmp, textOriginal, pIndex + 1, i, captures, captureIndex + 1 ) ) {
					return true;
				}
			}
			return false;
		}
		if ( tIndex >= textCmp.size() ) {
			return false;
		}
		if ( pch == '?' || pch == textCmp[tIndex] ) {
			++pIndex;
			++tIndex;
			continue;
		}
		return false;
	}
	return tIndex == textCmp.size();
}

static bool WildcardMatchCapture( const std::string& pattern, const std::string& text, bool caseSensitive, std::vector<std::string>& captures ){
	const std::size_t starCount = std::count( pattern.begin(), pattern.end(), '*' );
	captures.assign( starCount, std::string() );
	const std::string patternCmp = caseSensitive ? pattern : ToLowerCopy( pattern );
	const std::string textCmp = caseSensitive ? text : ToLowerCopy( text );
	return WildcardMatchCaptureRecursive( patternCmp, textCmp, text, 0, 0, captures, 0 );
}

static std::string ExpandWildcardReplacement( const std::string& replace, const std::vector<std::string>& captures ){
	std::string out;
	out.reserve( replace.size() );
	for ( std::size_t i = 0; i < replace.size(); ++i ) {
		const char ch = replace[i];
		if ( ch == '$' && i + 1 < replace.size() ) {
			const char next = replace[i + 1];
			if ( next == '$' ) {
				out.push_back( '$' );
				++i;
				continue;
			}
			if ( next >= '1' && next <= '9' ) {
				const std::size_t index = static_cast<std::size_t>( next - '1' );
				if ( index < captures.size() ) {
					out.append( captures[index] );
				}
				++i;
				continue;
			}
		}
		out.push_back( ch );
	}
	return out;
}

static std::vector<std::string> SplitFilterPatterns( const std::string& text ){
	std::vector<std::string> patterns;
	std::string current;
	for ( const char ch : text ) {
		if ( ch == ',' || ch == ';' || ch == '\n' || ch == '\r' ) {
			const std::string trimmed = TrimAscii( current );
			if ( !trimmed.empty() ) {
				patterns.push_back( CleanShaderPath( trimmed ) );
			}
			current.clear();
			continue;
		}
		current.push_back( ch );
	}
	const std::string trimmed = TrimAscii( current );
	if ( !trimmed.empty() ) {
		patterns.push_back( CleanShaderPath( trimmed ) );
	}
	return patterns;
}

static std::vector<std::string> SplitListPatterns( const std::string& text ){
	std::vector<std::string> patterns;
	std::string current;
	for ( const char ch : text ) {
		if ( ch == ',' || ch == ';' || ch == '\n' || ch == '\r' ) {
			const std::string trimmed = TrimAscii( current );
			if ( !trimmed.empty() ) {
				patterns.push_back( trimmed );
			}
			current.clear();
			continue;
		}
		current.push_back( ch );
	}
	const std::string trimmed = TrimAscii( current );
	if ( !trimmed.empty() ) {
		patterns.push_back( trimmed );
	}
	return patterns;
}

static bool ParseOptionalMask( const std::string& text, unsigned int& mask, bool& enabled, std::string& error, const char* label ){
	const std::string trimmed = TrimAscii( text );
	if ( trimmed.empty() ) {
		enabled = false;
		mask = 0;
		return true;
	}
	errno = 0;
	char* end = nullptr;
	const unsigned long value = std::strtoul( trimmed.c_str(), &end, 0 );
	if ( errno != 0 || end == trimmed.c_str() ) {
		error = StringStream<128>( "Invalid ", label, " mask" ).c_str();
		return false;
	}
	while ( *end != '\0' && std::isspace( static_cast<unsigned char>( *end ) ) ) {
		++end;
	}
	if ( *end != '\0' ) {
		error = StringStream<128>( "Invalid ", label, " mask" ).c_str();
		return false;
	}
	if ( value > std::numeric_limits<unsigned int>::max() ) {
		error = StringStream<128>( label, " mask is too large" ).c_str();
		return false;
	}
	mask = static_cast<unsigned int>( value );
	enabled = true;
	return true;
}

static bool MatchesAnyFilter( const std::vector<std::string>& filters, const std::string& value, bool caseSensitive ){
	return std::ranges::any_of( filters, [&]( const std::string& filter ){
		return WildcardMatch( filter, value, caseSensitive );
	} );
}

static bool PassesFilters( const TextureFindReplaceState& state, const std::string& shader ){
	if ( !state.includeFilters.empty() && !MatchesAnyFilter( state.includeFilters, shader, state.caseSensitive ) ) {
		return false;
	}
	if ( !state.excludeFilters.empty() && MatchesAnyFilter( state.excludeFilters, shader, state.caseSensitive ) ) {
		return false;
	}
	return true;
}

static bool PassesShaderFilters( const TextureFindReplaceState& state, const ShaderInfo& info ){
	if ( state.shaderFilter == TextureShaderFilter::DefaultOnly && !info.isDefault ) {
		return false;
	}
	if ( state.shaderFilter == TextureShaderFilter::RealOnly && info.isDefault ) {
		return false;
	}
	if ( state.usageFilter == TextureUsageFilter::InUseOnly && !info.inUse ) {
		return false;
	}
	if ( state.usageFilter == TextureUsageFilter::NotInUse && info.inUse ) {
		return false;
	}
	if ( ( state.minWidth > 0 || state.maxWidth > 0 ) && info.width <= 0 ) {
		return false;
	}
	if ( ( state.minHeight > 0 || state.maxHeight > 0 ) && info.height <= 0 ) {
		return false;
	}
	if ( state.minWidth > 0 && info.width < state.minWidth ) {
		return false;
	}
	if ( state.maxWidth > 0 && info.width > state.maxWidth ) {
		return false;
	}
	if ( state.minHeight > 0 && info.height < state.minHeight ) {
		return false;
	}
	if ( state.maxHeight > 0 && info.height > state.maxHeight ) {
		return false;
	}
	if ( state.useSurfaceFlagsRequire && ( info.surfaceFlags & state.surfaceFlagsRequire ) != state.surfaceFlagsRequire ) {
		return false;
	}
	if ( state.useSurfaceFlagsExclude && ( info.surfaceFlags & state.surfaceFlagsExclude ) != 0 ) {
		return false;
	}
	if ( state.useContentFlagsRequire && ( info.contentFlags & state.contentFlagsRequire ) != state.contentFlagsRequire ) {
		return false;
	}
	if ( state.useContentFlagsExclude && ( info.contentFlags & state.contentFlagsExclude ) != 0 ) {
		return false;
	}
	return true;
}

static bool BuildFindReplaceState( const TextureFindReplaceOptions& options, TextureFindReplaceState& state, std::string& error ){
	state.matchMode = options.matchMode;
	state.replaceMode = options.replaceMode;
	state.scope = options.scope;
	state.shaderFilter = options.shaderFilter;
	state.usageFilter = options.usageFilter;
	state.caseSensitive = options.caseSensitive;
	state.matchNameOnly = options.matchNameOnly;
	state.autoPrefix = options.autoPrefix;
	state.visibleOnly = options.visibleOnly;
	state.includeBrushes = options.includeBrushes;
	state.includePatches = options.includePatches;
	state.minWidth = std::max( 0, options.minWidth );
	state.maxWidth = std::max( 0, options.maxWidth );
	state.minHeight = std::max( 0, options.minHeight );
	state.maxHeight = std::max( 0, options.maxHeight );
	if ( state.minWidth > 0 && state.maxWidth > 0 && state.minWidth > state.maxWidth ) {
		error = "Invalid width range";
		return false;
	}
	if ( state.minHeight > 0 && state.maxHeight > 0 && state.minHeight > state.maxHeight ) {
		error = "Invalid height range";
		return false;
	}

	state.findPattern = TrimAscii( options.find );
	if ( state.matchMode != TextureFindMatchMode::Regex ) {
		state.findPattern = CleanShaderPath( state.findPattern );
	}
	if ( state.findPattern.empty() ) {
		error = "Find pattern is empty";
		return false;
	}
	if ( state.matchNameOnly ) {
		state.findPattern = SplitShaderParts( state.findPattern.c_str() ).leaf;
		if ( state.findPattern.empty() ) {
			error = "Find pattern is empty after trimming the path";
			return false;
		}
	}
	else {
		state.findPattern = NormalizeFullPath( state.findPattern, state.autoPrefix );
	}

	state.replaceRaw = TrimAscii( options.replace );
	state.doReplace = !state.replaceRaw.empty();
	if ( state.doReplace ) {
		state.replaceFull = NormalizeFullPath( state.replaceRaw, state.autoPrefix );
		state.replaceFull = CleanShaderPath( state.replaceFull );
	}

	state.includeFilters = SplitFilterPatterns( options.includeFilter );
	state.excludeFilters = SplitFilterPatterns( options.excludeFilter );
	if ( !ParseOptionalMask( options.surfaceFlagsRequire, state.surfaceFlagsRequire, state.useSurfaceFlagsRequire, error, "surface flags require" ) ) {
		return false;
	}
	if ( !ParseOptionalMask( options.surfaceFlagsExclude, state.surfaceFlagsExclude, state.useSurfaceFlagsExclude, error, "surface flags exclude" ) ) {
		return false;
	}
	if ( !ParseOptionalMask( options.contentFlagsRequire, state.contentFlagsRequire, state.useContentFlagsRequire, error, "content flags require" ) ) {
		return false;
	}
	if ( !ParseOptionalMask( options.contentFlagsExclude, state.contentFlagsExclude, state.useContentFlagsExclude, error, "content flags exclude" ) ) {
		return false;
	}
	state.useShaderFilters = ( state.shaderFilter != TextureShaderFilter::Any )
		|| ( state.usageFilter != TextureUsageFilter::Any )
		|| ( state.minWidth > 0 || state.maxWidth > 0 || state.minHeight > 0 || state.maxHeight > 0 )
		|| state.useSurfaceFlagsRequire || state.useSurfaceFlagsExclude || state.useContentFlagsRequire || state.useContentFlagsExclude;

	if ( state.matchMode == TextureFindMatchMode::Regex ) {
#if defined( __cpp_exceptions )
		try
		{
			const std::regex_constants::syntax_option_type regexOptions = state.caseSensitive
				? std::regex_constants::ECMAScript
				: ( std::regex_constants::ECMAScript | std::regex_constants::icase );
			state.regex = std::regex( state.findPattern, regexOptions );
			state.regexReady = true;
		}
		catch ( const std::regex_error& ) {
			error = "Invalid regex pattern";
			return false;
		}
#else
		error = "Regex matching requires exceptions";
		return false;
#endif
	}

	if ( state.matchMode == TextureFindMatchMode::Exact && !state.matchNameOnly ) {
		if ( !texdef_name_valid( state.findPattern.c_str() ) ) {
			error = StringStream<256>( "Invalid texture name: ", state.findPattern.c_str() ).c_str();
			return false;
		}
	}

	return true;
}

static bool BuildFindReplacePatternState( const std::string& find, const std::string& replace, TextureFindMatchMode matchMode,
                                          TextureReplaceMode replaceMode, bool caseSensitive, FindReplacePatternState& state, std::string& error ){
	state.matchMode = matchMode;
	state.replaceMode = replaceMode;
	state.caseSensitive = caseSensitive;
	state.findPattern = TrimAscii( find );
	if ( state.findPattern.empty() ) {
		error = "Find pattern is empty";
		return false;
	}
	state.replaceRaw = TrimAscii( replace );

	if ( state.matchMode == TextureFindMatchMode::Regex ) {
#if defined( __cpp_exceptions )
		try
		{
			const std::regex_constants::syntax_option_type regexOptions = state.caseSensitive
				? std::regex_constants::ECMAScript
				: ( std::regex_constants::ECMAScript | std::regex_constants::icase );
			state.regex = std::regex( state.findPattern, regexOptions );
			state.regexReady = true;
		}
		catch ( const std::regex_error& ) {
			error = "Invalid regex pattern";
			return false;
		}
#else
		error = "Regex matching requires exceptions";
		return false;
#endif
	}

	return true;
}

static bool BuildEntityFindReplaceState( const EntityFindReplaceOptions& options, EntityFindReplaceState& state, std::string& error ){
	if ( !BuildFindReplacePatternState( options.find, options.replace, options.matchMode, options.replaceMode, options.caseSensitive, state.pattern, error ) ) {
		return false;
	}
	state.scope = options.scope;
	state.visibleOnly = options.visibleOnly;
	state.searchKeys = options.searchKeys;
	state.searchValues = options.searchValues;
	state.replaceKeys = options.replaceKeys && state.searchKeys;
	state.replaceValues = options.replaceValues && state.searchValues;
	state.includeWorldspawn = options.includeWorldspawn;
	state.doReplace = !state.pattern.replaceRaw.empty();
	state.keyFilters = SplitListPatterns( options.keyFilter );
	state.classFilters = SplitListPatterns( options.classFilter );

	if ( !state.searchKeys && !state.searchValues ) {
		error = "No search fields enabled";
		return false;
	}
	if ( state.doReplace && !state.replaceKeys && !state.replaceValues ) {
		error = "No replacement fields enabled";
		return false;
	}
	return true;
}

static bool EntityKeyNameValid( const std::string& key ){
	if ( key.empty() ) {
		return false;
	}
	return key.find_first_of( " \n\r\t\v\"" ) == std::string::npos;
}

static bool MatchTarget( const TextureFindReplaceState& state, const std::string& target ){
	switch ( state.matchMode )
	{
	case TextureFindMatchMode::Exact:
		return state.caseSensitive
			? string_equal( target.c_str(), state.findPattern.c_str() )
			: string_equal_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::Contains:
		return state.caseSensitive
			? std::strstr( target.c_str(), state.findPattern.c_str() ) != nullptr
			: string_in_string_nocase( target.c_str(), state.findPattern.c_str() ) != nullptr;
	case TextureFindMatchMode::StartsWith:
		return state.caseSensitive
			? string_equal_prefix( target.c_str(), state.findPattern.c_str() )
			: string_equal_prefix_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::EndsWith:
		return state.caseSensitive
			? string_equal_suffix( target.c_str(), state.findPattern.c_str() )
			: string_equal_suffix_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::Wildcard:
		return WildcardMatch( state.findPattern, target, state.caseSensitive );
	case TextureFindMatchMode::Regex:
		return state.regexReady && std::regex_search( target, state.regex );
	}
	return false;
}

static bool MatchTarget( const FindReplacePatternState& state, const std::string& target ){
	switch ( state.matchMode )
	{
	case TextureFindMatchMode::Exact:
		return state.caseSensitive
			? string_equal( target.c_str(), state.findPattern.c_str() )
			: string_equal_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::Contains:
		return state.caseSensitive
			? std::strstr( target.c_str(), state.findPattern.c_str() ) != nullptr
			: string_in_string_nocase( target.c_str(), state.findPattern.c_str() ) != nullptr;
	case TextureFindMatchMode::StartsWith:
		return state.caseSensitive
			? string_equal_prefix( target.c_str(), state.findPattern.c_str() )
			: string_equal_prefix_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::EndsWith:
		return state.caseSensitive
			? string_equal_suffix( target.c_str(), state.findPattern.c_str() )
			: string_equal_suffix_nocase( target.c_str(), state.findPattern.c_str() );
	case TextureFindMatchMode::Wildcard:
		return WildcardMatch( state.findPattern, target, state.caseSensitive );
	case TextureFindMatchMode::Regex:
		return state.regexReady && std::regex_search( target, state.regex );
	}
	return false;
}

static std::string ReplaceAllCaseSensitive( const std::string& text, const std::string& find, const std::string& replace ){
	if ( find.empty() ) {
		return text;
	}
	std::string out;
	std::size_t pos = 0;
	while ( true ) {
		const std::size_t match = text.find( find, pos );
		if ( match == std::string::npos ) {
			out.append( text, pos, std::string::npos );
			break;
		}
		out.append( text, pos, match - pos );
		out.append( replace );
		pos = match + find.size();
	}
	return out;
}

static std::string ReplaceAllCaseInsensitive( const std::string& text, const std::string& find, const std::string& replace ){
	if ( find.empty() ) {
		return text;
	}
	const std::string foldedText = ToLowerCopy( text );
	const std::string foldedFind = ToLowerCopy( find );
	std::string out;
	std::size_t pos = 0;
	while ( true ) {
		const std::size_t match = foldedText.find( foldedFind, pos );
		if ( match == std::string::npos ) {
			out.append( text, pos, std::string::npos );
			break;
		}
		out.append( text, pos, match - pos );
		out.append( replace );
		pos = match + foldedFind.size();
	}
	return out;
}

static bool BuildReplacedTarget( const TextureFindReplaceState& state, const std::string& target, std::string& out ){
	switch ( state.matchMode )
	{
	case TextureFindMatchMode::Exact:
		out = state.replaceRaw;
		return true;
	case TextureFindMatchMode::Contains:
		out = state.caseSensitive
			? ReplaceAllCaseSensitive( target, state.findPattern, state.replaceRaw )
			: ReplaceAllCaseInsensitive( target, state.findPattern, state.replaceRaw );
		return true;
	case TextureFindMatchMode::StartsWith:
		out = state.replaceRaw + target.substr( state.findPattern.size() );
		return true;
	case TextureFindMatchMode::EndsWith:
		out = target.substr( 0, target.size() - state.findPattern.size() ) + state.replaceRaw;
		return true;
	case TextureFindMatchMode::Wildcard:
		{
			std::vector<std::string> captures;
			if ( !WildcardMatchCapture( state.findPattern, target, state.caseSensitive, captures ) ) {
				return false;
			}
			out = ExpandWildcardReplacement( state.replaceRaw, captures );
		}
		return true;
	case TextureFindMatchMode::Regex:
		if ( !state.regexReady ) {
			return false;
		}
		out = std::regex_replace( target, state.regex, state.replaceRaw );
		return true;
	}
	return false;
}

static bool BuildReplacedTarget( const FindReplacePatternState& state, const std::string& target, std::string& out ){
	switch ( state.matchMode )
	{
	case TextureFindMatchMode::Exact:
		out = state.replaceRaw;
		return true;
	case TextureFindMatchMode::Contains:
		out = state.caseSensitive
			? ReplaceAllCaseSensitive( target, state.findPattern, state.replaceRaw )
			: ReplaceAllCaseInsensitive( target, state.findPattern, state.replaceRaw );
		return true;
	case TextureFindMatchMode::StartsWith:
		out = state.replaceRaw + target.substr( state.findPattern.size() );
		return true;
	case TextureFindMatchMode::EndsWith:
		out = target.substr( 0, target.size() - state.findPattern.size() ) + state.replaceRaw;
		return true;
	case TextureFindMatchMode::Wildcard:
		{
			std::vector<std::string> captures;
			if ( !WildcardMatchCapture( state.findPattern, target, state.caseSensitive, captures ) ) {
				return false;
			}
			out = ExpandWildcardReplacement( state.replaceRaw, captures );
		}
		return true;
	case TextureFindMatchMode::Regex:
		if ( !state.regexReady ) {
			return false;
		}
		out = std::regex_replace( target, state.regex, state.replaceRaw );
		return true;
	}
	return false;
}

static MatchResult MatchShader( const TextureFindReplaceState& state, const char* shader, bool wantReplacement, ShaderInfoCache& shaderCache ){
	MatchResult result;
	const ShaderParts parts = SplitShaderParts( shader );
	if ( parts.full.empty() ) {
		return result;
	}
	if ( !PassesFilters( state, parts.full ) ) {
		return result;
	}
	if ( state.useShaderFilters ) {
		const ShaderInfo& info = shaderCache.get( parts.full.c_str() );
		if ( !PassesShaderFilters( state, info ) ) {
			return result;
		}
	}
	const std::string& target = state.matchNameOnly ? parts.leaf : parts.full;
	if ( !MatchTarget( state, target ) ) {
		return result;
	}
	result.matched = true;
	if ( !wantReplacement ) {
		return result;
	}

	std::string replacement;
	if ( state.replaceMode == TextureReplaceMode::ReplaceFull ) {
		const bool replaceHasPath = state.replaceRaw.find( '/' ) != std::string::npos
			|| state.replaceRaw.find( '\\' ) != std::string::npos;
		if ( state.matchNameOnly && !replaceHasPath ) {
			replacement = parts.prefix + state.replaceRaw;
		}
		else {
			replacement = state.replaceFull;
		}
	}
	else {
		std::string replacedTarget;
		if ( !BuildReplacedTarget( state, target, replacedTarget ) ) {
			return result;
		}
		replacement = state.matchNameOnly ? ( parts.prefix + replacedTarget ) : replacedTarget;
	}

	replacement = CleanShaderPath( replacement );
	if ( !texdef_name_valid( replacement.c_str() ) ) {
		globalWarningStream() << "FindReplaceTextures: invalid replacement texture: " << SingleQuoted( replacement.c_str() ) << '\n';
		return result;
	}

	result.replacementValid = true;
	result.replacement = replacement;
	return result;
}

template<typename Functor>
inline const Functor& Scene_ForEachVisibleBrush_ForEachFace( scene::Graph& graph, const Functor& functor ){
	Scene_forEachVisibleBrush( graph, BrushForEachFace( FaceInstanceVisitFace<Functor>( functor ) ) );
	return functor;
}

template<typename Functor>
inline const Functor& Scene_ForEachVisibleBrush_ForEachFaceInstance( scene::Graph& graph, const Functor& functor ){
	Scene_forEachVisibleBrush( graph, BrushForEachFace( FaceInstanceVisitAll<Functor>( functor ) ) );
	return functor;
}

template<typename Functor>
inline const Functor& Scene_ForEachVisibleSelectedBrush_ForEachFace( const Functor& functor ){
	Scene_forEachVisibleSelectedBrush( BrushForEachFace( FaceInstanceVisitFace<Functor>( functor ) ) );
	return functor;
}

template<typename Functor>
inline const Functor& Scene_ForEachVisibleSelectedBrush_ForEachFaceInstance( const Functor& functor ){
	Scene_forEachVisibleSelectedBrush( BrushForEachFace( FaceInstanceVisitAll<Functor>( functor ) ) );
	return functor;
}

template<typename Functor>
class PatchForEachAnyWalker : public scene::Graph::Walker
{
	const Functor& m_functor;
public:
	PatchForEachAnyWalker( const Functor& functor ) : m_functor( functor ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		Patch* patch = Node_getPatch( path.top() );
		if ( patch != nullptr ) {
			m_functor( *patch );
		}
		return true;
	}
};

template<typename Functor>
class PatchForEachInstanceAnyWalker : public scene::Graph::Walker
{
	const Functor& m_functor;
public:
	PatchForEachInstanceAnyWalker( const Functor& functor ) : m_functor( functor ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)path;
		PatchInstance* patch = Instance_getPatch( instance );
		if ( patch != nullptr ) {
			m_functor( *patch );
		}
		return true;
	}
};

}

// TTimo modified to handle shader architecture:
// expects shader names at input, comparison relies on shader names .. texture names no longer relevant
void FindReplaceTextures( const TextureFindReplaceOptions& options ){
	TextureFindReplaceState state;
	std::string error;
	if ( !BuildFindReplaceState( options, state, error ) ) {
		globalErrorStream() << "FindReplaceTextures: " << error.c_str() << ", aborted\n";
		return;
	}

	ShaderInfoCache shaderCache;

	if ( !state.includeBrushes && !state.includePatches ) {
		globalErrorStream() << "FindReplaceTextures: no target types enabled (brushes/patches)\n";
		return;
	}

	const bool doReplace = state.doReplace;
	if ( doReplace ) {
		const auto command = StringStream<256>( "textureFindReplace -find ", state.findPattern.c_str(), " -replace ", state.replaceRaw.c_str() );
		UndoableCommand undo( command );
	}

	int matchedBrushFaces = 0;
	int replacedBrushFaces = 0;
	int matchedPatches = 0;
	int replacedPatches = 0;

	if ( state.includeBrushes ) {
		if ( doReplace ) {
			struct FaceReplace
			{
				const TextureFindReplaceState& state;
				ShaderInfoCache& shaderCache;
				int* matched;
				int* replaced;
				void operator()( Face& face ) const {
					const MatchResult result = MatchShader( state, face.GetShader(), true, shaderCache );
					if ( !result.matched ) {
						return;
					}
					++( *matched );
					if ( result.replacementValid ) {
						face.SetShader( result.replacement.c_str() );
						++( *replaced );
					}
				}
			};
			const FaceReplace replacer{ state, shaderCache, &matchedBrushFaces, &replacedBrushFaces };

			switch ( state.scope )
			{
			case TextureFindScope::All:
				if ( state.visibleOnly ) {
					Scene_ForEachVisibleBrush_ForEachFace( GlobalSceneGraph(), replacer );
				}
				else {
					Scene_ForEachBrush_ForEachFace( GlobalSceneGraph(), replacer );
				}
				break;
			case TextureFindScope::Selected:
				if ( state.visibleOnly ) {
					Scene_ForEachVisibleSelectedBrush_ForEachFace( replacer );
				}
				else {
					Scene_ForEachSelectedBrush_ForEachFace( GlobalSceneGraph(), replacer );
				}
				break;
			case TextureFindScope::SelectedFaces:
				Scene_ForEachSelectedBrushFace( GlobalSceneGraph(), replacer );
				break;
			}
		}
		else {
			struct FaceSelect
			{
				const TextureFindReplaceState& state;
				ShaderInfoCache& shaderCache;
				int* matched;
				void operator()( FaceInstance& face ) const {
					const MatchResult result = MatchShader( state, face.getFace().GetShader(), false, shaderCache );
					if ( !result.matched ) {
						return;
					}
					face.setSelected( SelectionSystem::eFace, true );
					++( *matched );
				}
			};
			const FaceSelect selector{ state, shaderCache, &matchedBrushFaces };

			switch ( state.scope )
			{
			case TextureFindScope::All:
				if ( state.visibleOnly ) {
					Scene_ForEachVisibleBrush_ForEachFaceInstance( GlobalSceneGraph(), selector );
				}
				else {
					Scene_ForEachBrush_ForEachFaceInstance( GlobalSceneGraph(), selector );
				}
				break;
			case TextureFindScope::Selected:
				if ( state.visibleOnly ) {
					Scene_ForEachVisibleSelectedBrush_ForEachFaceInstance( selector );
				}
				else {
					Scene_ForEachSelectedBrush_ForEachFaceInstance( GlobalSceneGraph(), selector );
				}
				break;
			case TextureFindScope::SelectedFaces:
			{
				struct FaceCount
				{
					const TextureFindReplaceState& state;
					ShaderInfoCache& shaderCache;
					int* matched;
					void operator()( Face& face ) const {
						if ( MatchShader( state, face.GetShader(), false, shaderCache ).matched ) {
							++( *matched );
						}
					}
				};
				const FaceCount counter{ state, shaderCache, &matchedBrushFaces };
				Scene_ForEachSelectedBrushFace( GlobalSceneGraph(), counter );
				break;
			}
			}
		}
	}

	if ( state.includePatches ) {
		if ( doReplace ) {
			struct PatchReplace
			{
				const TextureFindReplaceState& state;
				ShaderInfoCache& shaderCache;
				int* matched;
				int* replaced;
				void operator()( Patch& patch ) const {
					const MatchResult result = MatchShader( state, patch.GetShader(), true, shaderCache );
					if ( !result.matched ) {
						return;
					}
					++( *matched );
					if ( result.replacementValid ) {
						patch.SetShader( result.replacement.c_str() );
						++( *replaced );
					}
				}
			};
			const PatchReplace replacer{ state, shaderCache, &matchedPatches, &replacedPatches };

			switch ( state.scope )
			{
			case TextureFindScope::All:
				if ( state.visibleOnly ) {
					Scene_forEachVisiblePatch( replacer );
				}
				else {
					GlobalSceneGraph().traverse( PatchForEachAnyWalker<PatchReplace>( replacer ) );
				}
				break;
			case TextureFindScope::Selected:
				if ( state.visibleOnly ) {
					Scene_forEachVisibleSelectedPatch( replacer );
				}
				else {
					Scene_forEachSelectedPatch( [&]( PatchInstance& patch ){ replacer( patch.getPatch() ); } );
				}
				break;
			case TextureFindScope::SelectedFaces:
				Scene_forEachSelectedPatch( [&]( PatchInstance& patch ){ replacer( patch.getPatch() ); } );
				break;
			}
		}
		else {
			struct PatchSelect
			{
				const TextureFindReplaceState& state;
				ShaderInfoCache& shaderCache;
				int* matched;
				void operator()( PatchInstance& patch ) const {
					const MatchResult result = MatchShader( state, patch.getPatch().GetShader(), false, shaderCache );
					if ( !result.matched ) {
						return;
					}
					patch.setSelected( true );
					++( *matched );
				}
			};
			const PatchSelect selector{ state, shaderCache, &matchedPatches };

			switch ( state.scope )
			{
			case TextureFindScope::All:
				if ( state.visibleOnly ) {
					Scene_forEachVisiblePatchInstance( selector );
				}
				else {
					GlobalSceneGraph().traverse( PatchForEachInstanceAnyWalker<PatchSelect>( selector ) );
				}
				break;
			case TextureFindScope::Selected:
				if ( state.visibleOnly ) {
					Scene_forEachVisibleSelectedPatchInstance( selector );
				}
				else {
					Scene_forEachSelectedPatch( selector );
				}
				break;
			case TextureFindScope::SelectedFaces:
				Scene_forEachSelectedPatch( selector );
				break;
			}
		}
	}

	if ( doReplace ) {
		globalOutputStream() << "Find/Replace Textures: matched " << matchedBrushFaces << " brush faces, " << matchedPatches
			<< " patches; replaced " << replacedBrushFaces << " brush faces, " << replacedPatches << " patches.\n";
	}
	else {
		globalOutputStream() << "Find Textures: matched " << matchedBrushFaces << " brush faces, " << matchedPatches << " patches.\n";
	}
}

void FindReplaceEntities( const EntityFindReplaceOptions& options ){
	EntityFindReplaceState state;
	std::string error;
	if ( !BuildEntityFindReplaceState( options, state, error ) ) {
		globalErrorStream() << "FindReplaceEntities: " << error.c_str() << ", aborted\n";
		return;
	}

	const bool doReplace = state.doReplace;
	if ( doReplace ) {
		const auto command = StringStream<256>( "entityFindReplace -find ", state.pattern.findPattern.c_str(), " -replace ", state.pattern.replaceRaw.c_str() );
		UndoableCommand undo( command );
	}

	int matchedEntities = 0;
	int matchedKeys = 0;
	int matchedValues = 0;
	int replacedKeys = 0;
	int replacedValues = 0;
	bool warnedInvalidKey = false;

	class EntityFindReplaceWalker : public scene::Graph::Walker
	{
		const EntityFindReplaceState& state;
		const scene::Node* m_world = Map_FindWorldspawn( g_map );
		int* matchedEntities;
		int* matchedKeys;
		int* matchedValues;
		int* replacedKeys;
		int* replacedValues;
		bool* warnedInvalidKey;
	public:
		EntityFindReplaceWalker( const EntityFindReplaceState& state, int* matchedEntities, int* matchedKeys, int* matchedValues,
		                         int* replacedKeys, int* replacedValues, bool* warnedInvalidKey )
			: state( state ),
			  matchedEntities( matchedEntities ),
			  matchedKeys( matchedKeys ),
			  matchedValues( matchedValues ),
			  replacedKeys( replacedKeys ),
			  replacedValues( replacedValues ),
			  warnedInvalidKey( warnedInvalidKey ){
		}

		bool pre( const scene::Path& path, scene::Instance& instance ) const override {
			if ( state.visibleOnly && !path.top().get().visible() ) {
				return false;
			}
			Entity* entity = Node_getEntity( path.top() );
			if ( entity == nullptr ) {
				return true;
			}
			if ( !state.includeWorldspawn && path.top().get_pointer() == m_world ) {
				return false;
			}
			if ( state.scope == EntityFindScope::Selected
			     && !( Instance_isSelected( instance ) || instance.childSelected() ) ) {
				return false;
			}
			if ( !state.classFilters.empty()
			     && !MatchesAnyFilter( state.classFilters, entity->getClassName(), state.pattern.caseSensitive ) ) {
				return false;
			}

			struct KeyValueEntry
			{
				std::string key;
				std::string value;
			};
			struct Collector : public Entity::Visitor
			{
				std::vector<KeyValueEntry>& entries;
				explicit Collector( std::vector<KeyValueEntry>& entries ) : entries( entries ){
				}
				void visit( const char* key, const char* value ) override {
					entries.push_back( { key ? key : "", value ? value : "" } );
				}
			};
			std::vector<KeyValueEntry> entries;
			Collector collector( entries );
			entity->forEachKeyValue( collector );

			const auto buildReplacement = [this]( const std::string& target, std::string& replacement ) -> bool {
				if ( state.pattern.replaceMode == TextureReplaceMode::ReplaceFull ) {
					replacement = state.pattern.replaceRaw;
					return true;
				}
				return BuildReplacedTarget( state.pattern, target, replacement );
			};

			bool entityMatched = false;
			std::vector<std::pair<std::string, std::string>> valueUpdates;
			std::vector<std::pair<std::string, std::string>> keyRenames;

			for ( const auto& entry : entries )
			{
				const bool isClassnameKey = string_equal( entry.key.c_str(), "classname" );
				if ( !state.keyFilters.empty()
				     && !MatchesAnyFilter( state.keyFilters, entry.key, state.pattern.caseSensitive ) ) {
					continue;
				}

				const bool keyMatched = state.searchKeys && MatchTarget( state.pattern, entry.key );
				const bool valueMatched = state.searchValues && MatchTarget( state.pattern, entry.value );

				if ( keyMatched ) {
					++( *matchedKeys );
					entityMatched = true;
				}
				if ( valueMatched ) {
					++( *matchedValues );
					entityMatched = true;
				}

				if ( !state.doReplace || isClassnameKey ) {
					continue;
				}

				if ( keyMatched && state.replaceKeys ) {
					std::string replacementKey;
					if ( buildReplacement( entry.key, replacementKey )
					     && replacementKey != entry.key ) {
						if ( !EntityKeyNameValid( replacementKey ) ) {
							if ( !*warnedInvalidKey ) {
								*warnedInvalidKey = true;
								globalWarningStream() << "FindReplaceEntities: invalid key name replacement skipped\n";
							}
						}
						else if ( !string_equal( replacementKey.c_str(), "classname" ) ) {
							keyRenames.emplace_back( entry.key, replacementKey );
						}
					}
				}

				if ( valueMatched && state.replaceValues ) {
					std::string replacementValue;
					if ( buildReplacement( entry.value, replacementValue )
					     && replacementValue != entry.value ) {
						valueUpdates.emplace_back( entry.key, replacementValue );
					}
				}
			}

			if ( entityMatched ) {
				++( *matchedEntities );
				if ( !state.doReplace ) {
					if ( Selectable* selectable = Instance_getSelectable( instance ) ) {
						selectable->setSelected( true );
					}
				}
			}

			if ( state.doReplace ) {
				for ( const auto& [key, value] : valueUpdates )
				{
					entity->setKeyValue( key.c_str(), value.c_str() );
					++( *replacedValues );
				}
				for ( const auto& [oldKey, newKey] : keyRenames )
				{
					const char* value = entity->getKeyValue( oldKey.c_str() );
					entity->setKeyValue( newKey.c_str(), value );
					entity->setKeyValue( oldKey.c_str(), "" );
					++( *replacedKeys );
				}
			}

			return false;
		}
	};

	GlobalSceneGraph().traverse( EntityFindReplaceWalker( state, &matchedEntities, &matchedKeys, &matchedValues, &replacedKeys, &replacedValues, &warnedInvalidKey ) );

	if ( doReplace ) {
		globalOutputStream() << "Find/Replace Entities: matched " << matchedEntities << " entities, " << matchedKeys
			<< " key matches, " << matchedValues << " value matches; replaced " << replacedKeys << " keys, "
			<< replacedValues << " values.\n";
	}
	else {
		globalOutputStream() << "Find Entities: matched " << matchedEntities << " entities, " << matchedKeys
			<< " key matches, " << matchedValues << " value matches.\n";
	}
}

typedef std::vector<const char*> PropertyValues;

bool propertyvalues_contain( const PropertyValues& propertyvalues, const char *str ){
	return std::ranges::any_of( propertyvalues, [str]( const char *prop ){ return string_equal( str, prop ); } );
}

template<typename EntityMatcher>
class EntityFindByPropertyValueWalker : public scene::Graph::Walker
{
	const EntityMatcher& m_entityMatcher;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	EntityFindByPropertyValueWalker( const EntityMatcher& entityMatcher ) : m_entityMatcher( entityMatcher ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( !path.top().get().visible() ){
			return false;
		}
		// ignore worldspawn
		if ( path.top().get_pointer() == m_world ) {
			return false;
		}

		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ){
			if( m_entityMatcher( entity ) ) {
				Instance_getSelectable( instance )->setSelected( true );
				return true;
			}
			return false;
		}
		else if( path.size() > 2 && !path.top().get().isRoot() ){
			Selectable* selectable = Instance_getSelectable( instance );
			if( selectable != 0 )
				selectable->setSelected( true );
		}
		return true;
	}
};

template<typename EntityMatcher>
void Scene_EntitySelectByPropertyValues( scene::Graph& graph, const EntityMatcher& entityMatcher ){
	graph.traverse( EntityFindByPropertyValueWalker<EntityMatcher>( entityMatcher ) );
}

void Scene_EntitySelectByPropertyValues( scene::Graph& graph, const char *prop, const PropertyValues& propertyvalues ){
	Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), [prop, &propertyvalues]( const Entity* entity )->bool{
		return propertyvalues_contain( propertyvalues, entity->getKeyValue( prop ) );
	} );
}

class EntityGetSelectedPropertyValuesWalker : public scene::Graph::Walker
{
	PropertyValues& m_propertyvalues;
	const char *m_prop;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	EntityGetSelectedPropertyValuesWalker( const char *prop, PropertyValues& propertyvalues )
		: m_propertyvalues( propertyvalues ), m_prop( prop ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Entity* entity = Node_getEntity( path.top() ) ){
			if( path.top().get_pointer() != m_world ){
				if ( Instance_isSelected( instance ) || instance.childSelected() ) {
					if ( !propertyvalues_contain( m_propertyvalues, entity->getKeyValue( m_prop ) ) ) {
						m_propertyvalues.push_back( entity->getKeyValue( m_prop ) );
					}
				}
			}
			return false;
		}
		return true;
	}
};
/*
class EntityGetSelectedPropertyValuesWalker : public scene::Graph::Walker
{
PropertyValues& m_propertyvalues;
const char *m_prop;
mutable bool m_selected_children;
const scene::Node* m_world;
public:
EntityGetSelectedPropertyValuesWalker( const char *prop, PropertyValues& propertyvalues )
	: m_propertyvalues( propertyvalues ), m_prop( prop ), m_selected_children( false ), m_world( Map_FindWorldspawn( g_map ) ){
}
bool pre( const scene::Path& path, scene::Instance& instance ) const {
	if ( Instance_isSelected( instance ) ) {
		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ) {
			if ( !propertyvalues_contain( m_propertyvalues, entity->getKeyValue( m_prop ) ) ) {
				m_propertyvalues.push_back( entity->getKeyValue( m_prop ) );
			}
			return false;
		}
		else{
			m_selected_children = true;
		}
	}
	return true;
}
void post( const scene::Path& path, scene::Instance& instance ) const {
	Entity* entity = Node_getEntity( path.top() );
	if( entity != 0 && m_selected_children ){
		m_selected_children = false;
		if( path.top().get_pointer() == m_world )
			return;
		if ( !propertyvalues_contain( m_propertyvalues, entity->getKeyValue( m_prop ) ) ) {
			m_propertyvalues.push_back( entity->getKeyValue( m_prop ) );
		}
	}
}
};
*/
void Scene_EntityGetPropertyValues( scene::Graph& graph, const char *prop, PropertyValues& propertyvalues ){
	graph.traverse( EntityGetSelectedPropertyValuesWalker( prop, propertyvalues ) );
}

void Scene_BrushPatchSelectByShader( const char *shader ){
	Scene_BrushSelectByShader( GlobalSceneGraph(), shader );
	Scene_PatchSelectByShader( GlobalSceneGraph(), shader );
}

void Select_AllOfType(){
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent ) {
		if ( GlobalSelectionSystem().ComponentMode() == SelectionSystem::eFace ) {
			GlobalSelectionSystem().setSelectedAllComponents( false );
			Scene_BrushSelectByShader_Component( GlobalSceneGraph(), TextureBrowser_GetSelectedShader() );
		}
	}
	else{
		PropertyValues propertyvalues;
		const char *prop = "classname";
		Scene_EntityGetPropertyValues( GlobalSceneGraph(), prop, propertyvalues );
		GlobalSelectionSystem().setSelectedAll( false );
		if ( !propertyvalues.empty() )
			Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), prop, propertyvalues );
		else
			Scene_BrushPatchSelectByShader( TextureBrowser_GetSelectedShader() );
	}
}

void Select_EntitiesByKeyValue( const char* key, const char* value ){
	GlobalSelectionSystem().setSelectedAll( false );
	if( key != nullptr && value != nullptr ){
		if( !string_empty( key ) && !string_empty( value ) ){
			Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), [key, value]( const Entity* entity )->bool{
				return string_equal_nocase( entity->getKeyValue( key ), value );
			} );
		}
	}
	else if( key != nullptr ){
		if( !string_empty( key ) ){
			Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), [key]( const Entity* entity )->bool{
				return entity->hasKeyValue( key );
			} );
		}
	}
	else if( value != nullptr ){
		if( !string_empty( value ) ){
			Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), [value]( const Entity* entity )->bool{
				class Visitor : public Entity::Visitor
				{
					const char* const m_value;
				public:
					bool m_found = false;
					Visitor( const char* value ) : m_value( value ){
					}
					void visit( const char* key, const char* value ) override {
						if ( string_equal_nocase( m_value, value ) ) {
							m_found = true;
						}
					}
				} visitor( value );
				entity->forEachKeyValue( visitor );
				return visitor.m_found;
			} );
		}
	}
}

void Select_FacesAndPatchesByShader( const char *shader ){
	Scene_BrushFacesSelectByShader( GlobalSceneGraph(), shader );
	Scene_PatchSelectByShader( GlobalSceneGraph(), shader );
}
void Select_FacesAndPatchesByShader_(){
	Select_FacesAndPatchesByShader( TextureBrowser_GetSelectedShader() );
}

void Select_Inside(){
	SelectByBounds<SelectionPolicy_Inside>::DoSelection();
}

void Select_Touching(){
	SelectByBounds<SelectionPolicy_Touching>::DoSelection( false );
}

void Select_TouchingTall(){
	SelectByBounds<SelectionPolicy_TouchingTall>::DoSelection( false );
}

void Select_ProjectTexture( const texdef_t& texdef, const Vector3* direction ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushProjectTexture_Selected( GlobalSceneGraph(), texdef, direction );
		Scene_PatchProjectTexture_Selected( GlobalSceneGraph(), texdef, direction );
	}
	Scene_BrushProjectTexture_Component_Selected( GlobalSceneGraph(), texdef, direction );

	SceneChangeNotify();
}

void Select_ProjectTexture( const TextureProjection& projection, const Vector3& normal ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushProjectTexture_Selected( GlobalSceneGraph(), projection, normal );
		Scene_PatchProjectTexture_Selected( GlobalSceneGraph(), projection, normal );
	}
	Scene_BrushProjectTexture_Component_Selected( GlobalSceneGraph(), projection, normal );

	SceneChangeNotify();
}

void Select_FitTexture( float horizontal, float vertical, bool only_dimension ){
	if ( GlobalSelectionSystem().Mode() != SelectionSystem::eComponent ) {
		Scene_BrushFitTexture_Selected( GlobalSceneGraph(), horizontal, vertical, only_dimension );
		Scene_PatchTileTexture_Selected( GlobalSceneGraph(), horizontal, vertical );
	}
	Scene_BrushFitTexture_Component_Selected( GlobalSceneGraph(), horizontal, vertical, only_dimension );

	SceneChangeNotify();
}


#include "commands.h"
#include "dialog.h"

inline void hide_node( scene::Node& node, bool hide ){
	hide
	? node.enable( scene::Node::eHidden )
	: node.disable( scene::Node::eHidden );
}

bool g_nodes_be_hidden = false;

ToggleItem g_hidden_item{ BoolExportCaller( g_nodes_be_hidden ) };

class HideSelectedWalker : public scene::Graph::Walker
{
	const bool m_hide;
public:
	HideSelectedWalker( bool hide )
		: m_hide( hide ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Instance_isSelected( instance ) ) {
			g_nodes_be_hidden = m_hide;
			hide_node( path.top(), m_hide );
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		if( m_hide && Node_isEntity( path.top().get() ) ) // hide group entity labels, when their content is entirely hidden
			if( scene::Traversable* traversable = Node_getTraversable( path.top().get() ) )
				if( Traversable_all_of_children( traversable, []( const scene::Node& node ){ return node.excluded( scene::Node::eHidden ); } ) )
					hide_node( path.top(), true );
	}
};

void Scene_Hide_Selected( bool hide ){
	GlobalSceneGraph().traverse( HideSelectedWalker( hide ) );
}

void Select_Hide(){
	Scene_Hide_Selected( true );
	/* not hiding worldspawn node so that newly created brushes are visible */
	if( scene::Node* w = Map_FindWorldspawn( g_map ) )
		hide_node( *w, false );
	SceneChangeNotify();
}

void HideSelected(){
	Select_Hide();
	if( GlobalSelectionSystem().countSelectedComponents() != 0 )
		GlobalSelectionSystem().setSelectedAllComponents( false );
	GlobalSelectionSystem().setSelectedAll( false );
	g_hidden_item.update();
}


class HideAllWalker : public scene::Graph::Walker
{
	bool m_hide;
public:
	HideAllWalker( bool hide )
		: m_hide( hide ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		hide_node( path.top(), m_hide );
		return true;
	}
};

void Scene_Hide_All( bool hide ){
	GlobalSceneGraph().traverse( HideAllWalker( hide ) );
}

void Select_ShowAllHidden(){
	Scene_Hide_All( false );
	SceneChangeNotify();
	g_nodes_be_hidden = false;
	g_hidden_item.update();
}


void Selection_Flipx(){
	UndoableCommand undo( "mirrorSelected -axis x" );
	Select_FlipAxis( 0 );
}

void Selection_Flipy(){
	UndoableCommand undo( "mirrorSelected -axis y" );
	Select_FlipAxis( 1 );
}

void Selection_Flipz(){
	UndoableCommand undo( "mirrorSelected -axis z" );
	Select_FlipAxis( 2 );
}

void Selection_Rotatex(){
	UndoableCommand undo( "rotateSelected -axis x -angle -90" );
	Select_RotateAxis( 0, -90 );
}

void Selection_Rotatey(){
	UndoableCommand undo( "rotateSelected -axis y -angle 90" );
	Select_RotateAxis( 1, 90 );
}

void Selection_Rotatez(){
	UndoableCommand undo( "rotateSelected -axis z -angle -90" );
	Select_RotateAxis( 2, -90 );
}
#include "xywindow.h"
void Selection_FlipHorizontally(){
	VIEWTYPE viewtype = GlobalXYWnd_getCurrentViewType();
	switch ( viewtype )
	{
	case XY:
	case XZ:
		Selection_Flipx();
		break;
	default:
		Selection_Flipy();
		break;
	}
}

void Selection_FlipVertically(){
	VIEWTYPE viewtype = GlobalXYWnd_getCurrentViewType();
	switch ( viewtype )
	{
	case XZ:
	case YZ:
		Selection_Flipz();
		break;
	default:
		Selection_Flipy();
		break;
	}
}

void Selection_RotateClockwise(){
	UndoableCommand undo( "rotateSelected Clockwise 90" );
	VIEWTYPE viewtype = GlobalXYWnd_getCurrentViewType();
	switch ( viewtype )
	{
	case XY:
		Select_RotateAxis( 2, -90 );
		break;
	case XZ:
		Select_RotateAxis( 1, 90 );
		break;
	default:
		Select_RotateAxis( 0, -90 );
		break;
	}
}

void Selection_RotateAnticlockwise(){
	UndoableCommand undo( "rotateSelected Anticlockwise 90" );
	VIEWTYPE viewtype = GlobalXYWnd_getCurrentViewType();
	switch ( viewtype )
	{
	case XY:
		Select_RotateAxis( 2, 90 );
		break;
	case XZ:
		Select_RotateAxis( 1, -90 );
		break;
	default:
		Select_RotateAxis( 0, 90 );
		break;
	}
}


void Nudge( int nDim, float fNudge ){
	Vector3 translate( 0, 0, 0 );
	translate[nDim] = fNudge;

	GlobalSelectionSystem().translateSelected( translate );
}

void Selection_NudgeZ( float amount ){
	const auto command = StringStream<64>( "nudgeSelected -axis z -amount ", amount );
	UndoableCommand undo( command );

	Nudge( 2, amount );
}

void Selection_MoveDown(){
	Selection_NudgeZ( -GetGridSize() );
}

void Selection_MoveUp(){
	Selection_NudgeZ( GetGridSize() );
}


inline Quaternion quaternion_for_euler_xyz_degrees( const Vector3& eulerXYZ ){
#if 0
	return quaternion_for_matrix4_rotation( matrix4_rotation_for_euler_xyz_degrees( eulerXYZ ) );
#elif 0
	return quaternion_multiplied_by_quaternion(
	           quaternion_multiplied_by_quaternion(
	               quaternion_for_z( degrees_to_radians( eulerXYZ[2] ) ),
	               quaternion_for_y( degrees_to_radians( eulerXYZ[1] ) )
	           ),
	           quaternion_for_x( degrees_to_radians( eulerXYZ[0] ) )
	       );
#elif 1
	double cx = cos( degrees_to_radians( eulerXYZ[0] * 0.5 ) );
	double sx = sin( degrees_to_radians( eulerXYZ[0] * 0.5 ) );
	double cy = cos( degrees_to_radians( eulerXYZ[1] * 0.5 ) );
	double sy = sin( degrees_to_radians( eulerXYZ[1] * 0.5 ) );
	double cz = cos( degrees_to_radians( eulerXYZ[2] * 0.5 ) );
	double sz = sin( degrees_to_radians( eulerXYZ[2] * 0.5 ) );

	return Quaternion(
	           cz * cy * sx - sz * sy * cx,
	           cz * sy * cx + sz * cy * sx,
	           sz * cy * cx - cz * sy * sx,
	           cz * cy * cx + sz * sy * sx
	       );
#endif
}


void Undo(){
	GlobalUndoSystem().undo();
	SceneChangeNotify();
}

void Redo(){
	GlobalUndoSystem().redo();
	SceneChangeNotify();
}

void deleteSelection(){
	if( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent && GlobalSelectionSystem().countSelectedComponents() != 0 ){
		UndoableCommand undo( "deleteSelectedComponents" );
		CSG_DeleteComponents();
	}
	else{
		UndoableCommand undo( "deleteSelected" );
		Select_Delete();
	}
}

void Map_ExportSelected( TextOutputStream& ostream ){
	Map_ExportSelected( ostream, Map_getFormat( g_map ) );
}

void Map_ImportSelected( TextInputStream& istream ){
	Map_ImportSelected( istream, Map_getFormat( g_map ) );
}

void Selection_Copy(){
	clipboard_copy( Map_ExportSelected );
}

void Selection_Paste(){
	clipboard_paste( Map_ImportSelected );
}

void Copy(){
	Selection_Copy();
}

void Paste(){
	UndoableCommand undo( "paste" );

	GlobalSelectionSystem().setSelectedAll( false );
	Selection_Paste();
}

void TranslateToCamera(){
	CamWnd& camwnd = *g_pParentWnd->GetCamWnd();
	GlobalSelectionSystem().translateSelected( vector3_snapped( Camera_getOrigin( camwnd ) - GlobalSelectionSystem().getBoundsSelected().origin, GetSnapGridSize() ) );
}

void PasteToCamera(){
	GlobalSelectionSystem().setSelectedAll( false );
	UndoableCommand undo( "pasteToCamera" );
	Selection_Paste();
	TranslateToCamera();
}

void MoveToCamera(){
	UndoableCommand undo( "moveToCamera" );
	TranslateToCamera();
}



class CloneSelected : public scene::Graph::Walker
{
	const bool m_makeUnique;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	mutable std::vector<scene::Node*> m_cloned;
	CloneSelected( bool makeUnique ) : m_makeUnique( makeUnique ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.size() == 1 ) {
			return true;
		}

		if ( path.top().get_pointer() == m_world ) { // ignore worldspawn, but keep checking children
			return true;
		}

		if ( !path.top().get().isRoot() ) {
			if ( Instance_isSelected( instance ) ) {
				return false;
			}
			if( m_makeUnique && instance.childSelected() ){ /* clone selected group entity primitives to new group entity */
				return false;
			}
		}

		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.size() == 1 ) {
			return;
		}

		if ( path.top().get_pointer() == m_world ) { // ignore worldspawn
			return;
		}

		if ( !path.top().get().isRoot() ) {
			if ( Instance_isSelected( instance ) ) {
				NodeSmartReference clone( Node_Clone( path.top() ) );
				Map_gatherNamespaced( clone );
				Node_getTraversable( path.parent().get() )->insert( clone );
				m_cloned.push_back( clone.get_pointer() );
			}
			else if( m_makeUnique && instance.childSelected() ){ /* clone selected group entity primitives to new group entity */
				NodeSmartReference clone( Node_Clone_Selected( path.top() ) );
				Map_gatherNamespaced( clone );
				Node_getTraversable( path.parent().get() )->insert( clone );
				m_cloned.push_back( clone.get_pointer() );
			}
		}
	}
};

void Scene_Clone_Selected( scene::Graph& graph, bool makeUnique ){
	CloneSelected cloneSelected( makeUnique );
	graph.traverse( cloneSelected );

	Map_mergeClonedNames( makeUnique );

	/* deselect originals */
	GlobalSelectionSystem().setSelectedAll( false );
	/* select cloned */
	for( scene::Node *node : cloneSelected.m_cloned )
	{
		class walker : public scene::Traversable::Walker
		{
		public:
			bool pre( scene::Node& node ) const override {
				if( scene::Instantiable *instantiable = Node_getInstantiable( node ) ){
					class visitor : public scene::Instantiable::Visitor
					{
					public:
						void visit( scene::Instance& instance ) const override {
							Instance_setSelected( instance, true );
						}
					};

					instantiable->forEachInstance( visitor() );
				}
				return true;
			}
		};
		Node_traverseSubgraph( *node, walker() );
	}
}

enum ENudgeDirection
{
	eNudgeUp = 1,
	eNudgeDown = 3,
	eNudgeLeft = 0,
	eNudgeRight = 2,
};

struct AxisBase
{
	Vector3 x;
	Vector3 y;
	Vector3 z;
	AxisBase( const Vector3& x_, const Vector3& y_, const Vector3& z_ )
		: x( x_ ), y( y_ ), z( z_ ){
	}
};

AxisBase AxisBase_forViewType( VIEWTYPE viewtype ){
	switch ( viewtype )
	{
	case XY:
		return AxisBase( g_vector3_axis_x, g_vector3_axis_y, g_vector3_axis_z );
	case XZ:
		return AxisBase( g_vector3_axis_x, g_vector3_axis_z, g_vector3_axis_y );
	case YZ:
		return AxisBase( g_vector3_axis_y, g_vector3_axis_z, g_vector3_axis_x );
	}

	ERROR_MESSAGE( "invalid viewtype" );
	return AxisBase( Vector3( 0, 0, 0 ), Vector3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
}

Vector3 AxisBase_axisForDirection( const AxisBase& axes, ENudgeDirection direction ){
	switch ( direction )
	{
	case eNudgeLeft:
		return vector3_negated( axes.x );
	case eNudgeUp:
		return axes.y;
	case eNudgeRight:
		return axes.x;
	case eNudgeDown:
		return vector3_negated( axes.y );
	}

	ERROR_MESSAGE( "invalid direction" );
	return Vector3( 0, 0, 0 );
}

bool g_bNudgeAfterClone = false;

void NudgeSelection( ENudgeDirection direction, float fAmount, VIEWTYPE viewtype ){
	AxisBase axes( AxisBase_forViewType( viewtype ) );
	Vector3 view_direction( vector3_negated( axes.z ) );
	Vector3 nudge( vector3_scaled( AxisBase_axisForDirection( axes, direction ), fAmount ) );
	GlobalSelectionSystem().NudgeManipulator( nudge, view_direction );
}

void Selection_Clone(){
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive ) {
		UndoableCommand undo( "cloneSelected" );

		Scene_Clone_Selected( GlobalSceneGraph(), false );

		if( g_bNudgeAfterClone ){
			NudgeSelection( eNudgeRight, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
			NudgeSelection( eNudgeDown, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
		}
	}
}

void Selection_Clone_MakeUnique(){
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive ) {
		UndoableCommand undo( "cloneSelectedMakeUnique" );

		Scene_Clone_Selected( GlobalSceneGraph(), true );

		if( g_bNudgeAfterClone ){
			NudgeSelection( eNudgeRight, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
			NudgeSelection( eNudgeDown, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
		}
	}
}

// called when the escape key is used (either on the main window or on an inspector)
void Selection_Deselect(){
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent ) {
		if ( GlobalSelectionSystem().countSelectedComponents() != 0 ) {
			GlobalSelectionSystem().setSelectedAllComponents( false );
		}
		else
		{
			SelectionSystem_DefaultMode();
			ComponentModeChanged();
		}
	}
	else
	{
		if ( GlobalSelectionSystem().countSelectedComponents() != 0 ) {
			GlobalSelectionSystem().setSelectedAllComponents( false );
		}
		else
		{
			GlobalSelectionSystem().setSelectedAll( false );
		}
	}
}

void Scene_Clone_Selected(){
	Scene_Clone_Selected( GlobalSceneGraph(), false );
}


void Selection_NudgeUp(){
	UndoableCommand undo( "nudgeSelectedUp" );
	NudgeSelection( eNudgeUp, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
}

void Selection_NudgeDown(){
	UndoableCommand undo( "nudgeSelectedDown" );
	NudgeSelection( eNudgeDown, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
}

void Selection_NudgeLeft(){
	UndoableCommand undo( "nudgeSelectedLeft" );
	NudgeSelection( eNudgeLeft, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
}

void Selection_NudgeRight(){
	UndoableCommand undo( "nudgeSelectedRight" );
	NudgeSelection( eNudgeRight, GetGridSize(), GlobalXYWnd_getCurrentViewType() );
}



void Texdef_Rotate( float angle ){
	const auto command = StringStream<64>( "brushRotateTexture -angle ", angle );
	UndoableCommand undo( command );
	Select_RotateTexture( angle );
}
// these are actually {Anti,}Clockwise in BP mode only (AP/220 - 50/50)
// TODO is possible to make really {Anti,}Clockwise
void Texdef_RotateClockwise(){
	Texdef_Rotate( -std::fabs( g_si_globals.rotate ) );
}

void Texdef_RotateAntiClockwise(){
	Texdef_Rotate( std::fabs( g_si_globals.rotate ) );
}

void Texdef_Scale( float x, float y ){
	const auto command = StringStream<64>( "brushScaleTexture -x ", x, " -y ", y );
	UndoableCommand undo( command );
	Select_ScaleTexture( x, y );
}

void Texdef_ScaleUp(){
	Texdef_Scale( 0, g_si_globals.scale[1] );
}

void Texdef_ScaleDown(){
	Texdef_Scale( 0, -g_si_globals.scale[1] );
}

void Texdef_ScaleLeft(){
	Texdef_Scale( -g_si_globals.scale[0], 0 );
}

void Texdef_ScaleRight(){
	Texdef_Scale( g_si_globals.scale[0], 0 );
}

void Texdef_Shift( float x, float y ){
	const auto command = StringStream<64>( "brushShiftTexture -x ", x, " -y ", y );
	UndoableCommand undo( command );
	Select_ShiftTexture( x, y );
}

void Texdef_ShiftLeft(){
	Texdef_Shift( -g_si_globals.shift[0], 0 );
}

void Texdef_ShiftRight(){
	Texdef_Shift( g_si_globals.shift[0], 0 );
}

void Texdef_ShiftUp(){
	Texdef_Shift( 0, g_si_globals.shift[1] );
}

void Texdef_ShiftDown(){
	Texdef_Shift( 0, -g_si_globals.shift[1] );
}



class SnappableSnapToGridSelected : public scene::Graph::Walker
{
	float m_snap;
public:
	SnappableSnapToGridSelected( float snap )
		: m_snap( snap ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.top().get().visible() ) {
			Snappable* snappable = Node_getSnappable( path.top() );
			if ( snappable != 0
			  && Instance_isSelected( instance ) ) {
				snappable->snapto( m_snap );
			}
		}
		return true;
	}
};

void Scene_SnapToGrid_Selected( scene::Graph& graph, float snap ){
	graph.traverse( SnappableSnapToGridSelected( snap ) );
}

class ComponentSnappableSnapToGridSelected : public scene::Graph::Walker
{
	float m_snap;
public:
	ComponentSnappableSnapToGridSelected( float snap )
		: m_snap( snap ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.top().get().visible() ) {
			ComponentSnappable* componentSnappable = Instance_getComponentSnappable( instance );
			if ( componentSnappable != 0
			  && Instance_isSelected( instance ) ) {
				componentSnappable->snapComponents( m_snap );
			}
		}
		return true;
	}
};

void Scene_SnapToGrid_Component_Selected( scene::Graph& graph, float snap ){
	graph.traverse( ComponentSnappableSnapToGridSelected( snap ) );
}

void Selection_SnapToGrid(){
	const auto command = StringStream<64>( "snapSelected -grid ", GetGridSize() );
	UndoableCommand undo( command );

	if ( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent && GlobalSelectionSystem().countSelectedComponents() ) {
		Scene_SnapToGrid_Component_Selected( GlobalSceneGraph(), GetGridSize() );
	}
	else
	{
		Scene_SnapToGrid_Selected( GlobalSceneGraph(), GetGridSize() );
	}
}



#include <QWidget>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include "gtkutil/spinbox.h"


class RotateDialog : public QObject
{
	QWidget *m_window{};
	QDoubleSpinBox *m_x;
	QDoubleSpinBox *m_y;
	QDoubleSpinBox *m_z;
	void construct(){
		m_window = new QWidget( MainFrame_getWindow(), Qt::Tool | Qt::WindowCloseButtonHint );
		m_window->setWindowTitle( "Arbitrary rotation" );
		m_window->installEventFilter( this );

		auto *grid = new QGridLayout( m_window );
		grid->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );

		{
			grid->addWidget( m_x = new DoubleSpinBox( -360, 360, 0, 6, 1, true ), 0, 1 );
			grid->addWidget( m_y = new DoubleSpinBox( -360, 360, 0, 6, 1, true ), 1, 1 );
			grid->addWidget( m_z = new DoubleSpinBox( -360, 360, 0, 6, 1, true ), 2, 1 );
		}
		{
			grid->addWidget( new SpinBoxLabel( "  X  ", m_x ), 0, 0 );
			grid->addWidget( new SpinBoxLabel( "  Y  ", m_y ), 1, 0 );
			grid->addWidget( new SpinBoxLabel( "  Z  ", m_z ), 2, 0 );
		}
		{
			auto *buttons = new QDialogButtonBox( Qt::Orientation::Vertical );
			grid->addWidget( buttons, 0, 2, 3, 1 );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Ok ), &QPushButton::clicked, [this](){ ok(); } );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Cancel ), &QPushButton::clicked, [this](){ cancel(); } );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Apply ), &QPushButton::clicked, [this](){ apply(); } );
		}
	}
	void apply(){
		const Vector3 eulerXYZ( m_x->value(), m_y->value(), m_z->value() );

		const auto command = StringStream<64>( "rotateSelectedEulerXYZ -x ", eulerXYZ[0], " -y ", eulerXYZ[1], " -z ", eulerXYZ[2] );
		UndoableCommand undo( command );

		GlobalSelectionSystem().rotateSelected( quaternion_for_euler_xyz_degrees( eulerXYZ ) );
	}
	void cancel(){
		m_window->hide();

		m_x->setValue( 0 ); // reset to 0 on close
		m_y->setValue( 0 );
		m_z->setValue( 0 );
	}
	void ok(){
		apply();
	//	cancel();
		m_window->hide();
	}
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ) {
			auto *keyEvent = static_cast<QKeyEvent*>( event );
			if( keyEvent->key() == Qt::Key_Escape ){
				cancel();
				event->accept();
			}
			else if( keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ){
				ok();
				event->accept();
			}
			else if( keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Space ){
				event->accept();
			}
		}
		else if( event->type() == QEvent::Close ) {
			event->ignore();
			cancel();
			return true;
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
public:
	void show(){
		if( m_window == nullptr )
			construct();
		m_window->show();
		m_window->raise();
		m_window->activateWindow();
	}
}
g_rotate_dialog;

void DoRotateDlg(){
	g_rotate_dialog.show();
}



class ScaleDialog : public QObject
{
	QWidget *m_window{};
	QDoubleSpinBox *m_x;
	QDoubleSpinBox *m_y;
	QDoubleSpinBox *m_z;
	void construct(){
		m_window = new QWidget( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
		m_window->setWindowTitle( "Arbitrary scale" );
		m_window->installEventFilter( this );

		auto *grid = new QGridLayout( m_window );
		grid->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );

		{
			grid->addWidget( m_x = new DoubleSpinBox( -32768, 32768, 1, 6, 1, false ), 0, 1 );
			grid->addWidget( m_y = new DoubleSpinBox( -32768, 32768, 1, 6, 1, false ), 1, 1 );
			grid->addWidget( m_z = new DoubleSpinBox( -32768, 32768, 1, 6, 1, false ), 2, 1 );
		}
		{
			grid->addWidget( new SpinBoxLabel( "  X  ", m_x ), 0, 0 );
			grid->addWidget( new SpinBoxLabel( "  Y  ", m_y ), 1, 0 );
			grid->addWidget( new SpinBoxLabel( "  Z  ", m_z ), 2, 0 );
		}
		{
			auto *buttons = new QDialogButtonBox( Qt::Orientation::Vertical );
			grid->addWidget( buttons, 0, 2, 3, 1 );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Ok ), &QPushButton::clicked, [this](){ ok(); } );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Cancel ), &QPushButton::clicked, [this](){ cancel(); } );
			QObject::connect( buttons->addButton( QDialogButtonBox::StandardButton::Apply ), &QPushButton::clicked, [this](){ apply(); } );
		}
	}
	void apply(){
		const float sx = m_x->value(), sy = m_y->value(), sz = m_z->value();

		const auto command = StringStream<64>( "scaleSelected -x ", sx, " -y ", sy, " -z ", sz );
		UndoableCommand undo( command );

		Select_Scale( sx, sy, sz );
	}
	void cancel(){
		m_window->hide();

		m_x->setValue( 1 ); // reset to 1 on close
		m_y->setValue( 1 );
		m_z->setValue( 1 );
	}
	void ok(){
		apply();
	//	cancel();
		m_window->hide();
	}
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ) {
			auto *keyEvent = static_cast<QKeyEvent*>( event );
			if( keyEvent->key() == Qt::Key_Escape ){
				cancel();
				event->accept();
			}
			else if( keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ){
				ok();
				event->accept();
			}
			else if( keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Space ){
				event->accept();
			}
		}
		else if( event->type() == QEvent::Close ) {
			event->ignore();
			cancel();
			return true;
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
public:
	void show(){
		if( m_window == nullptr )
			construct();
		m_window->show();
		m_window->raise();
		m_window->activateWindow();
	}
}
g_scale_dialog;

void DoScaleDlg(){
	g_scale_dialog.show();
}


class EntityGetSelectedPropertyValuesWalker_nonEmpty : public scene::Graph::Walker
{
	PropertyValues& m_propertyvalues;
	const char *m_prop;
	const scene::Node* m_world = Map_FindWorldspawn( g_map );
public:
	EntityGetSelectedPropertyValuesWalker_nonEmpty( const char *prop, PropertyValues& propertyvalues )
		: m_propertyvalues( propertyvalues ), m_prop( prop ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ){
			if( path.top().get_pointer() != m_world ){
				if ( Instance_isSelected( instance ) || instance.childSelected() ) {
					const char* keyvalue = entity->getKeyValue( m_prop );
					if ( !string_empty( keyvalue ) && !propertyvalues_contain( m_propertyvalues, keyvalue ) ) {
						m_propertyvalues.push_back( keyvalue );
					}
				}
			}
			return false;
		}
		return true;
	}
};

void Scene_EntityGetPropertyValues_nonEmpty( scene::Graph& graph, const char *prop, PropertyValues& propertyvalues ){
	graph.traverse( EntityGetSelectedPropertyValuesWalker_nonEmpty( prop, propertyvalues ) );
}

#include "preferences.h"

void Select_ConnectedEntities( bool targeting, bool targets, bool focus ){
	PropertyValues target_propertyvalues;
	PropertyValues targetname_propertyvalues;
	const char *target_prop = "target";
	const char *targetname_prop;
	if ( g_pGameDescription->mGameType == "doom3" ) {
		targetname_prop = "name";
	}
	else{
		targetname_prop = "targetname";
	}

	if( targeting ){
		Scene_EntityGetPropertyValues_nonEmpty( GlobalSceneGraph(), targetname_prop, targetname_propertyvalues );
	}
	if( targets ){
		Scene_EntityGetPropertyValues_nonEmpty( GlobalSceneGraph(), target_prop, target_propertyvalues );
	}

	if( target_propertyvalues.empty() && targetname_propertyvalues.empty() ){
		globalErrorStream() << "SelectConnectedEntities: nothing found\n";
		return;
	}

	if( !targeting || !targets ){
		GlobalSelectionSystem().setSelectedAll( false );
	}
	if ( targeting && !targetname_propertyvalues.empty() ) {
		Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), target_prop, targetname_propertyvalues );
	}
	if ( targets && !target_propertyvalues.empty() ) {
		Scene_EntitySelectByPropertyValues( GlobalSceneGraph(), targetname_prop, target_propertyvalues );
	}
	if( focus ){
		FocusAllViews();
	}
}

void SelectConnectedEntities(){
	Select_ConnectedEntities( true, true, false );
}



void Select_registerCommands(){
	GlobalCommands_insert( "ShowHidden", makeCallbackF( Select_ShowAllHidden ), QKeySequence( "Shift+H" ) );
	GlobalToggles_insert( "HideSelected", makeCallbackF( HideSelected ), ToggleItem::AddCallbackCaller( g_hidden_item ), QKeySequence( "H" ) );

	GlobalCommands_insert( "MirrorSelectionX", makeCallbackF( Selection_Flipx ) );
	GlobalCommands_insert( "RotateSelectionX", makeCallbackF( Selection_Rotatex ) );
	GlobalCommands_insert( "MirrorSelectionY", makeCallbackF( Selection_Flipy ) );
	GlobalCommands_insert( "RotateSelectionY", makeCallbackF( Selection_Rotatey ) );
	GlobalCommands_insert( "MirrorSelectionZ", makeCallbackF( Selection_Flipz ) );
	GlobalCommands_insert( "RotateSelectionZ", makeCallbackF( Selection_Rotatez ) );

	GlobalCommands_insert( "MirrorSelectionHorizontally", makeCallbackF( Selection_FlipHorizontally ) );
	GlobalCommands_insert( "MirrorSelectionVertically", makeCallbackF( Selection_FlipVertically ) );

	GlobalCommands_insert( "RotateSelectionClockwise", makeCallbackF( Selection_RotateClockwise ) );
	GlobalCommands_insert( "RotateSelectionAnticlockwise", makeCallbackF( Selection_RotateAnticlockwise ) );

	GlobalCommands_insert( "SelectTextured", makeCallbackF( Select_FacesAndPatchesByShader_ ), QKeySequence( "Ctrl+Shift+A" ) );

	GlobalCommands_insert( "Undo", makeCallbackF( Undo ), QKeySequence( "Ctrl+Z" ) );
	GlobalCommands_insert( "Redo", makeCallbackF( Redo ), QKeySequence( "Ctrl+Shift+Z" ) );
	GlobalCommands_insert( "Redo2", makeCallbackF( Redo ), QKeySequence( "Ctrl+Y" ) );
	GlobalCommands_insert( "Copy", makeCallbackF( Copy ), QKeySequence( "Ctrl+C" ) );
	GlobalCommands_insert( "Paste", makeCallbackF( Paste ), QKeySequence( "Ctrl+V" ) );
	GlobalCommands_insert( "PasteToCamera", makeCallbackF( PasteToCamera ), QKeySequence( "Shift+V" ) );
	GlobalCommands_insert( "MoveToCamera", makeCallbackF( MoveToCamera ), QKeySequence( "Ctrl+Shift+V" ) );
	GlobalCommands_insert( "CloneSelection", makeCallbackF( Selection_Clone ), QKeySequence( "Space" ) );
	GlobalCommands_insert( "CloneSelectionAndMakeUnique", makeCallbackF( Selection_Clone_MakeUnique ), QKeySequence( "Shift+Space" ) );
	GlobalCommands_insert( "CreateLinkedDuplicate", makeCallbackF( LinkedGroups_CreateLinkedDuplicate ) );
	GlobalCommands_insert( "SelectLinkedGroups", makeCallbackF( LinkedGroups_SelectLinkedGroups ) );
	GlobalCommands_insert( "SeparateLinkedGroups", makeCallbackF( LinkedGroups_SeparateSelectedLinkedGroups ) );
	GlobalCommands_insert( "DeleteSelection3", makeCallbackF( deleteSelection ), QKeySequence( "Delete" ) );
	GlobalCommands_insert( "DeleteSelection2", makeCallbackF( deleteSelection ), QKeySequence( "Backspace" ) );
	GlobalCommands_insert( "DeleteSelection", makeCallbackF( deleteSelection ), QKeySequence( "Z" ) );
	GlobalCommands_insert( "RepeatTransforms", makeCallbackF( +[](){ GlobalSelectionSystem().repeatTransforms(); } ), QKeySequence( "Ctrl+R" ) );
	GlobalCommands_insert( "ResetTransforms", makeCallbackF( +[](){ GlobalSelectionSystem().resetTransforms(); } ), QKeySequence( "Alt+R" ) );
//	GlobalCommands_insert( "ParentSelection", makeCallbackF( Scene_parentSelected ) );
	GlobalCommands_insert( "UnSelectSelection2", makeCallbackF( Selection_Deselect ), QKeySequence( "Escape" ) );
	GlobalCommands_insert( "UnSelectSelection", makeCallbackF( Selection_Deselect ), QKeySequence( "C" ) );
	GlobalCommands_insert( "InvertSelection", makeCallbackF( Select_Invert ), QKeySequence( "I" ) );
	GlobalCommands_insert( "SelectInside", makeCallbackF( Select_Inside ) );
	GlobalCommands_insert( "SelectTouching", makeCallbackF( Select_Touching ) );
	GlobalCommands_insert( "SelectTouchingTall", makeCallbackF( Select_TouchingTall ) );
	GlobalCommands_insert( "ExpandSelectionToPrimitives", makeCallbackF( Scene_ExpandSelectionToPrimitives ), QKeySequence( "Ctrl+E" ) );
	GlobalCommands_insert( "ExpandSelectionToEntities", makeCallbackF( Scene_ExpandSelectionToEntities ), QKeySequence( "Shift+E" ) );
	GlobalCommands_insert( "SelectConnectedEntities", makeCallbackF( SelectConnectedEntities ), QKeySequence( "Ctrl+Shift+E" ) );

	GlobalCommands_insert( "ArbitraryRotation", makeCallbackF( DoRotateDlg ), QKeySequence( "Shift+R" ) );
	GlobalCommands_insert( "ArbitraryScale", makeCallbackF( DoScaleDlg ), QKeySequence( "Ctrl+Shift+S" ) );

	GlobalCommands_insert( "SnapToGrid", makeCallbackF( Selection_SnapToGrid ), QKeySequence( "Ctrl+G" ) );

	GlobalCommands_insert( "SelectAllOfType", makeCallbackF( Select_AllOfType ), QKeySequence( "Shift+A" ) );

	GlobalCommands_insert( "TexRotateClock", makeCallbackF( Texdef_RotateClockwise ), QKeySequence( "Shift+PgDown" ) );
	GlobalCommands_insert( "TexRotateCounter", makeCallbackF( Texdef_RotateAntiClockwise ), QKeySequence( "Shift+PgUp" ) );
	GlobalCommands_insert( "TexScaleUp", makeCallbackF( Texdef_ScaleUp ), QKeySequence( "Ctrl+Up" ) );
	GlobalCommands_insert( "TexScaleDown", makeCallbackF( Texdef_ScaleDown ), QKeySequence( "Ctrl+Down" ) );
	GlobalCommands_insert( "TexScaleLeft", makeCallbackF( Texdef_ScaleLeft ), QKeySequence( "Ctrl+Left" ) );
	GlobalCommands_insert( "TexScaleRight", makeCallbackF( Texdef_ScaleRight ), QKeySequence( "Ctrl+Right" ) );
	GlobalCommands_insert( "TexShiftUp", makeCallbackF( Texdef_ShiftUp ), QKeySequence( "Shift+Up" ) );
	GlobalCommands_insert( "TexShiftDown", makeCallbackF( Texdef_ShiftDown ), QKeySequence( "Shift+Down" ) );
	GlobalCommands_insert( "TexShiftLeft", makeCallbackF( Texdef_ShiftLeft ), QKeySequence( "Shift+Left" ) );
	GlobalCommands_insert( "TexShiftRight", makeCallbackF( Texdef_ShiftRight ), QKeySequence( "Shift+Right" ) );

	GlobalCommands_insert( "MoveSelectionDOWN", makeCallbackF( Selection_MoveDown ), QKeySequence( QKeyCombination( Qt::KeypadModifier, Qt::Key_Minus ) ) );
	GlobalCommands_insert( "MoveSelectionUP", makeCallbackF( Selection_MoveUp ), QKeySequence( QKeyCombination( Qt::KeypadModifier, Qt::Key_Plus ) ) );

	GlobalCommands_insert( "SelectNudgeLeft", makeCallbackF( Selection_NudgeLeft ), QKeySequence( "Alt+Left" ) );
	GlobalCommands_insert( "SelectNudgeRight", makeCallbackF( Selection_NudgeRight ), QKeySequence( "Alt+Right" ) );
	GlobalCommands_insert( "SelectNudgeUp", makeCallbackF( Selection_NudgeUp ), QKeySequence( "Alt+Up" ) );
	GlobalCommands_insert( "SelectNudgeDown", makeCallbackF( Selection_NudgeDown ), QKeySequence( "Alt+Down" ) );
}



void SceneSelectionChange( const Selectable& selectable ){
	SceneChangeNotify();
}

SignalHandlerId Selection_boundsChanged;

#include "preferencesystem.h"

void Nudge_constructPreferences( PreferencesPage& page ){
	page.appendCheckBox( "", "Nudge selected after duplication", g_bNudgeAfterClone );
}

void Selection_construct(){
	GlobalPreferenceSystem().registerPreference( "NudgeAfterClone", BoolImportStringCaller( g_bNudgeAfterClone ), BoolExportStringCaller( g_bNudgeAfterClone ) );

	PreferencesDialog_addSettingsPreferences( makeCallbackF( Nudge_constructPreferences ) );

	GlobalSelectionSystem().addSelectionChangeCallback( FreeCaller<void(const Selectable&), SceneSelectionChange>() );
	GlobalSelectionSystem().addSelectionChangeCallback( FreeCaller<void(const Selectable&), UpdateWorkzone_ForSelectionChanged>() );
	Selection_boundsChanged = GlobalSceneGraph().addBoundsChangedCallback( FreeCaller<void(), UpdateWorkzone_ForSelection>() );
}

void Selection_destroy(){
	GlobalSceneGraph().removeBoundsChangedCallback( Selection_boundsChanged );
}
