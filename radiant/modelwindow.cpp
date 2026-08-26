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

#include "modelwindow.h"

#include <set>
#include <deque>
#include <algorithm>
#include <cmath>
#include <utility>
#include "ifiletypes.h"
#include "ifilesystem.h"
#include "iarchive.h"
#include "imodel.h"
#include "ientity.h"
#include "igl.h"
#include "irender.h"
#include "renderable.h"
#include "renderer.h"
#include "view.h"
#include "os/path.h"
#include "string/string.h"
#include "stringio.h"
#include "stream/stringstream.h"
#include "generic/callback.h"
#include "assetdrop.h"
#include "timer.h"
#include "assetbrowserprefs.h"

#include "gtkutil/glwidget.h"
#include "gtkutil/toolbar.h"
#include "gtkutil/cursor.h"
#include "gtkutil/fbo.h"
#include "gtkutil/mousepresses.h"
#include "gtkutil/guisettings.h"
#include "gtkutil/image.h"
#include "gtkutil/widget.h"

#include <QWidget>
#include <QApplication>
#include <QCursor>
#include <QToolBar>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QMenu>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QTreeView>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QScrollBar>
#include <QOpenGLWidget>
#include <QEvent>
#include <QDrag>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QShowEvent>
#include <QTimer>

#include "mainframe.h"
#include "camwindow.h"
#include "grid.h"
#include "instancelib.h"
#include "referencecache.h"
#include "traverselib.h"
#include "selectionlib.h"
#include "gtkmisc.h"

namespace {
	constexpr int kModelFilterApplyDelayMilliseconds = 150;
	constexpr float kAssetBrowserHoverScale = 1.05f;
	constexpr float kAssetBrowserHoverLerp = 0.2f;
	constexpr float kAssetBrowserHoverEpsilon = 0.001f;
	constexpr float kAssetBrowserHoverRotateDegrees = 12.0f;
	constexpr float kAssetBrowserHoverSpinDegreesPerSecond = 90.0f;

	class ScopedModelPreviewRebuild
	{
		bool& m_inProgress;
	public:
		explicit ScopedModelPreviewRebuild( bool& inProgress ) : m_inProgress( inProgress ) {
			m_inProgress = true;
		}
		~ScopedModelPreviewRebuild(){
			m_inProgress = false;
		}
	};

int AssetBrowser_fontPixelHeight(){
	return OpenGLFont_getPixelHeightSafe();
}

int AssetBrowser_fontPixelDescent(){
	return OpenGLFont_getPixelDescentSafe();
}

float AssetBrowser_approachHoverScale( float current, float target ){
	return current + ( target - current ) * kAssetBrowserHoverLerp;
}

QImage applyOpacity( QImage image, float opacity ){
	if ( image.isNull() ) {
		return image;
	}
	image = image.convertToFormat( QImage::Format_ARGB32 );
	const int alphaScale = std::clamp( static_cast<int>( opacity * 255.0f ), 0, 255 );
	for ( int y = 0; y < image.height(); ++y ) {
		auto* line = reinterpret_cast<QRgb*>( image.scanLine( y ) );
		for ( int x = 0; x < image.width(); ++x ) {
			const int alpha = qAlpha( line[x] );
			line[x] = qRgba( qRed( line[x] ), qGreen( line[x] ), qBlue( line[x] ),
			                 ( alpha * alphaScale ) / 255 );
		}
	}
	return image;
}

bool string_contains_nocase( const char* haystack, const char* needle ){
	if ( string_empty( needle ) ) {
		return true;
	}

	const std::size_t needle_len = string_length( needle );
	for ( const char* cursor = haystack; *cursor != '\0'; ++cursor ) {
		if ( string_equal_nocase_n( cursor, needle, needle_len ) ) {
			return true;
		}
	}
	return false;
}

Vector3 rotatedExtentsForAabb( const AABB& aabb, const Matrix4& rotation ){
	const AABB rotated = aabb_for_oriented_aabb( aabb, rotation );
	return rotated.extents;
}

void AssetBrowser_drawTileLabelBackground( const Vector3& pos, float width, int fontHeight, int fontDescent ){
	const float left = pos.x() - 2.0f;
	const float right = pos.x() + width + 2.0f;
	const float top = pos.z() + 2.0f - fontDescent;
	const float bottom = top - fontHeight - 4.0f;
	gl().glColor4f( 0.0f, 0.0f, 0.0f, 0.85f );
	gl().glBegin( GL_QUADS );
	gl().glVertex3f( left, 0, top );
	gl().glVertex3f( left, 0, bottom );
	gl().glVertex3f( right, 0, bottom );
	gl().glVertex3f( right, 0, top );
	gl().glEnd();
}

void AssetBrowser_drawTileLabelText( const char* text, const Vector3& pos ){
	gl().glColor4f( 0, 0, 0, 1 );
	gl().glRasterPos3f( pos.x() - 1, pos.y(), pos.z() - 1 );
	OpenGLFont_drawStringSafe( text );
	gl().glRasterPos3f( pos.x() + 1, pos.y(), pos.z() + 1 );
	OpenGLFont_drawStringSafe( text );

	gl().glColor4f( 1, 1, 1, 1 );
	gl().glRasterPos3f( pos.x(), pos.y(), pos.z() );
	OpenGLFont_drawStringSafe( text );
	gl().glRasterPos3f( pos.x() + 1, pos.y(), pos.z() );
	OpenGLFont_drawStringSafe( text );
}
} // namespace


/* specialized copy of class CompiledGraph */
class ModelGraph final : public scene::Graph, public scene::Instantiable::Observer
{
	typedef std::map<PathConstReference, scene::Instance*> InstanceMap;

	InstanceMap m_instances;
	scene::Path m_rootpath;

	scene::Instantiable::Observer& m_observer;

public:

	ModelGraph( scene::Instantiable::Observer& observer ) : m_observer( observer ){
	}

	void addSceneChangedCallback( const SignalHandler& handler ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: addSceneChangedCallback()" );
	}
	void sceneChanged() override {
		ASSERT_MESSAGE( 0, "Reached unreachable: sceneChanged()" );
	}

	scene::Node& root() override {
		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );
		return m_rootpath.top();
	}
	void insert_root( scene::Node& root ) override {
		//globalOutputStream() << "insert_root\n";

		ASSERT_MESSAGE( m_rootpath.empty(), "scenegraph root already exists" );

		root.IncRef();

		Node_traverseSubgraph( root, InstanceSubgraphWalker( this, scene::Path(), 0 ) );

		m_rootpath.push( makeReference( root ) );
	}
	void erase_root() override {
		//globalOutputStream() << "erase_root\n";

		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );

		scene::Node& root = m_rootpath.top();

		m_rootpath.pop();

		Node_traverseSubgraph( root, UninstanceSubgraphWalker( this, scene::Path() ) );

		root.DecRef();
	}
	class Layer* currentLayer() override {
		ASSERT_MESSAGE( 0, "Reached unreachable: currentLayer()" );
		return nullptr;
	}
	void boundsChanged() override {
		ASSERT_MESSAGE( 0, "Reached unreachable: boundsChanged()" );
	}

	void traverse( const Walker& walker ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: traverse()" );
	}

	void traverse_subgraph( const Walker& walker, const scene::Path& start ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: traverse_subgraph()" );
	}

	scene::Instance* find( const scene::Path& path ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: find()" );
		return nullptr;
	}

	void insert( scene::Instance* instance ) override {
		m_instances.insert( InstanceMap::value_type( PathConstReference( instance->path() ), instance ) );
		m_observer.insert( instance );
	}
	void erase( scene::Instance* instance ) override {
		m_instances.erase( PathConstReference( instance->path() ) );
		m_observer.erase( instance );
	}

	SignalHandlerId addBoundsChangedCallback( const SignalHandler& boundsChanged ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: addBoundsChangedCallback()" );
		return Handle<Opaque<SignalHandler>>( nullptr );
	}
	void removeBoundsChangedCallback( SignalHandlerId id ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: removeBoundsChangedCallback()" );
	}

	TypeId getNodeTypeId( const char* name ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: getNodeTypeId()" );
		return 0;
	}

	TypeId getInstanceTypeId( const char* name ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: getInstanceTypeId()" );
		return 0;
	}

	void clear(){
		DeleteSubgraph( root() );
	}
};

/* specialized copy of class TraversableNodeSet */
/// \brief A sequence of node references which notifies an observer of inserts and deletions, and uses the global undo system to provide undo for modifications.
class TraversableModelNodeSet : public scene::Traversable
{
	UnsortedNodeSet m_children;
	Observer* m_observer;

	void copy( const TraversableModelNodeSet& other ){
		m_children = other.m_children;
	}
	void notifyInsertAll(){
		if ( m_observer ) {
			for ( auto& node : m_children )
			{
				m_observer->insert( node );
			}
		}
	}
	void notifyEraseAll(){
		if ( m_observer ) {
			for ( auto& node : m_children )
			{
				m_observer->erase( node );
			}
		}
	}
public:
	TraversableModelNodeSet()
		: m_observer( 0 ){
	}
	TraversableModelNodeSet( const TraversableModelNodeSet& other )
		: scene::Traversable( other ), m_observer( 0 ){
		copy( other );
		notifyInsertAll();
	}
	~TraversableModelNodeSet(){
		notifyEraseAll();
	}
	TraversableModelNodeSet& operator=( const TraversableModelNodeSet& other ){
#if 1 // optimised change-tracking using diff algorithm
		if ( m_observer ) {
			nodeset_diff( m_children, other.m_children, m_observer );
		}
		copy( other );
#else
		TraversableModelNodeSet tmp( other );
		tmp.swap( *this );
#endif
		return *this;
	}
	void swap( TraversableModelNodeSet& other ){
		std::swap( m_children, other.m_children );
		std::swap( m_observer, other.m_observer );
	}

	void attach( Observer* observer ){
		ASSERT_MESSAGE( m_observer == 0, "TraversableModelNodeSet::attach: observer cannot be attached" );
		m_observer = observer;
		notifyInsertAll();
	}
	void detach( Observer* observer ){
		ASSERT_MESSAGE( m_observer == observer, "TraversableModelNodeSet::detach: observer cannot be detached" );
		notifyEraseAll();
		m_observer = 0;
	}
	/// \brief \copydoc scene::Traversable::insert()
	void insert( scene::Node& node ) override {
		ASSERT_MESSAGE( reinterpret_cast<intptr_t>( &node ) != 0, "TraversableModelNodeSet::insert: sanity check failed" );

		ASSERT_MESSAGE( m_children.find( NodeSmartReference( node ) ) == m_children.end(), "TraversableModelNodeSet::insert - element already exists" );

		m_children.push_back( NodeSmartReference( node ) );

		if ( m_observer ) {
			m_observer->insert( node );
		}
	}
	/// \brief \copydoc scene::Traversable::erase()
	void erase( scene::Node& node ) override {
		ASSERT_MESSAGE( reinterpret_cast<intptr_t>( &node ) != 0, "TraversableModelNodeSet::erase: sanity check failed" );

		ASSERT_MESSAGE( m_children.find( NodeSmartReference( node ) ) != m_children.end(), "TraversableModelNodeSet::erase - failed to find element" );

		if ( m_observer ) {
			m_observer->erase( node );
		}

		m_children.erase( NodeSmartReference( node ) );
	}
	/// \brief \copydoc scene::Traversable::traverse()
	void traverse( const Walker& walker ) override {
		UnsortedNodeSet::iterator i = m_children.begin();
		while ( i != m_children.end() )
		{
			// post-increment the iterator
			Node_traverseSubgraph( *i++, walker );
			// the Walker can safely remove the current node from
			// this container without invalidating the iterator
		}
	}
	/// \brief \copydoc scene::Traversable::empty()
	bool empty() const override {
		return m_children.empty();
	}
};

/* specialized copy of class MapRoot */
class ModelGraphRoot final : public scene::Node::Symbiot, public scene::Instantiable, public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<ModelGraphRoot, scene::Instantiable>::install( m_casts );
			NodeContainedCast<ModelGraphRoot, scene::Traversable>::install( m_casts );
			NodeContainedCast<ModelGraphRoot, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	IdentityTransform m_transform;
	TraversableModelNodeSet m_traverse;
	InstanceSet m_instances;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_traverse;
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
	}

	ModelGraphRoot() : m_node( this, this, StaticTypeCasts::instance().get(), nullptr ){
		m_node.m_isRoot = true;

		m_traverse.attach( this );
	}
	~ModelGraphRoot() = default;
	void release() override {
		m_traverse.detach( this );
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}

	void insert( scene::Node& child ) override {
		m_instances.insert( child );
	}
	void erase( scene::Node& child ) override {
		m_instances.erase( child );
	}

	scene::Node& clone() const {
		return ( new ModelGraphRoot( *this ) )->node();
	}

	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new SelectableInstance( path, parent );
	}
	void forEachInstance( const scene::Instantiable::Visitor& visitor ) override {
		m_instances.forEachInstance( visitor );
	}
	void insert( scene::Instantiable::Observer* observer, const scene::Path& path, scene::Instance* instance ) override {
		m_instances.insert( observer, path, instance );
	}
	scene::Instance* erase( scene::Instantiable::Observer* observer, const scene::Path& path ) override {
		return m_instances.erase( observer, path );
	}
};





#include "../plugins/entity/model.h"

class ModelNode final :
	public scene::Node::Symbiot,
	public scene::Instantiable,
	public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<ModelNode, scene::Instantiable>::install( m_casts );
			NodeContainedCast<ModelNode, scene::Traversable>::install( m_casts );
			NodeContainedCast<ModelNode, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};


	scene::Node m_node;
	InstanceSet m_instances;
	SingletonModel m_model;
	MatrixTransform m_transform;

	void construct(){
		m_model.attach( this );
	}
	void destroy(){
		m_model.detach( this );
	}

public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_model.getTraversable();
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
	}

	ModelNode() :
		m_node( this, this, StaticTypeCasts::instance().get(), nullptr ){
		construct();
	}
	ModelNode( const ModelNode& other ) :
		scene::Node::Symbiot( other ),
		scene::Instantiable( other ),
		scene::Traversable::Observer( other ),
		m_node( this, this, StaticTypeCasts::instance().get(), nullptr ){
		construct();
	}
	~ModelNode(){
		destroy();
	}

	void release() override {
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}

	void insert( scene::Node& child ) override {
		m_instances.insert( child );
	}
	void erase( scene::Node& child ) override {
		m_instances.erase( child );
	}

	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new SelectableInstance( path, parent );
	}
	void forEachInstance( const scene::Instantiable::Visitor& visitor ) override {
		m_instances.forEachInstance( visitor );
	}
	void insert( scene::Instantiable::Observer* observer, const scene::Path& path, scene::Instance* instance ) override {
		m_instances.insert( observer, path, instance );
	}
	scene::Instance* erase( scene::Instantiable::Observer* observer, const scene::Path& path ) override {
		return m_instances.erase( observer, path );
	}

	void setModel( const char* modelname ){
		m_model.modelChanged( modelname );
	}
};


ModelGraph* g_modelGraph = nullptr;





void ModelGraph_clear(){
	g_modelGraph->clear();
}




class ModelFS
{
public:
	const CopiedString m_folderName;
	ModelFS() = default;
	ModelFS( const StringRange range ) : m_folderName( range ){
	}
	bool operator<( const ModelFS& other ) const {
		return string_less( m_folderName.c_str(), other.m_folderName.c_str() );
	}
	mutable std::set<ModelFS> m_folders;
	mutable std::set<CopiedString> m_files;
	void insert( const char* filepath ) const {
		const char* slash = strchr( filepath, '/' );
		if( slash == nullptr ){
			m_files.emplace( filepath );
		}
		else{
			m_folders.emplace( StringRange( filepath, slash ) ).first->insert( slash + 1 );
		}
	}
};

class CellPos
{
	const int m_cellSize; //half size of model square (radius)
	const int m_fontHeight;
	const int m_fontDescent;
	const int m_plusWidth; //pre offset on the left
	const int m_plusHeight; //above text
	const int m_cellsInRow;

	int m_index = 0;
public:
	CellPos( int width, int cellSize, int fontHeight, int fontDescent ) :
		m_cellSize( cellSize ), m_fontHeight( fontHeight ),
		m_fontDescent( fontDescent ),
		m_plusWidth( 8 ),
		m_plusHeight( 0 ),
		m_cellsInRow( std::max( 1, ( width - m_plusWidth ) / ( m_cellSize * 2 + m_plusWidth ) ) ){
	}
	void operator++(){
		++m_index;
	}
	int index() const {
		return m_index;
	}
	Vector3 getOrigin( int index ) const { // origin of model square
		const int x = ( index % m_cellsInRow ) * m_cellSize * 2 + m_cellSize + ( index % m_cellsInRow + 1 ) * m_plusWidth;
		const int z = ( index / m_cellsInRow ) * m_cellSize * 2 + m_cellSize + ( index / m_cellsInRow + 1 ) * ( m_fontHeight + m_plusHeight );
		return Vector3( x, 0, -z );
	}
	Vector3 getOrigin() const { // origin of model square
		return getOrigin( m_index );
	}
	Vector3 getTextPos( int index ) const {
		const int x = ( index % m_cellsInRow ) * m_cellSize * 2 + ( index % m_cellsInRow + 1 ) * m_plusWidth;
		const int z = ( index / m_cellsInRow ) * m_cellSize * 2 + ( index / m_cellsInRow + 1 ) * ( m_fontHeight + m_plusHeight ) - 1 + m_fontDescent;
		return Vector3( x, 0, -z );
	}
	Vector3 getTextPos() const {
		return getTextPos( m_index );
	}
	int getCellSize() const {
		return m_cellSize;
	}
	int totalHeight( int height, int cellCount ) const {
		return std::max( height, ( ( cellCount - 1 ) / m_cellsInRow + 1 ) * ( m_cellSize * 2 + m_fontHeight + m_plusHeight ) + m_fontHeight );
	}
	int testSelect( int x, int z ) const { // index of cell at ( x, z )
		return std::min( m_cellsInRow - 1, ( x / ( m_cellSize * 2 + m_plusWidth ) ) ) + ( m_cellsInRow * ( z / ( m_cellSize * 2 + m_fontHeight + m_plusHeight ) ) );
	}
};

class ModelBrowser final : public scene::Instantiable::Observer
{
	// track instances in the order of insertion
	std::vector<scene::Instance*> m_modelInstances;
public:
	ModelFS m_modelFS;
	CopiedString m_prefFoldersToLoad = "*models/mapobjects/99*";
	ModelBrowser() : m_scrollAdjustment( [this]( int value ){
		//globalOutputStream() << "vertical scroll\n";
		setOriginZ( -value );
	} )
	{}
	~ModelBrowser() = default;

	const int m_MSAA = 8;
	Vector3 m_background_color = Vector3( .25f );

	QWidget* m_parent = nullptr;
	QOpenGLWidget* m_gl_widget = nullptr;
	QScrollBar* m_gl_scroll = nullptr;
	QTreeView* m_treeView = nullptr;
	QLineEdit* m_filterEntry = nullptr;
	QToolButton* m_globalFilterButton = nullptr;
	QToolButton* m_usedFilterButton = nullptr;
	QToolButton* m_clearFiltersButton = nullptr;
	QTimer* m_filterApplyTimer = nullptr;
	bool m_filterApplyPending = false;
	bool m_previewsDirty = false;
	bool m_previewRefreshReady = true;
	bool m_previewRebuildInProgress = false;
	bool m_referenceRefreshInProgress = false;

	int m_width;
	int m_height;

	int m_originZ = 0; // <= 0
	DeferredAdjustment m_scrollAdjustment;

	int m_cellSize = 80;

	CopiedString m_currentFolderPath;
	const ModelFS* m_currentFolder = nullptr;
	std::vector<CopiedString> m_visibleModelPaths;
	CopiedString m_filter;
	bool m_filterGlobal = false;
	bool m_filterUsed = false;
	int m_currentModelId = -1; // selected model index in currently visible model list
	int m_hoverModelId = -1;
	float m_hoverScale = 1.0f;
	float m_hoverScaleTarget = 1.0f;
	float m_hoverRotate = 0.0f;
	float m_hoverRotateTarget = 0.0f;
	float m_hoverSpin = 0.0f;
	Timer m_hoverSpinTimer;

	CellPos constructCellPos() const {
		return CellPos( m_width, m_cellSize, AssetBrowser_fontPixelHeight(), AssetBrowser_fontPixelDescent() );
	}
	void testSelect( int x, int z ){
		m_currentModelId = constructCellPos().testSelect( x, z - m_originZ );
		if( m_currentModelId >= static_cast<int>( m_modelInstances.size() ) )
			m_currentModelId = -1;
	}
	void updateHover( int x, int z ){
		setHoverId( constructCellPos().testSelect( x, z - m_originZ ) );
	}
	void clearHover(){
		setHoverId( -1 );
	}
	CopiedString modelPathForIndex( int index ) const {
		if ( index < 0 || index >= static_cast<int>( m_visibleModelPaths.size() ) ) {
			return CopiedString( "" );
		}
		return m_visibleModelPaths[index];
	}
	const char* modelLabelForIndex( int index ) const {
		if ( index < 0 || index >= static_cast<int>( m_visibleModelPaths.size() ) ) {
			return "";
		}
		const char* path = m_visibleModelPaths[index].c_str();
		return m_filterGlobal ? path : path_get_filename_start( path );
	}
	int hoverModelId() const {
		return m_hoverModelId;
	}
	float hoverScale() const {
		return m_hoverScale;
	}
	float hoverScaleForIndex( int index ) const {
		return index == m_hoverModelId ? m_hoverScale : 1.0f;
	}
	float hoverRotationForIndex( int index ) const {
		return index == m_hoverModelId ? ( m_hoverRotate + m_hoverSpin ) : 0.0f;
	}
	float hoverAlpha() const {
		if ( m_hoverModelId < 0 ) {
			return 0.0f;
		}
		return std::clamp( ( m_hoverScale - 1.0f ) / ( kAssetBrowserHoverScale - 1.0f ), 0.0f, 1.0f );
	}
	bool updateHoverAnimation(){
		const float previousSpin = m_hoverSpin;
		if ( m_hoverModelId < 0 ) {
			const bool rotateChanged = std::fabs( m_hoverRotate ) > kAssetBrowserHoverEpsilon;
			const bool spinChanged = std::fabs( m_hoverSpin ) > kAssetBrowserHoverEpsilon;
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			m_hoverRotate = 0.0f;
			m_hoverRotateTarget = 0.0f;
			m_hoverSpin = 0.0f;
			return rotateChanged || spinChanged;
		}
		if ( m_hoverModelId >= static_cast<int>( m_modelInstances.size() ) ) {
			const bool rotateChanged = std::fabs( m_hoverRotate ) > kAssetBrowserHoverEpsilon;
			const bool spinChanged = std::fabs( m_hoverSpin ) > kAssetBrowserHoverEpsilon;
			m_hoverModelId = -1;
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			m_hoverRotate = 0.0f;
			m_hoverRotateTarget = 0.0f;
			m_hoverSpin = 0.0f;
			return rotateChanged || spinChanged;
		}
		const float previous = m_hoverScale;
		const float previousRotate = m_hoverRotate;
		m_hoverScale = AssetBrowser_approachHoverScale( m_hoverScale, m_hoverScaleTarget );
		m_hoverRotate = AssetBrowser_approachHoverScale( m_hoverRotate, m_hoverRotateTarget );
		const bool scaleSettled = std::fabs( m_hoverScale - m_hoverScaleTarget ) < kAssetBrowserHoverEpsilon;
		const bool rotateSettled = std::fabs( m_hoverRotate - m_hoverRotateTarget ) < kAssetBrowserHoverEpsilon;
		if ( scaleSettled ) {
			m_hoverScale = m_hoverScaleTarget;
		}
		if ( rotateSettled ) {
			m_hoverRotate = m_hoverRotateTarget;
		}
		const bool spinActive = m_hoverScaleTarget > 1.0f + kAssetBrowserHoverEpsilon;
		if ( spinActive ) {
			m_hoverSpin = static_cast<float>(
				std::fmod( m_hoverSpinTimer.elapsed_sec() * kAssetBrowserHoverSpinDegreesPerSecond, 360.0 ) );
		}
		else
		{
			m_hoverSpin = 0.0f;
		}
		const bool spinChanged = std::fabs( m_hoverSpin - previousSpin ) > kAssetBrowserHoverEpsilon;
		if ( spinActive || !( scaleSettled && rotateSettled ) || spinChanged ) {
			queueDraw();
		}
		if ( m_hoverScaleTarget <= 1.0f + kAssetBrowserHoverEpsilon
		  && m_hoverScale <= 1.0f + kAssetBrowserHoverEpsilon ) {
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			m_hoverModelId = -1;
			m_hoverRotate = 0.0f;
			m_hoverRotateTarget = 0.0f;
			m_hoverSpin = 0.0f;
		}
		return ( std::fabs( m_hoverScale - previous ) > kAssetBrowserHoverEpsilon )
			|| ( std::fabs( m_hoverRotate - previousRotate ) > kAssetBrowserHoverEpsilon )
			|| spinChanged
			|| spinActive;
	}
private:
	void setHoverId( int hoverId ){
		if ( hoverId >= static_cast<int>( m_modelInstances.size() ) ) {
			hoverId = -1;
		}
		if ( hoverId < 0 ) {
			if ( m_hoverModelId >= 0 && m_hoverScaleTarget != 1.0f ) {
				m_hoverScaleTarget = 1.0f;
				m_hoverRotateTarget = 0.0f;
				m_hoverRotate = 0.0f;
				m_hoverSpin = 0.0f;
				queueDraw();
			}
			return;
		}

		const bool idChanged = hoverId != m_hoverModelId;
		if ( idChanged ) {
			m_hoverModelId = hoverId;
			m_hoverScale = 1.0f;
			m_hoverRotate = 0.0f;
			m_hoverSpin = 0.0f;
		}
		if ( idChanged || m_hoverScaleTarget != kAssetBrowserHoverScale ) {
			m_hoverScaleTarget = kAssetBrowserHoverScale;
			m_hoverRotateTarget = kAssetBrowserHoverRotateDegrees;
			m_hoverSpinTimer.start();
			m_hoverSpin = 0.0f;
			queueDraw();
		}
	}
	int totalHeight() const {
		return constructCellPos().totalHeight( m_height, m_modelInstances.size() );
	}
	void updateScroll() const {
		if ( m_gl_scroll == nullptr ) {
			return;
		}
		m_gl_scroll->setMinimum( 0 );
		m_gl_scroll->setMaximum( totalHeight() - m_height );
		m_gl_scroll->setValue( -m_originZ );
		m_gl_scroll->setPageStep( m_height );
		m_gl_scroll->setSingleStep( 20 );
	}
public:
	void setOriginZ( int origin ){
		m_originZ = origin;
		m_originInvalid = true;
		validate(); // do updateScroll() immediately here; calling it in render() may call setOriginZ() again with old value
		queueDraw();
	}
	void queueDraw() const {
		if ( m_gl_widget != nullptr )
			widget_queue_draw( *m_gl_widget );
	}
	bool m_originInvalid = true;
	void validate(){
		if( m_originInvalid ){
			m_originInvalid = false;
			const int lowest = std::min( m_height - totalHeight(), 0 );
			m_originZ = std::max( lowest, std::min( m_originZ, 0 ) );
			updateScroll();
		}
	}

private:
	void trackingDelta( int x, int y, const QMouseEvent *event ){
		m_move_amount += std::abs( x ) + std::abs( y );
		if ( event->buttons() & Qt::MouseButton::RightButton && y != 0 ) { // scroll view
			const int scale = event->modifiers().testFlag( Qt::KeyboardModifier::ShiftModifier )? 4 : 1;
			setOriginZ( m_originZ + y * scale );
		}
		else if ( event->buttons() & Qt::MouseButton::LeftButton && ( x != 0 || y != 0 ) && m_currentModelId >= 0 ) { // rotate selected model
			ASSERT_MESSAGE( m_currentModelId < static_cast<int>( m_modelInstances.size() ), "modelBrowser.m_currentModelId out of range" );
			scene::Instance *instance = m_modelInstances[m_currentModelId];
			if( TransformNode *transformNode = Node_getTransformNode( instance->path().parent() ) ){
				Matrix4 rot( g_matrix4_identity );
				matrix4_pivoted_rotate_by_euler_xyz_degrees( rot, Vector3( y, 0, x ) * ( 45.f / m_cellSize ), constructCellPos().getOrigin( m_currentModelId ) );
				matrix4_premultiply_by_matrix4( const_cast<Matrix4&>( transformNode->localToParent() ), rot );
				instance->parent()->transformChangedLocal();
				instance->transformChangedLocal();
				queueDraw();
			}
		}
	}
	FreezePointer m_freezePointer;
	bool m_move_started = false;
public:
	int m_move_amount;
	void tracking_MouseUp(){
		if( m_move_started ){
			m_move_started = false;
			m_freezePointer.unfreeze_pointer( false );
		}
	}
	void tracking_MouseDown(){
		tracking_MouseUp();
		m_move_started = true;
		m_move_amount = 0;
		m_freezePointer.freeze_pointer( m_gl_widget,
			[this]( int x, int y, const QMouseEvent *event ){
				trackingDelta( x, y, event );
			},
			[this](){
				tracking_MouseUp();
			} );
	}

	void insert( scene::Instance* instance ) override {
		if( instance->path().size() == 3 ){
			m_modelInstances.push_back( instance );
			m_originZ = 0;
			m_originInvalid = true;
		}
	}
	void erase( scene::Instance* instance ) override { // just invalidate everything (also happens on resource flush and refresh) //FIXME: redraw on resource refresh
		if ( m_referenceRefreshInProgress ) {
			m_previewsDirty = true;
		}
		m_modelInstances.clear();
		m_visibleModelPaths.clear();
		m_originZ = 0;
		m_originInvalid = true;
	}
	template<typename Functor>
	void forEachModelInstance( const Functor& functor ) const {
		for( scene::Instance* instance : m_modelInstances )
			functor( instance );
	}
	void setFilter( const char* filter ){
		m_filter = filter;
	}
	const char* filter() const {
		return m_filter.c_str();
	}
};

ModelBrowser g_ModelBrowser;
static bool g_modelBrowserTreeConstructed = false;

using ModelPathSet = std::set<CopiedString, StringLessNoCase>;

class ModelBrowserFileTypeCollector : public IFileTypeList
{
public:
	ModelPathSet m_extensions;
	void addType( const char* moduleName, filetype_t type ) override {
		m_extensions.emplace( moduleName );
	}
};

bool ModelBrowser_normalizeModelPath( const char* value, const ModelPathSet& modelExtensions, CopiedString& outPath ){
	if ( string_empty( value ) ) {
		return false;
	}
	const auto cleaned = StringStream<256>( PathCleaned( value ) );
	if ( !string_equal_prefix_nocase( cleaned.c_str(), "models/" ) ) {
		return false;
	}
	if ( !modelExtensions.contains( path_get_extension( cleaned.c_str() ) ) ) {
		return false;
	}
	outPath = cleaned.c_str();
	return true;
}

class ModelBrowserEntityModelCollector : public Entity::Visitor
{
	const ModelPathSet& m_modelExtensions;
	ModelPathSet& m_usedModels;
public:
	ModelBrowserEntityModelCollector( const ModelPathSet& modelExtensions, ModelPathSet& usedModels )
		: m_modelExtensions( modelExtensions ), m_usedModels( usedModels ){
	}
	void visit( const char* key, const char* value ) override {
		(void)key;
		CopiedString modelPath;
		if ( ModelBrowser_normalizeModelPath( value, m_modelExtensions, modelPath ) ) {
			m_usedModels.emplace( modelPath );
		}
	}
};

class ModelBrowserUsedModelWalker : public scene::Graph::Walker
{
	const ModelPathSet& m_modelExtensions;
	ModelPathSet& m_usedModels;
public:
	ModelBrowserUsedModelWalker( const ModelPathSet& modelExtensions, ModelPathSet& usedModels )
		: m_modelExtensions( modelExtensions ), m_usedModels( usedModels ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		if ( Entity* entity = Node_getEntity( path.top() ) ) {
			ModelBrowserEntityModelCollector collector( m_modelExtensions, m_usedModels );
			entity->forEachKeyValue( collector );
		}
		return true;
	}
};

ModelPathSet ModelBrowser_collectUsedModels(){
	ModelBrowserFileTypeCollector typelist;
	GlobalFiletypes().getTypeList( ModelLoader::Name, &typelist, true, false, false );

	ModelPathSet usedModels;
	GlobalSceneGraph().traverse( ModelBrowserUsedModelWalker( typelist.m_extensions, usedModels ) );
	return usedModels;
}

void ModelBrowser_collectAllFolderFiles( const ModelFS& folder, const char* prefix, std::vector<CopiedString>& files ){
	for ( const CopiedString& file : folder.m_files ) {
		files.emplace_back( StringStream<256>( prefix, file.c_str() ) );
	}
	for ( const ModelFS& subfolder : folder.m_folders ) {
		const auto nextPrefix = StringStream<256>( prefix, subfolder.m_folderName.c_str(), '/' );
		ModelBrowser_collectAllFolderFiles( subfolder, nextPrefix.c_str(), files );
	}
}

void ModelBrowser_updateClearFiltersButton(){
	if ( g_ModelBrowser.m_clearFiltersButton == nullptr ) {
		return;
	}
	const bool active = !string_empty( g_ModelBrowser.filter() )
	                 || g_ModelBrowser.m_filterGlobal
	                 || g_ModelBrowser.m_filterUsed;
	g_ModelBrowser.m_clearFiltersButton->setEnabled( active );
}

void ModelBrowser_updateUsedFilterButtonLabel( std::size_t usedCount ){
	if ( g_ModelBrowser.m_usedFilterButton != nullptr ) {
		g_ModelBrowser.m_usedFilterButton->setText( StringStream<64>( "Used (", usedCount, ')' ).c_str() );
	}
}

void ModelBrowser_cancelPendingFilterApply(){
	if ( g_ModelBrowser.m_filterApplyTimer != nullptr ) {
		g_ModelBrowser.m_filterApplyTimer->stop();
	}
	g_ModelBrowser.m_filterApplyPending = false;
}

void ModelBrowser_pausePendingFilterApply(){
	if ( g_ModelBrowser.m_filterApplyTimer != nullptr ) {
		g_ModelBrowser.m_filterApplyTimer->stop();
	}
}

void ModelBrowser_scheduleFilterApply(){
	g_ModelBrowser.m_filterApplyPending = true;
	if ( g_ModelBrowser.m_filterApplyTimer != nullptr && !g_ModelBrowser.m_referenceRefreshInProgress ) {
		g_ModelBrowser.m_filterApplyTimer->start( kModelFilterApplyDelayMilliseconds );
	}
}

void ModelBrowser_rebuildVisibleModels();

static QRect ModelBrowser_cellRectPixels( const ModelBrowser& browser, int index ){
	const CellPos cellPos = browser.constructCellPos();
	const Vector3 origin = cellPos.getOrigin( index );
	const float cellSize = cellPos.getCellSize();
	const float minx = origin.x() - cellSize;
	const float maxx = origin.x() + cellSize;
	const float minz = origin.z() - cellSize;
	const float maxz = origin.z() + cellSize;
	const int x = float_to_integer( minx );
	const int y = float_to_integer( browser.m_originZ - maxz );
	const int w = float_to_integer( maxx - minx );
	const int h = float_to_integer( maxz - minz );
	return QRect( x, y, w, h );
}

static QPixmap ModelBrowser_dragPixmap( ModelBrowser& browser ){
	if ( browser.m_gl_widget == nullptr || browser.m_currentModelId < 0 ) {
		return QPixmap();
	}
	QImage frame = browser.m_gl_widget->grabFramebuffer();
	QRect rect = ModelBrowser_cellRectPixels( browser, browser.m_currentModelId ).intersected( frame.rect() );
	if ( rect.isEmpty() ) {
		return QPixmap();
	}
	QImage tile = applyOpacity( frame.copy( rect ), 0.6f );
	QPixmap pixmap = QPixmap::fromImage( tile );
	pixmap.setDevicePixelRatio( browser.m_gl_widget->devicePixelRatioF() );
	return pixmap;
}





static float ModelBrowser_maxExtent(){
	float maxExtent = 0.0f;
	const Matrix4 rotation = matrix4_rotation_for_euler_xyz_degrees( AssetBrowser_defaultAngles() );
	g_ModelBrowser.forEachModelInstance( [&]( scene::Instance* instance ){
		if( Bounded *bounded = Instance_getBounded( *instance ) ){
			AABB aabb = bounded->localAABB();
			if ( !aabb_valid( aabb ) ) {
				return;
			}
			const Vector3 extents = rotatedExtentsForAabb( aabb, rotation );
			const float extent = std::max( extents[0], extents[2] );
			if ( extent > 0.0f ) {
				maxExtent = std::max( maxExtent, extent );
			}
		}
	} );
	return maxExtent > 0.0f ? maxExtent : 1.0f;
}

static float ModelBrowser_baseScale(){
	const float maxExtent = ModelBrowser_maxExtent();
	return maxExtent > 0.0f
	     ? static_cast<float>( g_ModelBrowser.m_cellSize ) / ( maxExtent * kAssetBrowserHoverScale )
	     : 1.0f;
}

class models_set_transforms
{
	const float m_baseScale;
	mutable CellPos m_cellPos = g_ModelBrowser.constructCellPos();
public:
	explicit models_set_transforms( float baseScale ) : m_baseScale( baseScale ){
	}
	void operator()( scene::Instance* instance ) const {
		if( TransformNode *transformNode = Node_getTransformNode( instance->path().parent() ) ){
			if( Bounded *bounded = Instance_getBounded( *instance ) ){
				AABB aabb = bounded->localAABB();
				if ( !aabb_valid( aabb ) ) {
					aabb = AABB( g_vector3_identity, Vector3( 1, 1, 1 ) );
				}
				const int index = m_cellPos.index();
				const float scale = m_baseScale * g_ModelBrowser.hoverScaleForIndex( index );
				const float hoverRotate = g_ModelBrowser.hoverRotationForIndex( index );
				const Matrix4 baseRotation = matrix4_rotation_for_euler_xyz_degrees( AssetBrowser_defaultAngles() );
				const Matrix4 rotation = ( std::fabs( hoverRotate ) > 0.0f )
					? matrix4_multiplied_by_matrix4( baseRotation, matrix4_rotation_for_z_degrees( hoverRotate ) )
					: baseRotation;
				const_cast<Matrix4&>( transformNode->localToParent() ) =
				        matrix4_multiplied_by_matrix4(
				            matrix4_translation_for_vec3( m_cellPos.getOrigin() ),
				            matrix4_multiplied_by_matrix4(
				                rotation,
				                matrix4_multiplied_by_matrix4(
				                    matrix4_scale_for_vec3( Vector3( scale, scale, scale ) ),
				                    matrix4_translation_for_vec3( -aabb.origin )
				                )
				            )
				        );
				instance->parent()->transformChangedLocal();
				instance->transformChangedLocal();
//%		globalOutputStream() << transformNode->localToParent() << " transformNode->localToParent()\n";
				++m_cellPos;
			}
		}
	}
};

void ModelBrowser_rebuildVisibleModels(){
	if ( !g_ModelBrowser.m_previewRefreshReady ) {
		return;
	}
	if ( g_ModelBrowser.m_referenceRefreshInProgress ) {
		g_ModelBrowser.m_filterApplyPending = true;
		return;
	}
	if ( g_ModelBrowser.m_previewRebuildInProgress ) {
		ModelBrowser_scheduleFilterApply();
		return;
	}
	const bool forceReload = std::exchange( g_ModelBrowser.m_previewsDirty, false );
	ScopedModelPreviewRebuild rebuildGuard( g_ModelBrowser.m_previewRebuildInProgress );
	ModelBrowser_cancelPendingFilterApply();

	std::vector<CopiedString> candidates;
	if ( g_ModelBrowser.m_filterGlobal ) {
		ModelBrowser_collectAllFolderFiles( g_ModelBrowser.m_modelFS, "", candidates );
	}
	else if ( g_ModelBrowser.m_currentFolder != nullptr ) {
		for ( const CopiedString& file : g_ModelBrowser.m_currentFolder->m_files ) {
			candidates.emplace_back( StringStream<256>( g_ModelBrowser.m_currentFolderPath.c_str(), file.c_str() ) );
		}
	}

	ModelPathSet usedModels;
	if ( g_ModelBrowser.m_filterUsed ) {
		usedModels = ModelBrowser_collectUsedModels();
		ModelBrowser_updateUsedFilterButtonLabel( usedModels.size() );
	}
	const char* filter = g_ModelBrowser.filter();
	std::vector<CopiedString> visibleModelPaths;
	visibleModelPaths.reserve( candidates.size() );

	for ( const CopiedString& path : candidates ) {
		const char* pathText = path.c_str();
		if ( !string_contains_nocase( pathText, filter ) ) {
			continue;
		}
		if ( g_ModelBrowser.m_filterUsed && !usedModels.contains( pathText ) ) {
			continue;
		}
		visibleModelPaths.push_back( path );
	}

	if ( !forceReload && visibleModelPaths == g_ModelBrowser.m_visibleModelPaths ) {
		ModelBrowser_updateClearFiltersButton();
		g_ModelBrowser.queueDraw();
		return;
	}

	ModelGraph_clear();
	g_ModelBrowser.m_visibleModelPaths = std::move( visibleModelPaths );
	g_ModelBrowser.m_currentModelId = -1;
	g_ModelBrowser.clearHover();

	if ( scene::Traversable* traversable = Node_getTraversable( g_modelGraph->root() ) ) {
		const char* status = g_ModelBrowser.m_filterGlobal ? "All Objects" : g_ModelBrowser.m_currentFolderPath.c_str();
		ScopeDisableScreenUpdates disableScreenUpdates( status, "Loading Objects" );
		for ( const CopiedString& path : g_ModelBrowser.m_visibleModelPaths ) {
			auto *modelNode = new ModelNode;
			modelNode->setModel( path.c_str() );
			NodeSmartReference node( modelNode->node() );
			traversable->insert( node );
		}
		g_ModelBrowser.forEachModelInstance( models_set_transforms( ModelBrowser_baseScale() ) );
	}

	g_ModelBrowser.m_originZ = 0;
	g_ModelBrowser.m_originInvalid = true;
	ModelBrowser_updateClearFiltersButton();
	g_ModelBrowser.queueDraw();
}


class ModelRenderer : public Renderer
{
	struct state_type
	{
		state_type() :
			m_state( 0 ){
		}
		Shader* m_state;
	};
public:
	ModelRenderer( RenderStateFlags globalstate ) :
		m_globalstate( globalstate ){
		m_state_stack.push_back( state_type() );
	}

	void SetState( Shader* state, EStyle style ) override {
		ASSERT_NOTNULL( state );
		if ( style == eFullMaterials ) {
			m_state_stack.back().m_state = state;
		}
	}
	EStyle getStyle() const override {
		return eFullMaterials;
	}
	void PushState() override {
		m_state_stack.push_back( m_state_stack.back() );
	}
	void PopState() override {
		ASSERT_MESSAGE( !m_state_stack.empty(), "popping empty stack" );
		m_state_stack.pop_back();
	}
	void Highlight( EHighlightMode mode, bool bEnable = true ) override {
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& localToWorld ) override {
		m_state_stack.back().m_state->addRenderable( renderable, localToWorld );
	}

	void render( const Matrix4& modelview, const Matrix4& projection ){
		GlobalShaderCache().render( m_globalstate, modelview, projection );
	}
private:
	std::vector<state_type> m_state_stack;
	RenderStateFlags m_globalstate;
};

/*
x=0, z<=0
origin -----> +x
      |  --
      | |  |
      |  --
      \/ -z
*/
void ModelBrowser_render(){
	g_ModelBrowser.validate();
	const bool hoverChanged = g_ModelBrowser.updateHoverAnimation();
	if ( hoverChanged ) {
		g_ModelBrowser.forEachModelInstance( models_set_transforms( ModelBrowser_baseScale() ) );
	}

	const int W = g_ModelBrowser.m_width;
	const int H = g_ModelBrowser.m_height;
	gl().glViewport( 0, 0, W, H );

	// enable depth buffer writes
	gl().glDepthMask( GL_TRUE );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	gl().glClearColor( g_ModelBrowser.m_background_color[0],
	                   g_ModelBrowser.m_background_color[1],
	                   g_ModelBrowser.m_background_color[2], 0 );
	gl().glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	const unsigned int globalstate = RENDER_DEPTHTEST
	                               | RENDER_COLOURWRITE
	                               | RENDER_DEPTHWRITE
	                               | RENDER_ALPHATEST
	                               | RENDER_BLEND
	                               | RENDER_CULLFACE
	                               | RENDER_COLOURARRAY
	                               | RENDER_FOG
	                               | RENDER_COLOURCHANGE
	                               | RENDER_FILL
	                               | RENDER_LIGHTING
	                               | RENDER_TEXTURE
	                               | RENDER_SMOOTH
	                               | RENDER_SCALED;


	Matrix4 m_projection;

	m_projection[0] = 1.0f / ( W / 2.f );
	m_projection[5] = 1.0f / ( H / 2.f );
	m_projection[10] = 1.0f / ( 9999 );

	m_projection[12] = 0;
	m_projection[13] = 0;
	m_projection[14] = -1;

	m_projection[1] = m_projection[2] = m_projection[3] =
	m_projection[4] = m_projection[6] = m_projection[7] =
	m_projection[8] = m_projection[9] = m_projection[11] = 0;

	m_projection[15] = 1;


	Matrix4 m_modelview;
	// translation
	m_modelview[12] = -W / 2.f;
	m_modelview[13] = H / 2.f - g_ModelBrowser.m_originZ;
	m_modelview[14] = 9999;

	// axis base
	//XZ:
	m_modelview[0]  =  1;
	m_modelview[1]  =  0;
	m_modelview[2]  =  0;

	m_modelview[4]  =  0;
	m_modelview[5]  =  0;
	m_modelview[6]  =  1;

	m_modelview[8]  =  0;
	m_modelview[9]  =  1;
	m_modelview[10] =  0;


	m_modelview[3] = m_modelview[7] = m_modelview[11] = 0;
	m_modelview[15] = 1;



	View m_view( true );
	m_view.Construct( m_projection, m_modelview, W, H );


	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &m_projection ) );

	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadMatrixf( reinterpret_cast<const float*>( &m_modelview ) );


	if( !g_ModelBrowser.m_visibleModelPaths.empty() ){
		{	// prepare for 2d stuff
			gl().glDisable( GL_BLEND );

			gl().glClientActiveTexture( GL_TEXTURE0 );
			gl().glActiveTexture( GL_TEXTURE0 );

			gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			gl().glDisableClientState( GL_NORMAL_ARRAY );
			gl().glDisableClientState( GL_COLOR_ARRAY );

			gl().glDisable( GL_TEXTURE_2D );
			gl().glDisable( GL_LIGHTING );
			gl().glDisable( GL_COLOR_MATERIAL );
			gl().glDisable( GL_DEPTH_TEST );
		}

		{	// brighter background squares
			gl().glColor4f( g_ModelBrowser.m_background_color[0] + .05f,
			                g_ModelBrowser.m_background_color[1] + .05f,
			                g_ModelBrowser.m_background_color[2] + .05f, 1.f );
			gl().glDepthMask( GL_FALSE );
			gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
			gl().glDisable( GL_CULL_FACE );

			CellPos cellPos = g_ModelBrowser.constructCellPos();
			gl().glBegin( GL_QUADS );
			for( std::size_t i = g_ModelBrowser.m_visibleModelPaths.size(); i != 0; --i ){
				const Vector3 origin = cellPos.getOrigin();
				const float minx = origin.x() - cellPos.getCellSize();
				const float maxx = origin.x() + cellPos.getCellSize();
				const float minz = origin.z() - cellPos.getCellSize();
				const float maxz = origin.z() + cellPos.getCellSize();
				gl().glVertex3f( minx, 0, maxz );
				gl().glVertex3f( minx, 0, minz );
				gl().glVertex3f( maxx, 0, minz );
				gl().glVertex3f( maxx, 0, maxz );
				++cellPos;
			}
			gl().glEnd();
		}

		// one directional light source directly behind the viewer
		{
			GLfloat inverse_cam_dir[4], ambient[4], diffuse[4];

			ambient[0] = ambient[1] = ambient[2] = 0.4f;
			ambient[3] = 1;
			diffuse[0] = diffuse[1] = diffuse[2] = 0.4f;
			diffuse[3] = 1;

			inverse_cam_dir[0] = -m_view.getViewDir()[0];
			inverse_cam_dir[1] = -m_view.getViewDir()[1];
			inverse_cam_dir[2] = -m_view.getViewDir()[2];
			inverse_cam_dir[3] = 0;

			gl().glLightfv( GL_LIGHT0, GL_POSITION, inverse_cam_dir );

			gl().glLightfv( GL_LIGHT0, GL_AMBIENT, ambient );
			gl().glLightfv( GL_LIGHT0, GL_DIFFUSE, diffuse );

			gl().glEnable( GL_LIGHT0 );
		}

		{
			ModelRenderer renderer( globalstate );

			g_ModelBrowser.forEachModelInstance( [&renderer, &m_view]( scene::Instance* instance ){
				if( Renderable *renderable = Instance_getRenderable( *instance ) )
					renderable->renderSolid( renderer, m_view );
			} );

			renderer.render( m_modelview, m_projection );
		}

		{	// prepare for 2d stuff
			gl().glColor4f( 1, 1, 1, 1 );
			gl().glDisable( GL_BLEND );

			gl().glClientActiveTexture( GL_TEXTURE0 );
			gl().glActiveTexture( GL_TEXTURE0 );

			gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			gl().glDisableClientState( GL_NORMAL_ARRAY );
			gl().glDisableClientState( GL_COLOR_ARRAY );

			gl().glDisable( GL_TEXTURE_2D );
			gl().glDisable( GL_LIGHTING );
			gl().glDisable( GL_COLOR_MATERIAL );
			gl().glDisable( GL_DEPTH_TEST );
			gl().glLineWidth( 1 );
		}
		{	// hover outline
			const int hoverId = g_ModelBrowser.hoverModelId();
			if ( hoverId >= 0 ) {
				const CellPos cellPos = g_ModelBrowser.constructCellPos();
				const Vector3 origin = cellPos.getOrigin( hoverId );
				const float cellSize = cellPos.getCellSize() * g_ModelBrowser.hoverScale();
				const float minx = origin.x() - cellSize;
				const float maxx = origin.x() + cellSize;
				const float minz = origin.z() - cellSize;
				const float maxz = origin.z() + cellSize;
				gl().glLineWidth( 2 );
				gl().glColor4f( 1.f, 0.9f, 0.2f, 1.f );
				gl().glBegin( GL_LINE_LOOP );
				gl().glVertex3f( minx, 0, maxz );
				gl().glVertex3f( minx, 0, minz );
				gl().glVertex3f( maxx, 0, minz );
				gl().glVertex3f( maxx, 0, maxz );
				gl().glEnd();
				gl().glLineWidth( 1 );
			}
		}
		{	// render model file names
			if ( OpenGLFont_canDrawSafe() ) {
				const int fontHeight = AssetBrowser_fontPixelHeight();
				const int fontDescent = AssetBrowser_fontPixelDescent();
				CellPos cellPos = g_ModelBrowser.constructCellPos();
				int index = 0;
				gl().glEnable( GL_BLEND );
				gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				for( const CopiedString& path : g_ModelBrowser.m_visibleModelPaths ){
					(void)path;
					const Vector3 pos = cellPos.getTextPos();
					if( m_view.TestPoint( pos ) ){
						AssetBrowser_drawTileLabelBackground( pos, static_cast<float>( cellPos.getCellSize() * 2 ), fontHeight, fontDescent );
						AssetBrowser_drawTileLabelText( g_ModelBrowser.modelLabelForIndex( index ), pos );
					}
					++cellPos;
					++index;
				}
				gl().glDisable( GL_BLEND );
			}
		}
	}

	// bind back to the default texture so that we don't have problems
	// elsewhere using/modifying texture maps between contexts
	gl().glBindTexture( GL_TEXTURE_2D, 0 );
}


class ModelBrowserGLWidget : public QOpenGLWidget
{
	ModelBrowser& m_modBro;
	FBO *m_fbo{};
	qreal m_scale = 1.0;
	MousePresses m_mouse;
	QPoint m_dragStart;
	bool m_skipRelease = false;
public:
	ModelBrowserGLWidget( ModelBrowser& modelBrowser ) : QOpenGLWidget(), m_modBro( modelBrowser )
	{
		setMouseTracking( true );
	}

	~ModelBrowserGLWidget() override {
		delete m_fbo;
		glwidget_context_destroyed();
	}
protected:
	void initializeGL() override
	{
		glwidget_context_created( *this );
	}
	void resizeGL( int w, int h ) override
	{
		m_scale = devicePixelRatioF();
		m_modBro.m_width = float_to_integer( w * m_scale );
		m_modBro.m_height = float_to_integer( h * m_scale );
		m_modBro.m_originInvalid = true;
		m_modBro.forEachModelInstance( models_set_transforms( ModelBrowser_baseScale() ) );

		delete m_fbo;
		m_fbo = nullptr;
		if ( m_modBro.m_width <= 0 || m_modBro.m_height <= 0 ) {
			return;
		}
		m_fbo = new FBO( m_modBro.m_width, m_modBro.m_height, true, m_modBro.m_MSAA );
	}
	void paintGL() override
	{
		if ( m_fbo == nullptr ) {
			return;
		}
		if( ScreenUpdates_Enabled() && m_fbo->bind() ){
			GlobalOpenGL_debugAssertNoErrors();
			ModelBrowser_render();
			GlobalOpenGL_debugAssertNoErrors();
			m_fbo->blit();
			m_fbo->release();
		}
	}

	void mousePressEvent( QMouseEvent *event ) override {
		setFocus();
		const auto press = m_mouse.press( event );
		if( press == MousePresses::Left2x ){
			mouseDoubleClick();
		}
		else if ( press == MousePresses::Left || press == MousePresses::Right ) {
			if ( press == MousePresses::Right || ( press == MousePresses::Left && event->modifiers().testFlag( Qt::KeyboardModifier::AltModifier ) ) ) {
				m_modBro.tracking_MouseDown();
			} else if ( press == MousePresses::Left ) {
				m_modBro.m_move_amount = 0;
			}
			if ( press == MousePresses::Left ) {
				const QPoint localPos = mouseEventLocalPos( event );
				m_dragStart = localPos;
				m_modBro.testSelect( localPos.x() * m_scale, localPos.y() * m_scale );
			}
		}
	}
	void mouseMoveEvent( QMouseEvent *event ) override {
		if ( event->buttons() == Qt::MouseButton::NoButton ) {
			const QPoint localPos = mouseEventLocalPos( event );
			m_modBro.updateHover( localPos.x() * m_scale, localPos.y() * m_scale );
			return;
		}
		if ( !( event->buttons() & Qt::MouseButton::LeftButton ) ) {
			return;
		}
		if ( event->modifiers().testFlag( Qt::KeyboardModifier::AltModifier ) ) {
			return;
		}
		const QPoint localPos = mouseEventLocalPos( event );
		if ( ( localPos - m_dragStart ).manhattanLength() < QApplication::startDragDistance() ) {
			return;
		}
		if ( m_modBro.m_currentModelId < 0 ) {
			return;
		}

		const CopiedString modelPath = m_modBro.modelPathForIndex( m_modBro.m_currentModelId );
		if ( modelPath.empty() ) {
			return;
		}
		auto* mimeData = new QMimeData;
		mimeData->setData( kModelBrowserMimeType, QByteArray( modelPath.c_str() ) );
		mimeData->setText( modelPath.c_str() );

		m_modBro.tracking_MouseUp();
		auto* drag = new QDrag( this );
		drag->setMimeData( mimeData );
		const QPixmap pixmap = ModelBrowser_dragPixmap( m_modBro );
		if ( !pixmap.isNull() ) {
			drag->setPixmap( pixmap );
			drag->setHotSpot( pixmap.rect().center() );
		}
		m_skipRelease = true;
		drag->exec( Qt::CopyAction );
	}
	void leaveEvent( QEvent *event ) override {
		m_modBro.clearHover();
		QOpenGLWidget::leaveEvent( event );
	}
	void mouseDoubleClick(){
		/* create misc_model */
		if ( m_modBro.m_currentModelId >= 0 ) {
			UndoableCommand undo( "insertModel" );
			// todo
			// GlobalEntityClassManager() search for "misc_model"
			// otherwise search for entityClass->miscmodel_is
			// otherwise go with GlobalEntityClassManager().findOrInsert( "misc_model", false );
			EntityClass* entityClass = GlobalEntityClassManager().findOrInsert( "misc_model", false );
			NodeSmartReference node( GlobalEntityCreator().createEntity( entityClass ) );

			Node_getTraversable( GlobalSceneGraph().root() )->insert( node );

			scene::Path entitypath( makeReference( GlobalSceneGraph().root() ) );
			entitypath.push( makeReference( node.get() ) );
			scene::Instance& instance = findInstance( entitypath );

			if ( Transformable* transform = Instance_getTransformable( instance ) ) { // might be cool to consider model aabb here
				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setTranslation( vector3_snapped( Camera_getOrigin( *g_pParentWnd->GetCamWnd() ) - Camera_getViewVector( *g_pParentWnd->GetCamWnd() ) * 128.f, GetSnapGridSize() ) );
				transform->freezeTransform();
			}

			GlobalSelectionSystem().setSelectedAll( false );
			Instance_setSelected( instance, true );

			const CopiedString modelPath = m_modBro.modelPathForIndex( m_modBro.m_currentModelId );
			if ( !modelPath.empty() ) {
				Node_getEntity( node )->setKeyValue( entityClass->miscmodel_key(), modelPath.c_str() );
			}
		}
	}
	void mouseReleaseEvent( QMouseEvent *event ) override {
		const auto release = m_mouse.release( event );
		if ( m_skipRelease ) {
			m_skipRelease = false;
			return;
		}
		if ( release == MousePresses::Left || release == MousePresses::Right ) {
			m_modBro.tracking_MouseUp();
		}
		if ( release == MousePresses::Left && m_modBro.m_move_amount < 16 && m_modBro.m_currentModelId >= 0 ) { // assign model to selected entity nodes
			const CopiedString modelPath = m_modBro.modelPathForIndex( m_modBro.m_currentModelId );
			if ( modelPath.empty() ) {
				return;
			}
			class EntityVisitor : public SelectionSystem::Visitor
			{
				const char* m_filePath;
			public:
				EntityVisitor( const char* filePath ) : m_filePath( filePath ){
				}
				void visit( scene::Instance& instance ) const override {
					if( Entity* entity = Node_getEntity( instance.path().top() ) ){
						entity->setKeyValue( entity->getEntityClass().miscmodel_key(), m_filePath );
					}
				}
			} visitor( modelPath.c_str() );
			UndoableCommand undo( "entityAssignModel" );
			GlobalSelectionSystem().foreachSelected( visitor );
		}
		else if( release == MousePresses::Right && m_modBro.m_move_amount < 16 ){
			m_modBro.forEachModelInstance( models_set_transforms( ModelBrowser_baseScale() ) );
			m_modBro.queueDraw();
		}
	}
	void wheelEvent( QWheelEvent *event ) override {
		setFocus();
		if( !m_modBro.m_parent->isActiveWindow() ){
			m_modBro.m_parent->activateWindow();
			m_modBro.m_parent->raise();
		}

		m_modBro.setOriginZ( m_modBro.m_originZ + std::copysign( 64, event->angleDelta().y() ) );
	}
};



static void TreeView_onRowActivated( const QModelIndex& index ){
	StringOutputStream sstream( 64 );
	const ModelFS *modelFS = &g_ModelBrowser.m_modelFS;
	{
		std::deque<QModelIndex> iters;
		iters.push_front( index );
		while( iters.front().parent().isValid() )
			iters.push_front( iters.front().parent() );
		for( const QModelIndex& i : iters ){
			const auto dir = i.data( Qt::ItemDataRole::DisplayRole ).toByteArray();
			const auto found = modelFS->m_folders.find( ModelFS( StringRange( dir.constData(), strlen( dir.constData() ) ) ) );
			if( found != modelFS->m_folders.end() ){ // ok to not find, while loading root
				modelFS = &( *found );
				sstream << dir.constData() << '/';
			}
		}
	}

//%						globalOutputStream() << sstream << " sstream\n";
	g_ModelBrowser.m_currentFolder = modelFS;
	g_ModelBrowser.m_currentFolderPath = sstream;
	ModelBrowser_rebuildVisibleModels();

	//deactivate, so SPACE and RETURN wont be broken for 2d
	g_ModelBrowser.m_treeView->clearFocus();
}



#if 0
void modelFS_traverse( const ModelFS& modelFS ){
	static int depth = -1;
	++depth;
	for( int i = 0; i < depth; ++i ){
		globalOutputStream() << '\t';
	}
	globalOutputStream() << modelFS.m_folderName << '\n';
	for( const ModelFS& m : modelFS.m_folders )
		modelFS_traverse( m );

	--depth;
}
#endif
void ModelBrowser_constructTreeModel( const ModelFS& modelFS, QStandardItemModel* model, QStandardItem* parent ){
	auto *item = new QStandardItem( modelFS.m_folderName.c_str() );
	parent->appendRow( item );
	for( const ModelFS& m : modelFS.m_folders )
		ModelBrowser_constructTreeModel( m, model, item ); //recursion
}

typedef std::map<CopiedString, std::size_t> ModelFoldersMap;

class ModelFolders
{
public:
	ModelFoldersMap m_modelFoldersMap;
	// parse string of format *pathToLoad/depth*path2ToLoad/depth*
	// */depth* for root path
	ModelFolders( const char* pathsString ){
		const auto str = StringStream<128>( PathCleaned( pathsString ) );

		const char* start = str.c_str();
		while( true ){
			while( *start == '*' )
				++start;
			const char* end = start;
			while( *end && *end != '*' )
				++end;
			if( start == end )
				break;
			const char* slash = nullptr;
			for( const char* s = start; s != end; ++s )
				if( *s == '/' )
					slash = s;
			if( slash != nullptr && end - slash > 1 ){
				std::size_t depth;
				Size_importString( depth, CopiedString( StringRange( slash + 1, end ) ).c_str() );
				StringRange folder( start, ( start == slash )? slash : slash + 1 );
				m_modelFoldersMap.emplace( folder, depth );
			}
			start = end;
		}

		ModelFoldersMap mapObjectsOnly;
		for ( const auto& [folder, depth] : m_modelFoldersMap ) {
			if ( string_equal_prefix_nocase( folder.c_str(), "models/mapobjects/" ) ) {
				mapObjectsOnly.emplace( folder, depth );
			}
		}
		m_modelFoldersMap = std::move( mapObjectsOnly );

		if( m_modelFoldersMap.empty() )
			m_modelFoldersMap.emplace( "models/mapobjects/", 99 );
	}
};


using StringSetNoCase = std::set<CopiedString, StringLessNoCase>;

class ModelPaths_ArchiveVisitor : public Archive::Visitor
{
	const StringSetNoCase& m_modelExtensions;
	ModelFS& m_modelFS;
public:
	const ModelFoldersMap& m_modelFoldersMap;
	ModelPaths_ArchiveVisitor( const StringSetNoCase& modelExtensions, ModelFS& modelFS, const ModelFoldersMap& modelFoldersMap )
		: m_modelExtensions( modelExtensions ),	m_modelFS( modelFS ), m_modelFoldersMap( modelFoldersMap ){
	}
	void visit( const char* name ) override {
		if( m_modelExtensions.contains( path_get_extension( name ) ) ){
			m_modelFS.insert( name );
//%			globalOutputStream() << name << " name\n";
		}
	}
};

void ModelPaths_addFromArchive( ModelPaths_ArchiveVisitor& visitor, const char *archiveName ){
//%	globalOutputStream() << "\t\t" << archiveName << " archiveName\n";
	Archive *archive = GlobalFileSystem().getArchive( archiveName, false );
	if ( archive != nullptr ) {
		for( const auto& folder : visitor.m_modelFoldersMap ){
			archive->forEachFile( Archive::VisitorFunc( visitor, Archive::eFiles, folder.second ), folder.first.c_str() );
		}
	}
}
typedef ReferenceCaller<ModelPaths_ArchiveVisitor, void(const char*), ModelPaths_addFromArchive> ModelPaths_addFromArchiveCaller;

void ModelBrowser_constructTree(){
	if ( g_ModelBrowser.m_referenceRefreshInProgress ) {
		g_modelBrowserTreeConstructed = false;
		return;
	}
	if ( !g_ModelBrowser.m_previewRefreshReady ) {
		return;
	}
	if ( g_ModelBrowser.m_previewRebuildInProgress ) {
		g_modelBrowserTreeConstructed = false;
		return;
	}
	ModelBrowser_cancelPendingFilterApply();
	if ( g_ModelBrowser.m_treeView == nullptr ) {
		return;
	}

	g_ModelBrowser.m_currentFolder = nullptr;
	g_ModelBrowser.m_currentFolderPath = "";
	g_ModelBrowser.m_modelFS.m_folders.clear();
	g_ModelBrowser.m_modelFS.m_files.clear();
	ModelGraph_clear();
	g_ModelBrowser.queueDraw();

	class : public IFileTypeList
	{
	public:
		StringSetNoCase m_modelExtensions;
		void addType( const char* moduleName, filetype_t type ) override {
			m_modelExtensions.emplace( moduleName );
		}
	} typelist;
	GlobalFiletypes().getTypeList( ModelLoader::Name, &typelist, true, false, false );

	ModelFolders modelFolders( g_ModelBrowser.m_prefFoldersToLoad.c_str() );

	ModelPaths_ArchiveVisitor visitor( typelist.m_modelExtensions, g_ModelBrowser.m_modelFS, modelFolders.m_modelFoldersMap );
	GlobalFileSystem().forEachArchive( ModelPaths_addFromArchiveCaller( visitor ), false, false );

//%	modelFS_traverse( g_ModelBrowser.m_modelFS );


	auto *model = qobject_cast<QStandardItemModel*>( g_ModelBrowser.m_treeView->model() );
	if ( model == nullptr ) {
		model = new QStandardItemModel( g_ModelBrowser.m_treeView );
		g_ModelBrowser.m_treeView->setModel( model );
	}
	else {
		model->clear();
	}

	{
		if( !g_ModelBrowser.m_modelFS.m_files.empty() ){ // models in the root: add blank item for access
			model->invisibleRootItem()->appendRow( new QStandardItem( "" ) );
		}

		for( const ModelFS& m : g_ModelBrowser.m_modelFS.m_folders )
			ModelBrowser_constructTreeModel( m, model, model->invisibleRootItem() );
	}

	if ( model->rowCount() > 0 ) {
		const QModelIndex first = model->index( 0, 0 );
		g_ModelBrowser.m_treeView->setCurrentIndex( first );
		TreeView_onRowActivated( first );
	}
	else {
		g_ModelBrowser.m_previewsDirty = false;
	}
	g_modelBrowserTreeConstructed = true;
}

class TexBro_QTreeView : public QTreeView
{
protected:
	bool event( QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ){
			event->accept();
			return true;
		}
		return QTreeView::event( event );
	}
};

void ModelBrowser_EnsureTree(){
	if ( !g_ModelBrowser.m_previewRefreshReady || g_ModelBrowser.m_referenceRefreshInProgress ) {
		return;
	}
	if ( g_ModelBrowser.m_previewRebuildInProgress ) {
		ModelBrowser_scheduleFilterApply();
		return;
	}
	if ( !g_modelBrowserTreeConstructed ) {
		ModelBrowser_constructTree();
	}
	else if ( g_ModelBrowser.m_filterApplyPending || g_ModelBrowser.m_previewsDirty ) {
		ModelBrowser_rebuildVisibleModels();
	}
}

class ModelBrowserSplitter : public QSplitter
{
protected:
	void showEvent( QShowEvent* event ) override {
		QSplitter::showEvent( event );
		QTimer::singleShot( 0, this, [this](){
			if ( isVisible() ) {
				ModelBrowser_EnsureTree();
			}
		} );
	}
};

QWidget* ModelBrowser_constructWindow( QWidget* toplevel ){
	g_ModelBrowser.m_parent = toplevel;
	g_modelBrowserTreeConstructed = false;

	const bool disableOpenGL = OpenGLWidgetsDisabled();

	auto *splitter = new ModelBrowserSplitter;
	auto *containerWidgetLeft = new QWidget; // Adding a QLayout to a QSplitter is not supported, use proxy widget
	auto *containerWidgetRight = new QWidget; // Adding a QLayout to a QSplitter is not supported, use proxy widget
	splitter->addWidget( containerWidgetLeft );
	splitter->addWidget( containerWidgetRight );
	auto *vbox = new QVBoxLayout( containerWidgetLeft );
	auto *hbox = new QHBoxLayout( containerWidgetRight );

	hbox->setContentsMargins( 0, 0, 0, 0 );
	vbox->setContentsMargins( 0, 0, 0, 0 );
	hbox->setSpacing( 0 );
	vbox->setSpacing( 0 );

	{	// menu bar
		auto *toolbar = new QToolBar;
		vbox->addWidget( toolbar );
		const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
		toolbar->setIconSize( QSize( iconSize, iconSize ) );

		auto* menu_view = new QMenu( toolbar );
		menu_view->addAction( "Reset Preview Scroll", [](){
			g_ModelBrowser.setOriginZ( 0 );
			g_ModelBrowser.queueDraw();
		} );
		menu_view->addAction( "Expand Folders", [](){
			if ( g_ModelBrowser.m_treeView != nullptr ) {
				g_ModelBrowser.m_treeView->expandAll();
			}
		} );
		menu_view->addAction( "Collapse Folders", [](){
			if ( g_ModelBrowser.m_treeView != nullptr ) {
				g_ModelBrowser.m_treeView->collapseAll();
			}
		} );
		menu_view->setParent( toolbar, menu_view->windowFlags() );

		toolbar_append_button( toolbar, "View", "texbro_view.png", PointerCaller<QMenu, void(), +[]( QMenu* menu ){
			menu->popup( QCursor::pos() );
		}>( menu_view ) );
		toolbar_append_button( toolbar, "Find / Replace...", "texbro_find-replace.png", "FindReplaceObjects" );
		toolbar_append_button( toolbar, "Flush / Reload Objects", "refresh_models.png", makeCallbackF( +[](){
			ModelBrowser_cancelPendingFilterApply();
			RefreshReferences();
			ModelBrowser_constructTree();
		} ) );
	}
	{	// filter bar
		auto *filterBar = new QWidget;
		auto *filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		QLineEdit *entry = g_ModelBrowser.m_filterEntry = new QLineEdit;
		filterLayout->addWidget( entry, 1 );
		entry->setClearButtonEnabled( true );
		entry->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		entry->setPlaceholderText( "Filter by name" );

		auto* globalButton = g_ModelBrowser.m_globalFilterButton = new QToolButton;
		globalButton->setAutoRaise( true );
		globalButton->setFocusPolicy( Qt::NoFocus );
		globalButton->setCheckable( true );
		globalButton->setIcon( new_local_icon( "f-world.png" ) );
		globalButton->setToolTip( "Global switch: filter across all folders" );
		filterLayout->addWidget( globalButton );

		auto* usedButton = g_ModelBrowser.m_usedFilterButton = new QToolButton;
		usedButton->setAutoRaise( true );
		usedButton->setFocusPolicy( Qt::NoFocus );
		usedButton->setCheckable( true );
		usedButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		usedButton->setText( "Used (0)" );
		usedButton->setToolTip( "Show only objects used in the current level" );
		filterLayout->addWidget( usedButton );

		auto *clearButton = g_ModelBrowser.m_clearFiltersButton = new QToolButton;
		clearButton->setAutoRaise( true );
		clearButton->setFocusPolicy( Qt::NoFocus );
		clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		clearButton->setToolTip( "Clear filters" );
		filterLayout->addWidget( clearButton );

		entry->setText( g_ModelBrowser.filter() );
		globalButton->setChecked( g_ModelBrowser.m_filterGlobal );
		usedButton->setChecked( g_ModelBrowser.m_filterUsed );
		auto* filterApplyTimer = g_ModelBrowser.m_filterApplyTimer = new QTimer( filterBar );
		filterApplyTimer->setSingleShot( true );
		QObject::connect( filterApplyTimer, &QTimer::timeout, [](){
			ModelBrowser_rebuildVisibleModels();
		} );

		QObject::connect( clearButton, &QToolButton::clicked, [entry, globalButton, usedButton](){
			ModelBrowser_cancelPendingFilterApply();
			g_ModelBrowser.setFilter( "" );
			g_ModelBrowser.m_filterGlobal = false;
			g_ModelBrowser.m_filterUsed = false;
			{
				const QSignalBlocker entryBlocker( entry );
				const QSignalBlocker globalBlocker( globalButton );
				const QSignalBlocker usedBlocker( usedButton );
				entry->clear();
				globalButton->setChecked( false );
				usedButton->setChecked( false );
			}
			ModelBrowser_updateClearFiltersButton();
			ModelBrowser_rebuildVisibleModels();
		} );
		QObject::connect( entry, &QLineEdit::textChanged, []( const QString& text ){
			g_ModelBrowser.setFilter( text.toLatin1().constData() );
			ModelBrowser_updateClearFiltersButton();
			ModelBrowser_scheduleFilterApply();
		} );
		QObject::connect( globalButton, &QToolButton::toggled, []( bool checked ){
			g_ModelBrowser.m_filterGlobal = checked;
			ModelBrowser_rebuildVisibleModels();
		} );
		QObject::connect( usedButton, &QToolButton::toggled, []( bool checked ){
			g_ModelBrowser.m_filterUsed = checked;
			ModelBrowser_rebuildVisibleModels();
		} );

		ModelBrowser_updateClearFiltersButton();
		if ( g_ModelBrowser.m_filterUsed ) {
			ModelBrowser_updateUsedFilterButtonLabel( ModelBrowser_collectUsedModels().size() );
		}
		vbox->addWidget( filterBar );
	}
	{	// TreeView
		g_ModelBrowser.m_treeView = new TexBro_QTreeView;
		g_ModelBrowser.m_treeView->setHeaderHidden( true );
		g_ModelBrowser.m_treeView->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
		g_ModelBrowser.m_treeView->setUniformRowHeights( true ); // optimization
		g_ModelBrowser.m_treeView->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		g_ModelBrowser.m_treeView->setExpandsOnDoubleClick( false );
		g_ModelBrowser.m_treeView->header()->setStretchLastSection( false ); // non greedy column sizing; + QHeaderView::ResizeMode::ResizeToContents = no text elision 🤷‍♀️
		g_ModelBrowser.m_treeView->header()->setSectionResizeMode( QHeaderView::ResizeMode::ResizeToContents );


		QObject::connect( g_ModelBrowser.m_treeView, &QAbstractItemView::activated, TreeView_onRowActivated );

		vbox->addWidget( g_ModelBrowser.m_treeView );
	}
	{	// gl_widget
		if ( disableOpenGL ) {
			g_ModelBrowser.m_gl_widget = nullptr;
			hbox->addWidget( glwidget_createDisabledPlaceholder( "OpenGL disabled (Objects)", nullptr ) );
		}
		else {
			g_ModelBrowser.m_gl_widget = new ModelBrowserGLWidget( g_ModelBrowser );
			hbox->addWidget( g_ModelBrowser.m_gl_widget );
		}
	}
	{	// gl_widget scrollbar
		if ( !disableOpenGL ) {
			auto *scroll = g_ModelBrowser.m_gl_scroll = new QScrollBar;
			hbox->addWidget( scroll );

			QObject::connect( scroll, &QAbstractSlider::valueChanged, []( int value ){
				g_ModelBrowser.m_scrollAdjustment.value_changed( value );
			} );
		}
		else {
			g_ModelBrowser.m_gl_scroll = nullptr;
		}
	}

	splitter->setStretchFactor( 0, 0 ); // consistent treeview side sizing on resizes
	splitter->setStretchFactor( 1, 1 );
	g_guiSettings.addSplitter( splitter, "ModelBrowser/splitter", { 100, 500 } );
	return splitter;
}

void ModelBrowser_destroyWindow(){
	ModelBrowser_cancelPendingFilterApply();
	g_ModelBrowser.m_parent = nullptr;
	g_ModelBrowser.m_gl_widget = nullptr;
	g_ModelBrowser.m_gl_scroll = nullptr;
	g_ModelBrowser.m_treeView = nullptr;
	g_ModelBrowser.m_filterEntry = nullptr;
	g_ModelBrowser.m_globalFilterButton = nullptr;
	g_ModelBrowser.m_usedFilterButton = nullptr;
	g_ModelBrowser.m_clearFiltersButton = nullptr;
	g_ModelBrowser.m_filterApplyTimer = nullptr;
	g_ModelBrowser.m_currentFolder = nullptr;
	g_ModelBrowser.m_currentFolderPath = "";
	g_ModelBrowser.m_previewsDirty = false;
	g_ModelBrowser.m_previewRefreshReady = true;
	g_ModelBrowser.m_previewRebuildInProgress = false;
	g_ModelBrowser.m_referenceRefreshInProgress = false;
	g_modelBrowserTreeConstructed = false;
}


const Vector3& ModelBrowser_getBackgroundColour(){
	return g_ModelBrowser.m_background_color;
}

void ModelBrowser_setBackgroundColour( const Vector3& colour ){
	g_ModelBrowser.m_background_color = colour;
	g_ModelBrowser.queueDraw();
}


#include "preferencesystem.h"
#include "preferences.h"
#include "stringio.h"

void CellSizeImport( int& oldvalue, int value ){
	if( oldvalue != value ){
		oldvalue = value;
		g_ModelBrowser.forEachModelInstance( models_set_transforms( ModelBrowser_baseScale() ) );
		g_ModelBrowser.m_originInvalid = true;
		g_ModelBrowser.queueDraw();
	}
}
typedef ReferenceCaller<int, void(int), CellSizeImport> CellSizeImportCaller;

void FoldersToLoadImport( CopiedString& self, const char* value ){
	if( self != value ){
		self = value;
		ModelBrowser_constructTree();
	}
}
typedef ReferenceCaller<CopiedString, void(const char*), FoldersToLoadImport> FoldersToLoadImportCaller;

void ModelBrowser_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Model Browser", "Model Browser Preferences" ) );

	page.appendSpinner( "Model View Size", 16, 8192,
	                    IntImportCallback( CellSizeImportCaller( g_ModelBrowser.m_cellSize ) ),
	                    IntExportCallback( IntExportCaller( g_ModelBrowser.m_cellSize ) ) );
	page.appendEntry( "Default Model Angles (x y z)",
	                  Vector3ImportStringCaller( AssetBrowser_defaultAngles() ),
	                  Vector3ExportStringCaller( AssetBrowser_defaultAngles() ) );
	page.appendEntry( "List of *folderToLoad/depth*",
	                  StringImportCallback( FoldersToLoadImportCaller( g_ModelBrowser.m_prefFoldersToLoad ) ),
	                  StringExportCallback( StringExportCaller( g_ModelBrowser.m_prefFoldersToLoad ) ) );
}
void ModelBrowser_registerPreferencesPage(){
	PreferencesDialog_addSettingsPage( makeCallbackF( ModelBrowser_constructPage ) );
}

void ModelBrowser_Construct(){
	GlobalPreferenceSystem().registerPreference( "ModelBrowserFolders", CopiedStringImportStringCaller( g_ModelBrowser.m_prefFoldersToLoad ), CopiedStringExportStringCaller( g_ModelBrowser.m_prefFoldersToLoad ) );
	GlobalPreferenceSystem().registerPreference( "ModelBrowserCellSize", IntImportStringCaller( g_ModelBrowser.m_cellSize ), IntExportStringCaller( g_ModelBrowser.m_cellSize ) );
	GlobalPreferenceSystem().registerPreference( "ColorModBroBackground", Vector3ImportStringCaller( g_ModelBrowser.m_background_color ), Vector3ExportStringCaller( g_ModelBrowser.m_background_color ) );
	GlobalPreferenceSystem().registerPreference( "AssetBrowserDefaultAngles", Vector3ImportStringCaller( AssetBrowser_defaultAngles() ), Vector3ExportStringCaller( AssetBrowser_defaultAngles() ) );
	GlobalPreferenceSystem().registerPreference( "ModelBrowserFilter",
	                                             CopiedStringImportStringCaller( g_ModelBrowser.m_filter ),
	                                             CopiedStringExportStringCaller( g_ModelBrowser.m_filter ) );
	GlobalPreferenceSystem().registerPreference( "ModelBrowserFilterGlobal",
	                                             BoolImportStringCaller( g_ModelBrowser.m_filterGlobal ),
	                                             BoolExportStringCaller( g_ModelBrowser.m_filterGlobal ) );
	GlobalPreferenceSystem().registerPreference( "ModelBrowserFilterUsed",
	                                             BoolImportStringCaller( g_ModelBrowser.m_filterUsed ),
	                                             BoolExportStringCaller( g_ModelBrowser.m_filterUsed ) );

	ModelBrowser_registerPreferencesPage();

	g_modelGraph = new ModelGraph( g_ModelBrowser );
	g_modelGraph->insert_root( ( new ModelGraphRoot )->node() );
}

void ModelBrowser_Destroy(){
	ModelBrowser_cancelPendingFilterApply();
	g_modelGraph->erase_root();
	delete g_modelGraph;
	g_modelGraph = nullptr;
}

void ModelBrowser_flushReferences(){
	ModelBrowser_cancelPendingFilterApply();
	g_ModelBrowser.m_previewRefreshReady = false;
	ModelGraph_clear();
	g_ModelBrowser.m_previewsDirty = true;
	g_ModelBrowser.queueDraw();
}

static void ModelBrowser_scheduleEnsureTreeIfVisible(){
	if ( !g_ModelBrowser.m_previewRefreshReady
	  || g_ModelBrowser.m_referenceRefreshInProgress
	  || ( g_modelBrowserTreeConstructed
	    && !g_ModelBrowser.m_filterApplyPending
	    && !g_ModelBrowser.m_previewsDirty ) ) {
		return;
	}

	if ( QTreeView* treeView = g_ModelBrowser.m_treeView; treeView != nullptr && treeView->isVisible() ) {
		QTimer::singleShot( 0, treeView, [treeView](){
			if ( treeView->isVisible()
			  && g_ModelBrowser.m_previewRefreshReady
			  && !g_ModelBrowser.m_referenceRefreshInProgress
			  && ( !g_modelBrowserTreeConstructed
			    || g_ModelBrowser.m_filterApplyPending
			    || g_ModelBrowser.m_previewsDirty ) ) {
				ModelBrowser_EnsureTree();
			}
		} );
	}
}

void ModelBrowser_mapReady(){
	g_ModelBrowser.m_previewRefreshReady = true;
	ModelBrowser_scheduleEnsureTreeIfVisible();
}

void ModelBrowser_beginReferenceRefresh(){
	ModelBrowser_pausePendingFilterApply();
	g_ModelBrowser.m_referenceRefreshInProgress = true;
}

bool ModelBrowser_canRefreshReferences(){
	return !g_ModelBrowser.m_previewRebuildInProgress;
}

void ModelBrowser_endReferenceRefresh(){
	g_ModelBrowser.m_referenceRefreshInProgress = false;
	if ( g_ModelBrowser.m_previewsDirty && g_modelGraph != nullptr ) {
		ModelGraph_clear();
		g_ModelBrowser.m_currentModelId = -1;
		g_ModelBrowser.clearHover();
		g_ModelBrowser.queueDraw();
	}
	ModelBrowser_scheduleEnsureTreeIfVisible();
}
