#include "entitybrowser.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include "assetdrop.h"
#include "eclasslib.h"
#include "entitylib.h"
#include "ieclass.h"
#include "ientity.h"
#include "igl.h"
#include "irender.h"
#include "iscenegraph.h"
#include "instancelib.h"
#include "mainframe.h"
#include "commands.h"
#include "renderable.h"
#include "renderer.h"
#include "scenelib.h"
#include "selectionlib.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "traverselib.h"
#include "view.h"
#include "filterbar.h"
#include "math/aabb.h"
#include "timer.h"
#include "assetbrowserprefs.h"
#include "gtkmisc.h"

#include "generic/callback.h"

#include "gtkutil/cursor.h"
#include "gtkutil/fbo.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/guisettings.h"
#include "gtkutil/image.h"
#include "gtkutil/mousepresses.h"
#include "gtkutil/toolbar.h"
#include "gtkutil/widget.h"

namespace {
constexpr int kEntityFilterApplyDelayMilliseconds = 150;

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

	constexpr float kAssetBrowserHoverScale = 1.05f;
	constexpr float kAssetBrowserHoverLerp = 0.2f;
	constexpr float kAssetBrowserHoverEpsilon = 0.001f;
	constexpr float kAssetBrowserHoverRotateDegrees = 12.0f;
	constexpr float kAssetBrowserHoverSpinDegreesPerSecond = 90.0f;

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
class EntityGraph final : public scene::Graph, public scene::Instantiable::Observer
{
	typedef std::map<PathConstReference, scene::Instance*> InstanceMap;

	InstanceMap m_instances;
	scene::Path m_rootpath;

	scene::Instantiable::Observer& m_observer;

public:
	EntityGraph( scene::Instantiable::Observer& observer ) : m_observer( observer ){
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
		ASSERT_MESSAGE( m_rootpath.empty(), "scenegraph root already exists" );

		root.IncRef();

		Node_traverseSubgraph( root, InstanceSubgraphWalker( this, scene::Path(), 0 ) );

		m_rootpath.push( makeReference( root ) );
	}
	void erase_root() override {
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
class TraversableEntityNodeSet : public scene::Traversable
{
	UnsortedNodeSet m_children;
	Observer* m_observer;

	void copy( const TraversableEntityNodeSet& other ){
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
	TraversableEntityNodeSet()
		: m_observer( 0 ){
	}
	TraversableEntityNodeSet( const TraversableEntityNodeSet& other )
		: scene::Traversable( other ), m_observer( 0 ){
		copy( other );
		notifyInsertAll();
	}
	~TraversableEntityNodeSet(){
		notifyEraseAll();
	}
	TraversableEntityNodeSet& operator=( const TraversableEntityNodeSet& other ){
		if ( m_observer ) {
			nodeset_diff( m_children, other.m_children, m_observer );
		}
		copy( other );
		return *this;
	}
	void swap( TraversableEntityNodeSet& other ){
		std::swap( m_children, other.m_children );
		std::swap( m_observer, other.m_observer );
	}

	void attach( Observer* observer ){
		ASSERT_MESSAGE( m_observer == 0, "TraversableEntityNodeSet::attach: observer cannot be attached" );
		m_observer = observer;
		notifyInsertAll();
	}
	void detach( Observer* observer ){
		ASSERT_MESSAGE( m_observer == observer, "TraversableEntityNodeSet::detach: observer cannot be detached" );
		notifyEraseAll();
		m_observer = 0;
	}
	void insert( scene::Node& node ) override {
		ASSERT_MESSAGE( reinterpret_cast<intptr_t>( &node ) != 0, "TraversableEntityNodeSet::insert: sanity check failed" );

		ASSERT_MESSAGE( m_children.find( NodeSmartReference( node ) ) == m_children.end(), "TraversableEntityNodeSet::insert - element already exists" );

		m_children.push_back( NodeSmartReference( node ) );

		if ( m_observer ) {
			m_observer->insert( node );
		}
	}
	void erase( scene::Node& node ) override {
		ASSERT_MESSAGE( reinterpret_cast<intptr_t>( &node ) != 0, "TraversableEntityNodeSet::erase: sanity check failed" );

		ASSERT_MESSAGE( m_children.find( NodeSmartReference( node ) ) != m_children.end(), "TraversableEntityNodeSet::erase - failed to find element" );

		if ( m_observer ) {
			m_observer->erase( node );
		}

		m_children.erase( NodeSmartReference( node ) );
	}
	void traverse( const Walker& walker ) override {
		UnsortedNodeSet::iterator i = m_children.begin();
		while ( i != m_children.end() )
		{
			Node_traverseSubgraph( *i++, walker );
		}
	}
	bool empty() const override {
		return m_children.empty();
	}
};

class BrowserCube final : public Bounded
{
	Shader* m_state = nullptr;
	bool m_ownsState = false;
	CopiedString m_shaderName;
	AABB m_aabb_local;
	Vector3 m_arrowOrigin;
	Vector3 m_arrowAngles;
	RenderableArrow m_arrow;
	bool m_drawArrow = false;
	RenderableSolidAABB m_aabb_solid;
	RenderableWireframeAABB m_aabb_wire;
public:
	BrowserCube( const AABB& aabb, Shader* state, bool drawArrow = false ) :
		m_state( state ),
		m_aabb_local( aabb ),
		m_arrowOrigin( aabb.origin ),
		m_arrowAngles( 0, 0, 0 ),
		m_arrow( m_arrowOrigin, m_arrowAngles ),
		m_drawArrow( drawArrow ),
		m_aabb_solid( m_aabb_local ),
		m_aabb_wire( m_aabb_local ){
	}
	BrowserCube( const AABB& aabb, const char* shaderName, bool drawArrow = false ) :
		m_state( GlobalShaderCache().capture( shaderName ) ),
		m_ownsState( true ),
		m_shaderName( shaderName ),
		m_aabb_local( aabb ),
		m_arrowOrigin( aabb.origin ),
		m_arrowAngles( 0, 0, 0 ),
		m_arrow( m_arrowOrigin, m_arrowAngles ),
		m_drawArrow( drawArrow ),
		m_aabb_solid( m_aabb_local ),
		m_aabb_wire( m_aabb_local ){
	}
	~BrowserCube(){
		if ( m_ownsState ) {
			GlobalShaderCache().release( m_shaderName.c_str() );
		}
	}
	const AABB& localAABB() const override {
		return m_aabb_local;
	}
	void renderSolid( Renderer& renderer, const VolumeTest& volume, const Matrix4& localToWorld ) const {
		renderer.SetState( m_state, Renderer::eFullMaterials );
		renderer.addRenderable( m_aabb_solid, localToWorld );
		if ( m_drawArrow ) {
			renderer.addRenderable( m_arrow, localToWorld );
		}
	}
	void renderWireframe( Renderer& renderer, const VolumeTest& volume, const Matrix4& localToWorld ) const {
		renderer.addRenderable( m_aabb_wire, localToWorld );
		if ( m_drawArrow ) {
			renderer.addRenderable( m_arrow, localToWorld );
		}
	}
};

class BrowserCubeInstance final : public scene::Instance, public Renderable
{
	class TypeCasts
	{
		InstanceTypeCastTable m_casts;
	public:
		TypeCasts(){
			InstanceContainedCast<BrowserCubeInstance, Bounded>::install( m_casts );
			InstanceStaticCast<BrowserCubeInstance, Renderable>::install( m_casts );
		}
		InstanceTypeCastTable& get(){
			return m_casts;
		}
	};

	BrowserCube& m_cube;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	Bounded& get( NullType<Bounded> ){
		return m_cube;
	}

	BrowserCubeInstance( const scene::Path& path, scene::Instance* parent, BrowserCube& cube ) :
		Instance( path, parent, this, StaticTypeCasts::instance().get() ),
		m_cube( cube ){
	}

	void renderSolid( Renderer& renderer, const VolumeTest& volume ) const override {
		m_cube.renderSolid( renderer, volume, Instance::localToWorld() );
	}
	void renderWireframe( Renderer& renderer, const VolumeTest& volume ) const override {
		m_cube.renderWireframe( renderer, volume, Instance::localToWorld() );
	}
};

static bool EntityBrowser_isCubeInstance( scene::Instance* instance ){
	return InstanceTypeCast<BrowserCubeInstance>::cast( *instance ) != nullptr;
}

class BrowserCubeNode final : public scene::Node::Symbiot, public scene::Instantiable
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<BrowserCubeNode, scene::Instantiable>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	InstanceSet m_instances;
	BrowserCube m_cube;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	BrowserCubeNode( const AABB& aabb, Shader* state, bool drawArrow = false ) :
		m_node( this, this, StaticTypeCasts::instance().get(), nullptr ),
		m_cube( aabb, state, drawArrow ){
	}
	BrowserCubeNode( const AABB& aabb, const char* shaderName, bool drawArrow = false ) :
		m_node( this, this, StaticTypeCasts::instance().get(), nullptr ),
		m_cube( aabb, shaderName, drawArrow ){
	}

	void release() override {
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}

	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new BrowserCubeInstance( path, parent, m_cube );
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

class EntityBrowserPreviewNode final : public scene::Node::Symbiot, public scene::Instantiable, public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<EntityBrowserPreviewNode, scene::Instantiable>::install( m_casts );
			NodeContainedCast<EntityBrowserPreviewNode, scene::Traversable>::install( m_casts );
			NodeContainedCast<EntityBrowserPreviewNode, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	MatrixTransform m_transform;
	TraversableEntityNodeSet m_traverse;
	InstanceSet m_instances;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	EntityBrowserPreviewNode( const NodeSmartReference& child ) :
		m_node( this, this, StaticTypeCasts::instance().get(), nullptr ){
		m_traverse.attach( this );
		m_traverse.insert( child );
	}

	void release() override {
		m_traverse.detach( this );
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_traverse;
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
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
};

class EntityGraphRoot final : public scene::Node::Symbiot, public scene::Instantiable, public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<EntityGraphRoot, scene::Instantiable>::install( m_casts );
			NodeContainedCast<EntityGraphRoot, scene::Traversable>::install( m_casts );
			NodeContainedCast<EntityGraphRoot, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	IdentityTransform m_transform;
	TraversableEntityNodeSet m_traverse;
	InstanceSet m_instances;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_traverse;
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
	}

	EntityGraphRoot() : m_node( this, this, StaticTypeCasts::instance().get(), nullptr ){
		m_node.m_isRoot = true;

		m_traverse.attach( this );
	}
	~EntityGraphRoot() = default;
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
		return ( new EntityGraphRoot( *this ) )->node();
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

EntityGraph* g_entityGraph = nullptr;

void EntityGraph_clear(){
	g_entityGraph->clear();
}

struct EntityCategory
{
	CopiedString name;
	std::vector<EntityClass*> classes;
};

struct CopiedStringLessNoCase
{
	bool operator()( const CopiedString& a, const CopiedString& b ) const {
		return string_less_nocase( a.c_str(), b.c_str() );
	}
};

CopiedString EntityBrowser_categoryForName( const char* classname ){
	const char* underscore = strchr( classname, '_' );
	if ( underscore == nullptr || underscore == classname ) {
		return CopiedString( "misc" );
	}
	return CopiedString( StringRange( classname, underscore ) );
}

class EntityCategoryCollector : public EntityClassVisitor
{
public:
	std::map<CopiedString, std::vector<EntityClass*>, CopiedStringLessNoCase> categories;

	void visit( EntityClass* eclass ) override {
		if ( eclass == nullptr ) {
			return;
		}
		CopiedString category = EntityBrowser_categoryForName( eclass->name() );
		categories[category].push_back( eclass );
	}
};

class CellPos
{
	const int m_cellSize;
	const int m_fontHeight;
	const int m_fontDescent;
	const int m_plusWidth;
	const int m_plusHeight;
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
	Vector3 getOrigin( int index ) const {
		const int x = ( index % m_cellsInRow ) * m_cellSize * 2 + m_cellSize + ( index % m_cellsInRow + 1 ) * m_plusWidth;
		const int z = ( index / m_cellsInRow ) * m_cellSize * 2 + m_cellSize + ( index / m_cellsInRow + 1 ) * ( m_fontHeight + m_plusHeight );
		return Vector3( x, 0, -z );
	}
	Vector3 getOrigin() const {
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
	int cellsInRow() const {
		return m_cellsInRow;
	}
	int rowHeight() const {
		return m_cellSize * 2 + m_fontHeight + m_plusHeight;
	}
	int totalHeight( int height, int cellCount ) const {
		return std::max( height, ( ( cellCount - 1 ) / m_cellsInRow + 1 ) * ( m_cellSize * 2 + m_fontHeight + m_plusHeight ) + m_fontHeight );
	}
	int testSelect( int x, int z ) const {
		if( x < 0 || z < 0 ) {
			return -1;
		}
		const int col = x / ( m_cellSize * 2 + m_plusWidth );
		const int row = z / ( m_cellSize * 2 + m_fontHeight + m_plusHeight );
		return row * m_cellsInRow + col;
	}
};

class EntityBrowser final : public scene::Instantiable::Observer
{
	std::vector<scene::Instance*> m_entityInstances;
	std::vector<EntityClass*> m_visibleClasses;
	std::vector<EntityCategory> m_categories;
	const EntityCategory* m_currentCategory = nullptr;
	CopiedString m_filter;

public:
	EntityBrowser() : m_scrollAdjustment( [this]( int value ){
		setOriginZ( -value );
	} ){
	}
	~EntityBrowser() = default;

	const int m_MSAA = 8;
	Vector3 m_background_color = Vector3( .25f );

	QWidget* m_parent = nullptr;
	QStackedWidget* m_viewStack = nullptr;
	QOpenGLWidget* m_gl_widget = nullptr;
	QScrollBar* m_gl_scroll = nullptr;
	QTreeWidget* m_listWidget = nullptr;
	QTreeView* m_treeView = nullptr;
	QLineEdit* m_filterEntry = nullptr;
	QToolButton* m_globalFilterButton = nullptr;
	QToolButton* m_usedFilterButton = nullptr;
	QToolButton* m_clearFiltersButton = nullptr;
	QToolButton* m_listModeButton = nullptr;
	QTimer* m_filterApplyTimer = nullptr;
	bool m_filterApplyPending = false;
	bool m_filterGlobal = false;
	bool m_filterUsed = false;
	bool m_listMode = false;
	bool m_referenceRefreshInProgress = false;
	bool m_referencesDirty = false;
	bool m_previewLoadInProgress = false;
	bool m_treeReloadPending = false;
	bool m_treeReloadQueued = false;
	std::size_t m_previewGraphRevision = 0;
	int m_loadedPreviewCount = 0;

	int m_width = 0;
	int m_height = 0;

	int m_originZ = 0;
	DeferredAdjustment m_scrollAdjustment;

	int m_cellSize = 80;
	int m_currentEntityId = -1;
	int m_hoverEntityId = -1;
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
		m_currentEntityId = constructCellPos().testSelect( x, z - m_originZ );
		if( m_currentEntityId >= static_cast<int>( m_visibleClasses.size() ) )
			m_currentEntityId = -1;
	}
	void updateHover( int x, int z ){
		setHoverId( constructCellPos().testSelect( x, z - m_originZ ) );
	}
	void clearHover(){
		setHoverId( -1 );
	}
	int hoverEntityId() const {
		return m_hoverEntityId;
	}
	float hoverScale() const {
		return m_hoverScale;
	}
	float hoverScaleForIndex( int index ) const {
		return index == m_hoverEntityId ? m_hoverScale : 1.0f;
	}
	float hoverRotationForIndex( int index ) const {
		return index == m_hoverEntityId ? ( m_hoverRotate + m_hoverSpin ) : 0.0f;
	}
	float hoverAlpha() const {
		if ( m_hoverEntityId < 0 ) {
			return 0.0f;
		}
		return std::clamp( ( m_hoverScale - 1.0f ) / ( kAssetBrowserHoverScale - 1.0f ), 0.0f, 1.0f );
	}
	bool updateHoverAnimation(){
		const float previousSpin = m_hoverSpin;
		if ( m_hoverEntityId < 0 ) {
			const bool rotateChanged = std::fabs( m_hoverRotate ) > kAssetBrowserHoverEpsilon;
			const bool spinChanged = std::fabs( m_hoverSpin ) > kAssetBrowserHoverEpsilon;
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			m_hoverRotate = 0.0f;
			m_hoverRotateTarget = 0.0f;
			m_hoverSpin = 0.0f;
			return rotateChanged || spinChanged;
		}
		if ( m_hoverEntityId >= static_cast<int>( m_visibleClasses.size() ) ) {
			const bool rotateChanged = std::fabs( m_hoverRotate ) > kAssetBrowserHoverEpsilon;
			const bool spinChanged = std::fabs( m_hoverSpin ) > kAssetBrowserHoverEpsilon;
			m_hoverEntityId = -1;
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
			m_hoverEntityId = -1;
			m_hoverRotate = 0.0f;
			m_hoverRotateTarget = 0.0f;
			m_hoverSpin = 0.0f;
		}
		return ( std::fabs( m_hoverScale - previous ) > kAssetBrowserHoverEpsilon )
			|| ( std::fabs( m_hoverRotate - previousRotate ) > kAssetBrowserHoverEpsilon )
			|| spinChanged
			|| spinActive;
	}
	const EntityClass* currentEntityClass() const {
		if ( m_currentEntityId < 0 || m_currentEntityId >= static_cast<int>( m_visibleClasses.size() ) ) {
			return nullptr;
		}
		return m_visibleClasses[m_currentEntityId];
	}
private:
	void setHoverId( int hoverId ){
		if ( hoverId >= static_cast<int>( m_visibleClasses.size() ) ) {
			hoverId = -1;
		}
		if ( hoverId < 0 ) {
			if ( m_hoverEntityId >= 0 && m_hoverScaleTarget != 1.0f ) {
				m_hoverScaleTarget = 1.0f;
				m_hoverRotateTarget = 0.0f;
				m_hoverRotate = 0.0f;
				m_hoverSpin = 0.0f;
				queueDraw();
			}
			return;
		}

		const bool idChanged = hoverId != m_hoverEntityId;
		if ( idChanged ) {
			m_hoverEntityId = hoverId;
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
		return constructCellPos().totalHeight( m_height, m_visibleClasses.size() );
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
		validate();
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
		if ( event->buttons() & Qt::MouseButton::RightButton && y != 0 ) {
			const int scale = event->modifiers().testFlag( Qt::KeyboardModifier::ShiftModifier )? 4 : 1;
			setOriginZ( m_originZ + y * scale );
		}
	}
	FreezePointer m_freezePointer;
	bool m_move_started = false;
public:
	int m_move_amount = 0;
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
			m_entityInstances.push_back( instance );
			m_originZ = 0;
			m_originInvalid = true;
		}
	}
	void erase( scene::Instance* instance ) override {
		if ( m_referenceRefreshInProgress ) {
			m_referencesDirty = true;
		}
		m_entityInstances.clear();
		m_currentEntityId = -1;
		m_originZ = 0;
		m_originInvalid = true;
	}
	template<typename Functor>
	void forEachEntityInstance( const Functor& functor ) const {
		for( scene::Instance* instance : m_entityInstances )
			functor( instance );
	}

	void setCategories( std::vector<EntityCategory> categories ){
		m_categories = std::move( categories );
	}
	const std::vector<EntityCategory>& categories() const {
		return m_categories;
	}
	const EntityCategory* findCategory( const char* name ) const {
		for ( const EntityCategory& category : m_categories ) {
			if ( string_equal_nocase( category.name.c_str(), name ) ) {
				return &category;
			}
		}
		return nullptr;
	}
	void setFilter( const char* filter ){
		m_filter = filter;
	}
	const char* filter() const {
		return m_filter.c_str();
	}
	CopiedString& filterStorage(){
		return m_filter;
	}
	const CopiedString& filterStorage() const {
		return m_filter;
	}
	void setCurrentCategory( const EntityCategory* category ){
		m_currentCategory = category;
	}
	const EntityCategory* currentCategory() const {
		return m_currentCategory;
	}
	std::vector<EntityClass*>& visibleClasses(){
		return m_visibleClasses;
	}
};

EntityBrowser g_EntityBrowser;
static bool g_entityBrowserTreeConstructed = false;

using EntityClassnameSet = std::set<CopiedString, StringLessNoCase>;

class EntityBrowserUsedClassCollector : public scene::Graph::Walker
{
	EntityClassnameSet& m_used;
public:
	explicit EntityBrowserUsedClassCollector( EntityClassnameSet& used ) : m_used( used ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Entity* entity = Node_getEntity( path.top() ) ) {
			m_used.emplace( entity->getClassName() );
		}
		return true;
	}
};

EntityClassnameSet EntityBrowser_collectUsedClasses(){
	EntityClassnameSet used;
	GlobalSceneGraph().traverse( EntityBrowserUsedClassCollector( used ) );
	return used;
}

void EntityBrowser_updateClearFiltersButton(){
	if ( g_EntityBrowser.m_clearFiltersButton != nullptr ) {
		const bool active = !string_empty( g_EntityBrowser.filter() )
		                 || g_EntityBrowser.m_filterGlobal
		                 || g_EntityBrowser.m_filterUsed;
		g_EntityBrowser.m_clearFiltersButton->setEnabled( active );
	}
}

void EntityBrowser_updateUsedFilterButtonLabel( std::size_t usedCount ){
	if ( g_EntityBrowser.m_usedFilterButton != nullptr ) {
		g_EntityBrowser.m_usedFilterButton->setText( StringStream<64>( "Used (", usedCount, ')' ).c_str() );
	}
}

void EntityBrowser_cancelPendingFilterApply(){
	if ( g_EntityBrowser.m_filterApplyTimer != nullptr ) {
		g_EntityBrowser.m_filterApplyTimer->stop();
	}
	g_EntityBrowser.m_filterApplyPending = false;
}

void EntityBrowser_scheduleFilterApply(){
	g_EntityBrowser.m_filterApplyPending = true;
	if ( g_EntityBrowser.m_filterApplyTimer != nullptr
	  && !g_EntityBrowser.m_referenceRefreshInProgress
	  && !g_EntityBrowser.m_previewLoadInProgress ) {
		g_EntityBrowser.m_filterApplyTimer->start( kEntityFilterApplyDelayMilliseconds );
	}
}

void EntityBrowser_pausePendingFilterApply(){
	if ( g_EntityBrowser.m_filterApplyTimer != nullptr ) {
		g_EntityBrowser.m_filterApplyTimer->stop();
	}
}

static bool EntityBrowser_isTriggerClass( const EntityClass* eclass ){
	return eclass != nullptr
	    && !eclass->fixedsize
	    && string_equal_nocase_n( eclass->name(), "trigger_", 8 );
}

static bool EntityBrowser_needsFixedSizePreview( const EntityClass* eclass ){
	return eclass != nullptr
	    && eclass->fixedsize
	    && !eclass->miscmodel_is
	    && string_empty( eclass->modelpath() );
}

static bool EntityBrowser_needsBrushPreview( const EntityClass* eclass ){
	return eclass != nullptr
	    && !eclass->fixedsize
	    && !eclass->miscmodel_is
	    && string_empty( eclass->modelpath() );
}

static bool EntityBrowser_needsMiscModelFallbackPreview( const EntityClass* eclass ){
	if ( eclass == nullptr || !eclass->miscmodel_is ) {
		return false;
	}
	if ( !string_empty( eclass->modelpath() ) ) {
		return false;
	}
	const char* modelKey = eclass->miscmodel_key();
	return string_empty( modelKey ) || string_empty( EntityClass_valueForKey( *eclass, modelKey ) );
}

static void EntityBrowser_addTriggerPreview( scene::Node& node ){
	scene::Traversable* traversable = Node_getTraversable( node );
	if ( traversable == nullptr ) {
		return;
	}

	const Vector3 extents( 16.0f, 16.0f, 16.0f );
	const AABB bounds( g_vector3_identity, extents );
	const CopiedString shader = GetCommonShader( "trigger" );
	NodeSmartReference cubeNode( ( new BrowserCubeNode( bounds, shader.c_str() ) )->node() );
	traversable->insert( cubeNode );
}

static NodeSmartReference EntityBrowser_createFixedSizePreviewNode( EntityClass* eclass ){
	const AABB bounds = aabb_for_minmax( eclass->mins, eclass->maxs );
	NodeSmartReference cubeNode( ( new BrowserCubeNode( bounds, eclass->m_state_fill, true ) )->node() );
	return NodeSmartReference( ( new EntityBrowserPreviewNode( cubeNode ) )->node() );
}

static NodeSmartReference EntityBrowser_createBrushPreviewNode(){
	const Vector3 extents( 16.0f, 16.0f, 16.0f );
	const AABB bounds( g_vector3_identity, extents );
	const CopiedString shader = GetCommonShader( "notex" );
	NodeSmartReference cubeNode( ( new BrowserCubeNode( bounds, shader.c_str() ) )->node() );
	return NodeSmartReference( ( new EntityBrowserPreviewNode( cubeNode ) )->node() );
}

static NodeSmartReference EntityBrowser_createPreviewNode( EntityClass* eclass ){
	if ( EntityBrowser_needsFixedSizePreview( eclass ) ) {
		return EntityBrowser_createFixedSizePreviewNode( eclass );
	}
	if ( EntityBrowser_needsMiscModelFallbackPreview( eclass ) ) {
		return EntityBrowser_createBrushPreviewNode();
	}
	if ( EntityBrowser_isTriggerClass( eclass ) ) {
		NodeSmartReference node( GlobalEntityCreator().createEntity( eclass ) );
		EntityBrowser_addTriggerPreview( node.get() );
		return node;
	}
	if ( EntityBrowser_needsBrushPreview( eclass ) ) {
		return EntityBrowser_createBrushPreviewNode();
	}
	return NodeSmartReference( GlobalEntityCreator().createEntity( eclass ) );
}

static QRect EntityBrowser_cellRectPixels( const EntityBrowser& browser, int index ){
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

static QPixmap EntityBrowser_dragPixmap( EntityBrowser& browser ){
	if ( browser.m_gl_widget == nullptr || browser.m_currentEntityId < 0 ) {
		return QPixmap();
	}
	QImage frame = browser.m_gl_widget->grabFramebuffer();
	QRect rect = EntityBrowser_cellRectPixels( browser, browser.m_currentEntityId ).intersected( frame.rect() );
	if ( rect.isEmpty() ) {
		return QPixmap();
	}
	QImage tile = applyOpacity( frame.copy( rect ), 0.6f );
	QPixmap pixmap = QPixmap::fromImage( tile );
	pixmap.setDevicePixelRatio( browser.m_gl_widget->devicePixelRatioF() );
	return pixmap;
}

static float EntityBrowser_maxScaledExtent(){
	float maxExtent = 0.0f;
	const Matrix4 rotation = matrix4_rotation_for_euler_xyz_degrees( AssetBrowser_defaultAngles() );
	g_EntityBrowser.forEachEntityInstance( [&]( scene::Instance* instance ){
		if ( Bounded *bounded = Instance_getBounded( *instance ) ) {
			AABB aabb = bounded->localAABB();
			if ( !aabb_valid( aabb ) ) {
				return;
			}
			const Vector3 extents = rotatedExtentsForAabb( aabb, rotation );
			const float baseExtent = std::max( extents[0], extents[2] );
			if ( baseExtent <= 0.0f ) {
				return;
			}
			const Entity* entity = Node_getEntity( instance->path().parent() );
			const float scaledExtent = baseExtent * ( entity != nullptr && EntityBrowser_isTriggerClass( &entity->getEntityClass() ) ? 2.0f : 1.0f );
			maxExtent = std::max( maxExtent, scaledExtent );
		}
	} );
	return maxExtent > 0.0f ? maxExtent : 1.0f;
}

static float EntityBrowser_baseScale(){
	const float maxExtent = EntityBrowser_maxScaledExtent();
	return maxExtent > 0.0f
	     ? static_cast<float>( g_EntityBrowser.m_cellSize ) / ( maxExtent * kAssetBrowserHoverScale )
	     : 1.0f;
}

class entities_set_transforms
{
	const float m_baseScale;
	mutable CellPos m_cellPos = g_EntityBrowser.constructCellPos();
public:
	explicit entities_set_transforms( float baseScale ) : m_baseScale( baseScale ){
	}
	void operator()( scene::Instance* instance ) const {
		if( TransformNode *transformNode = Node_getTransformNode( instance->path().parent() ) ){
			if( Bounded *bounded = Instance_getBounded( *instance ) ){
				AABB aabb = bounded->localAABB();
				if ( !aabb_valid( aabb ) ) {
					aabb = AABB( g_vector3_identity, Vector3( 1, 1, 1 ) );
				}
				const Entity* entity = Node_getEntity( instance->path().parent() );
				const bool isTrigger = entity != nullptr && EntityBrowser_isTriggerClass( &entity->getEntityClass() );
				const int index = m_cellPos.index();
				float scale = m_baseScale * g_EntityBrowser.hoverScaleForIndex( index );
				if ( isTrigger ) {
					scale *= 2.0f;
				}
				const float hoverRotate = g_EntityBrowser.hoverRotationForIndex( index );
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
				++m_cellPos;
			}
		}
	}
};

class EntityRenderer : public Renderer
{
	struct state_type
	{
		state_type() :
			m_state( 0 ){
		}
		Shader* m_state;
	};
public:
	EntityRenderer( RenderStateFlags globalstate ) :
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

static void EntityBrowser_ensureVisiblePreviews();
static void EntityBrowser_queuePendingTreeReload();

void EntityBrowser_render(){
	g_EntityBrowser.validate();
	EntityBrowser_ensureVisiblePreviews();
	const bool hoverChanged = g_EntityBrowser.updateHoverAnimation();
	if ( hoverChanged ) {
		g_EntityBrowser.forEachEntityInstance( entities_set_transforms( EntityBrowser_baseScale() ) );
	}

	const int W = g_EntityBrowser.m_width;
	const int H = g_EntityBrowser.m_height;
	gl().glViewport( 0, 0, W, H );

	gl().glDepthMask( GL_TRUE );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	gl().glClearColor( g_EntityBrowser.m_background_color[0],
	                   g_EntityBrowser.m_background_color[1],
	                   g_EntityBrowser.m_background_color[2], 0 );
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
	m_modelview[12] = -W / 2.f;
	m_modelview[13] = H / 2.f - g_EntityBrowser.m_originZ;
	m_modelview[14] = 9999;

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

	if( g_EntityBrowser.currentCategory() != nullptr ){
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
			gl().glColor4f( g_EntityBrowser.m_background_color[0] + .05f,
			                g_EntityBrowser.m_background_color[1] + .05f,
			                g_EntityBrowser.m_background_color[2] + .05f, 1.f );
			gl().glDepthMask( GL_FALSE );
			gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
			gl().glDisable( GL_CULL_FACE );

			CellPos cellPos = g_EntityBrowser.constructCellPos();
			gl().glBegin( GL_QUADS );
			for( std::size_t i = g_EntityBrowser.visibleClasses().size(); i != 0; --i ){
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

		{	// directional lighting: keep models readable, add depth to cube tiles
			auto setDirectionalLight = []( const Vector3& direction, float ambientStrength, float diffuseStrength ){
				GLfloat dir[4] = { direction[0], direction[1], direction[2], 0.0f };
				GLfloat ambient[4] = { ambientStrength, ambientStrength, ambientStrength, 1.0f };
				GLfloat diffuse[4] = { diffuseStrength, diffuseStrength, diffuseStrength, 1.0f };
				gl().glLightfv( GL_LIGHT0, GL_POSITION, dir );
				gl().glLightfv( GL_LIGHT0, GL_AMBIENT, ambient );
				gl().glLightfv( GL_LIGHT0, GL_DIFFUSE, diffuse );
				gl().glEnable( GL_LIGHT0 );
			};

			const Vector3 viewDir = m_view.getViewDir();
			setDirectionalLight( Vector3( -viewDir[0], -viewDir[1], -viewDir[2] ), 0.4f, 0.4f );
			{
				EntityRenderer renderer( globalstate );
				g_EntityBrowser.forEachEntityInstance( [&renderer, &m_view]( scene::Instance* instance ){
					if ( EntityBrowser_isCubeInstance( instance ) ) {
						return;
					}
					if( Renderable *renderable = Instance_getRenderable( *instance ) ) {
						renderable->renderSolid( renderer, m_view );
					}
				} );
				renderer.render( m_modelview, m_projection );
			}

			const Vector3 cubeLightDir = vector3_normalised( Vector3( -0.4f, 0.6f, -1.0f ) );
			setDirectionalLight( cubeLightDir, 0.2f, 0.75f );
			{
				EntityRenderer renderer( globalstate );
				g_EntityBrowser.forEachEntityInstance( [&renderer, &m_view]( scene::Instance* instance ){
					if ( !EntityBrowser_isCubeInstance( instance ) ) {
						return;
					}
					if( Renderable *renderable = Instance_getRenderable( *instance ) ) {
						renderable->renderSolid( renderer, m_view );
					}
				} );
				renderer.render( m_modelview, m_projection );
			}
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
			const int hoverId = g_EntityBrowser.hoverEntityId();
			if ( hoverId >= 0 ) {
				const CellPos cellPos = g_EntityBrowser.constructCellPos();
				const Vector3 origin = cellPos.getOrigin( hoverId );
				const float cellSize = cellPos.getCellSize() * g_EntityBrowser.hoverScale();
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
		{	// render entity class names
			if ( OpenGLFont_canDrawSafe() ) {
				const int fontHeight = AssetBrowser_fontPixelHeight();
				const int fontDescent = AssetBrowser_fontPixelDescent();
				CellPos cellPos = g_EntityBrowser.constructCellPos();
				gl().glEnable( GL_BLEND );
				gl().glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				for( const EntityClass* eclass : g_EntityBrowser.visibleClasses() ){
					const Vector3 pos = cellPos.getTextPos();
					if( m_view.TestPoint( pos ) ){
						AssetBrowser_drawTileLabelBackground( pos, static_cast<float>( cellPos.getCellSize() * 2 ), fontHeight, fontDescent );
						AssetBrowser_drawTileLabelText( eclass->name(), pos );
					}
					++cellPos;
				}
				gl().glDisable( GL_BLEND );
			}
		}
	}

	gl().glBindTexture( GL_TEXTURE_2D, 0 );
}

class EntityBrowserGLWidget : public QOpenGLWidget
{
	EntityBrowser& m_entBro;
	FBO *m_fbo{};
	qreal m_scale = 1.0;
	MousePresses m_mouse;
	QPoint m_dragStart;
public:
	EntityBrowserGLWidget( EntityBrowser& entityBrowser ) : QOpenGLWidget(), m_entBro( entityBrowser ){
		setMouseTracking( true );
	}

	~EntityBrowserGLWidget() override {
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
		m_entBro.m_width = float_to_integer( w * m_scale );
		m_entBro.m_height = float_to_integer( h * m_scale );
		m_entBro.m_originInvalid = true;
		m_entBro.forEachEntityInstance( entities_set_transforms( EntityBrowser_baseScale() ) );

		delete m_fbo;
		m_fbo = nullptr;
		if ( m_entBro.m_width <= 0 || m_entBro.m_height <= 0 ) {
			return;
		}
		m_fbo = new FBO( m_entBro.m_width, m_entBro.m_height, true, m_entBro.m_MSAA );
	}
	void paintGL() override
	{
		if ( m_fbo == nullptr ) {
			return;
		}
		if( ScreenUpdates_Enabled() && m_fbo->bind() ){
			GlobalOpenGL_debugAssertNoErrors();
			EntityBrowser_render();
			GlobalOpenGL_debugAssertNoErrors();
			m_fbo->blit();
			m_fbo->release();
		}
	}

	void mousePressEvent( QMouseEvent *event ) override {
		setFocus();
		const auto press = m_mouse.press( event );
		if ( press == MousePresses::Left || press == MousePresses::Right ) {
			if ( press == MousePresses::Right ) {
				m_entBro.tracking_MouseDown();
			} else {
				m_entBro.m_move_amount = 0;
			}
			if ( press == MousePresses::Left ) {
				const QPoint localPos = mouseEventLocalPos( event );
				m_dragStart = localPos;
				m_entBro.testSelect( localPos.x() * m_scale, localPos.y() * m_scale );
			}
		}
	}
	void mouseMoveEvent( QMouseEvent *event ) override {
		if ( event->buttons() == Qt::MouseButton::NoButton ) {
			const QPoint localPos = mouseEventLocalPos( event );
			m_entBro.updateHover( localPos.x() * m_scale, localPos.y() * m_scale );
			return;
		}
		if ( !( event->buttons() & Qt::MouseButton::LeftButton ) ) {
			return;
		}
		const QPoint localPos = mouseEventLocalPos( event );
		if ( ( localPos - m_dragStart ).manhattanLength() < QApplication::startDragDistance() ) {
			return;
		}

		const EntityClass* eclass = m_entBro.currentEntityClass();
		if ( eclass == nullptr ) {
			return;
		}

		auto* mimeData = new QMimeData;
		mimeData->setData( kEntityBrowserMimeType, QByteArray( eclass->name() ) );
		mimeData->setText( eclass->name() );

		m_entBro.tracking_MouseUp();
		auto* drag = new QDrag( this );
		drag->setMimeData( mimeData );
		const QPixmap pixmap = EntityBrowser_dragPixmap( m_entBro );
		if ( !pixmap.isNull() ) {
			drag->setPixmap( pixmap );
			drag->setHotSpot( pixmap.rect().center() );
		}
		drag->exec( Qt::CopyAction );
	}
	void leaveEvent( QEvent *event ) override {
		m_entBro.clearHover();
		QOpenGLWidget::leaveEvent( event );
	}
	void mouseReleaseEvent( QMouseEvent *event ) override {
		const auto release = m_mouse.release( event );
		if ( release == MousePresses::Left || release == MousePresses::Right ) {
			m_entBro.tracking_MouseUp();
		}
	}
	void wheelEvent( QWheelEvent *event ) override {
		setFocus();
		m_entBro.setOriginZ( m_entBro.m_originZ + std::copysign( 64, event->angleDelta().y() ) );
	}
};

static int EntityBrowser_visiblePreviewTargetCount(){
	if ( g_EntityBrowser.m_listMode ) {
		return 0;
	}
	const CellPos cellPos = g_EntityBrowser.constructCellPos();
	const int cellsInRow = cellPos.cellsInRow();
	const int rowHeight = cellPos.rowHeight();
	if ( cellsInRow <= 0 || rowHeight <= 0 ) {
		return 0;
	}
	const int topRow = std::max( 0, -g_EntityBrowser.m_originZ / rowHeight );
	const int bottomRow = std::max( topRow, ( g_EntityBrowser.m_height - g_EntityBrowser.m_originZ ) / rowHeight );
	const int bufferedBottom = bottomRow + 2;
	return std::min( static_cast<int>( g_EntityBrowser.visibleClasses().size() ),
	                 ( bufferedBottom + 1 ) * cellsInRow );
}

static void EntityBrowser_insertPreviewNodesUpTo( int targetCount ){
	if ( g_entityGraph == nullptr || g_EntityBrowser.m_listMode || g_EntityBrowser.m_previewLoadInProgress ) {
		return;
	}
	targetCount = std::max( 0, std::min( targetCount, static_cast<int>( g_EntityBrowser.visibleClasses().size() ) ) );
	if ( targetCount <= g_EntityBrowser.m_loadedPreviewCount ) {
		return;
	}

	if ( scene::Traversable* traversable = Node_getTraversable( g_entityGraph->root() ) ) {
		const std::size_t graphRevision = g_EntityBrowser.m_previewGraphRevision;
		class ScopedPreviewLoad
		{
			bool& m_inProgress;
		public:
			explicit ScopedPreviewLoad( bool& inProgress ) : m_inProgress( inProgress ) {
				m_inProgress = true;
				EntityBrowser_pausePendingFilterApply();
			}
			~ScopedPreviewLoad(){
				m_inProgress = false;
				if ( g_EntityBrowser.m_treeReloadPending ) {
					EntityBrowser_queuePendingTreeReload();
				}
				else if ( g_EntityBrowser.m_filterApplyPending ) {
					EntityBrowser_scheduleFilterApply();
				}
			}
		} previewLoad( g_EntityBrowser.m_previewLoadInProgress );

		const char* status = g_EntityBrowser.currentCategory() != nullptr ? g_EntityBrowser.currentCategory()->name.c_str() : "Entities";
		ScopeDisableScreenUpdates disableScreenUpdates( status, "Loading Entity Previews" );
		for ( int i = g_EntityBrowser.m_loadedPreviewCount; i < targetCount; ++i ) {
			if ( graphRevision != g_EntityBrowser.m_previewGraphRevision
			  || i >= static_cast<int>( g_EntityBrowser.visibleClasses().size() ) ) {
				return;
			}
			NodeSmartReference node( EntityBrowser_createPreviewNode( g_EntityBrowser.visibleClasses()[i] ) );
			if ( graphRevision != g_EntityBrowser.m_previewGraphRevision ) {
				return;
			}
			traversable->insert( node );
		}
		if ( graphRevision != g_EntityBrowser.m_previewGraphRevision ) {
			return;
		}
		g_EntityBrowser.m_loadedPreviewCount = targetCount;
		g_EntityBrowser.forEachEntityInstance( entities_set_transforms( EntityBrowser_baseScale() ) );
	}
}

static void EntityBrowser_ensureVisiblePreviews(){
	if ( g_EntityBrowser.m_referenceRefreshInProgress
	  || g_EntityBrowser.m_gl_widget == nullptr
	  || !g_EntityBrowser.m_gl_widget->isVisible() ) {
		return;
	}
	EntityBrowser_insertPreviewNodesUpTo( EntityBrowser_visiblePreviewTargetCount() );
}

static void EntityBrowser_rebuildListWidget(){
	if ( g_EntityBrowser.m_listWidget == nullptr ) {
		return;
	}
	g_EntityBrowser.m_listWidget->clear();
	int index = 0;
	for ( EntityClass* eclass : g_EntityBrowser.visibleClasses() ) {
		auto* item = new QTreeWidgetItem( g_EntityBrowser.m_listWidget );
		item->setText( 0, eclass->name() );
		item->setData( 0, Qt::ItemDataRole::UserRole, index++ );
	}
}

static void EntityBrowser_selectCategory( const QString& name ){
	if ( g_EntityBrowser.m_referenceRefreshInProgress || g_EntityBrowser.m_previewLoadInProgress ) {
		g_EntityBrowser.m_filterApplyPending = true;
		return;
	}
	EntityBrowser_cancelPendingFilterApply();
	const EntityCategory* category = g_EntityBrowser.findCategory( name.toLatin1().constData() );
	g_EntityBrowser.setCurrentCategory( category );
	EntityBrowser_updateClearFiltersButton();

	const EntityCategory* visibleCategory = category;
	if ( g_EntityBrowser.m_filterGlobal ) {
		if ( const EntityCategory* all = g_EntityBrowser.findCategory( "All" ); all != nullptr ) {
			visibleCategory = all;
		}
	}

	EntityClassnameSet usedClasses;
	if ( g_EntityBrowser.m_filterUsed ) {
		usedClasses = EntityBrowser_collectUsedClasses();
		EntityBrowser_updateUsedFilterButtonLabel( usedClasses.size() );
	}

	++g_EntityBrowser.m_previewGraphRevision;
	EntityGraph_clear();
	g_EntityBrowser.m_loadedPreviewCount = 0;
	g_EntityBrowser.visibleClasses().clear();
	g_EntityBrowser.m_currentEntityId = -1;
	g_EntityBrowser.clearHover();
	if ( visibleCategory != nullptr ) {
		for ( EntityClass* eclass : visibleCategory->classes ) {
			if ( !string_contains_nocase( eclass->name(), g_EntityBrowser.filter() ) ) {
				continue;
			}
			if ( g_EntityBrowser.m_filterUsed && !usedClasses.contains( eclass->name() ) ) {
				continue;
			}
			g_EntityBrowser.visibleClasses().push_back( eclass );
		}
	}
	EntityBrowser_rebuildListWidget();
	EntityBrowser_ensureVisiblePreviews();
	g_EntityBrowser.queueDraw();
}

static void EntityBrowser_applyCurrentFilterNow(){
	EntityBrowser_cancelPendingFilterApply();
	if ( g_EntityBrowser.m_treeView != nullptr && g_EntityBrowser.m_treeView->currentIndex().isValid() ) {
		EntityBrowser_selectCategory( g_EntityBrowser.m_treeView->currentIndex().data( Qt::ItemDataRole::DisplayRole ).toString() );
		return;
	}
	if ( const EntityCategory* category = g_EntityBrowser.currentCategory() ) {
		EntityBrowser_selectCategory( category->name.c_str() );
	}
	else {
		EntityBrowser_updateClearFiltersButton();
	}
}

static void EntityBrowser_setListMode( bool listMode ){
	if ( g_EntityBrowser.m_listMode == listMode ) {
		return;
	}
	g_EntityBrowser.m_listMode = listMode;
	if ( g_EntityBrowser.m_viewStack != nullptr ) {
		g_EntityBrowser.m_viewStack->setCurrentIndex( listMode ? 1 : 0 );
	}
	if ( g_EntityBrowser.m_listModeButton != nullptr
	  && g_EntityBrowser.m_listModeButton->isChecked() != listMode ) {
		g_EntityBrowser.m_listModeButton->setChecked( listMode );
	}
	if ( const EntityCategory* category = g_EntityBrowser.currentCategory() ) {
		EntityBrowser_selectCategory( category->name.c_str() );
	}
}

static void EntityBrowser_constructCategories(){
	EntityCategoryCollector collector;
	GlobalEntityClassManager().forEach( collector );

	std::vector<EntityCategory> categories;
	EntityCategory all;
	all.name = "All";

	auto entity_sorter = []( EntityClass* a, EntityClass* b ){
		return string_less_nocase( a->name(), b->name() );
	};

	for ( auto& pair : collector.categories ) {
		auto& classes = pair.second;
		std::sort( classes.begin(), classes.end(), entity_sorter );
		EntityCategory category;
		category.name = pair.first;
		category.classes = classes;
		categories.push_back( category );

		all.classes.insert( all.classes.end(), classes.begin(), classes.end() );
	}

	std::sort( all.classes.begin(), all.classes.end(), entity_sorter );
	categories.insert( categories.begin(), std::move( all ) );

	g_EntityBrowser.setCategories( std::move( categories ) );
}

static void EntityBrowser_constructTree(){
	EntityBrowser_cancelPendingFilterApply();
	if ( g_EntityBrowser.m_treeView == nullptr ) {
		return;
	}

	g_EntityBrowser.setCurrentCategory( nullptr );
	EntityBrowser_constructCategories();

	auto *model = qobject_cast<QStandardItemModel*>( g_EntityBrowser.m_treeView->model() );
	if ( model == nullptr ) {
		model = new QStandardItemModel( g_EntityBrowser.m_treeView );
		g_EntityBrowser.m_treeView->setModel( model );
	}
	else {
		model->clear();
	}
	for ( const EntityCategory& category : g_EntityBrowser.categories() ) {
		model->invisibleRootItem()->appendRow( new QStandardItem( category.name.c_str() ) );
	}

	if ( model->rowCount() > 0 ) {
		const QModelIndex first = model->index( 0, 0 );
		g_EntityBrowser.m_treeView->setCurrentIndex( first );
		EntityBrowser_selectCategory( first.data( Qt::ItemDataRole::DisplayRole ).toString() );
	}

	g_entityBrowserTreeConstructed = true;
}

static void EntityBrowser_reloadTree(){
	if ( g_EntityBrowser.m_referenceRefreshInProgress || g_EntityBrowser.m_previewLoadInProgress ) {
		g_EntityBrowser.m_treeReloadPending = true;
		return;
	}

	g_EntityBrowser.m_treeReloadPending = false;
	EntityBrowser_flushReferences();
	EntityBrowser_constructTree();
}

static void EntityBrowser_queuePendingTreeReload(){
	if ( !g_EntityBrowser.m_treeReloadPending || g_EntityBrowser.m_treeReloadQueued ) {
		return;
	}
	if ( QTreeView* treeView = g_EntityBrowser.m_treeView; treeView != nullptr ) {
		g_EntityBrowser.m_treeReloadQueued = true;
		QTimer::singleShot( 0, treeView, [](){
			g_EntityBrowser.m_treeReloadQueued = false;
			if ( std::exchange( g_EntityBrowser.m_treeReloadPending, false ) ) {
				EntityBrowser_reloadTree();
			}
		} );
	}
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

QWidget* EntityBrowser_constructWindow( QWidget* toplevel ){
	g_EntityBrowser.m_parent = toplevel;
	g_entityBrowserTreeConstructed = false;

	const bool disableOpenGL = OpenGLWidgetsDisabled();

	auto *splitter = new QSplitter;
	auto *containerWidgetLeft = new QWidget;
	auto *containerWidgetRight = new QWidget;
	splitter->addWidget( containerWidgetLeft );
	splitter->addWidget( containerWidgetRight );
	auto *vbox = new QVBoxLayout( containerWidgetLeft );
	auto *vboxRight = new QVBoxLayout( containerWidgetRight );

	vbox->setContentsMargins( 0, 0, 0, 0 );
	vboxRight->setContentsMargins( 0, 0, 0, 0 );
	vbox->setSpacing( 0 );
	vboxRight->setSpacing( 0 );

	{	// menu bar
		auto *toolbar = new QToolBar;
		vbox->addWidget( toolbar );
		const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
		toolbar->setIconSize( QSize( iconSize, iconSize ) );

		auto* menu_view = new QMenu( toolbar );
		menu_view->addAction( "Reset Preview Scroll", [](){
			g_EntityBrowser.setOriginZ( 0 );
			g_EntityBrowser.queueDraw();
		} );
		menu_view->addAction( "Expand Categories", [](){
			if ( g_EntityBrowser.m_treeView != nullptr ) {
				g_EntityBrowser.m_treeView->expandAll();
			}
		} );
		menu_view->addAction( "Collapse Categories", [](){
			if ( g_EntityBrowser.m_treeView != nullptr ) {
				g_EntityBrowser.m_treeView->collapseAll();
			}
		} );
		auto* listModeAction = menu_view->addAction( "List View" );
		listModeAction->setCheckable( true );
		listModeAction->setChecked( g_EntityBrowser.m_listMode );
		QObject::connect( listModeAction, &QAction::toggled, []( bool checked ){
			EntityBrowser_setListMode( checked );
		} );
		menu_view->setParent( toolbar, menu_view->windowFlags() );

		toolbar_append_button( toolbar, "View", "texbro_view.png", PointerCaller<QMenu, void(), +[]( QMenu* menu ){
			menu->popup( QCursor::pos() );
		}>( menu_view ) );
		auto* listModeButton = g_EntityBrowser.m_listModeButton = new QToolButton;
		listModeButton->setAutoRaise( true );
		listModeButton->setCheckable( true );
		listModeButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		listModeButton->setText( "List" );
		listModeButton->setToolTip( "Toggle list/icon view" );
		listModeButton->setChecked( g_EntityBrowser.m_listMode );
		QObject::connect( listModeButton, &QToolButton::toggled, [listModeAction]( bool checked ){
			if ( listModeAction->isChecked() != checked ) {
				listModeAction->setChecked( checked );
			}
			EntityBrowser_setListMode( checked );
		} );
		toolbar->addWidget( listModeButton );
		toolbar_append_button( toolbar, "Find / Replace...", "texbro_find-replace.png", "FindReplaceEntities" );
		toolbar_append_button( toolbar, "Flush / Reload Entity List", "refresh_models.png", makeCallbackF( EntityBrowser_reloadTree ) );
	}
	{	// filter bar
		auto *filterBar = new QWidget;
		auto *filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		QLineEdit *entry = g_EntityBrowser.m_filterEntry = new QLineEdit;
		filterLayout->addWidget( entry, 1 );
		entry->setClearButtonEnabled( true );
		entry->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		entry->setPlaceholderText( "Filter by name" );

		auto* globalButton = g_EntityBrowser.m_globalFilterButton = new QToolButton;
		globalButton->setAutoRaise( true );
		globalButton->setFocusPolicy( Qt::NoFocus );
		globalButton->setCheckable( true );
		globalButton->setIcon( new_local_icon( "f-world.png" ) );
		globalButton->setToolTip( "Global switch: filter across all categories" );
		filterLayout->addWidget( globalButton );

		auto* usedButton = g_EntityBrowser.m_usedFilterButton = new QToolButton;
		usedButton->setAutoRaise( true );
		usedButton->setFocusPolicy( Qt::NoFocus );
		usedButton->setCheckable( true );
		usedButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		usedButton->setText( "Used (0)" );
		usedButton->setToolTip( "Show only entity classes used in the current level" );
		filterLayout->addWidget( usedButton );

		auto *clearButton = g_EntityBrowser.m_clearFiltersButton = new QToolButton;
		clearButton->setAutoRaise( true );
		clearButton->setFocusPolicy( Qt::NoFocus );
		clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		clearButton->setToolTip( "Clear filters" );
		filterLayout->addWidget( clearButton );

		entry->setText( g_EntityBrowser.filter() );
		globalButton->setChecked( g_EntityBrowser.m_filterGlobal );
		usedButton->setChecked( g_EntityBrowser.m_filterUsed );
		auto* filterApplyTimer = g_EntityBrowser.m_filterApplyTimer = new QTimer( filterBar );
		filterApplyTimer->setSingleShot( true );
		QObject::connect( filterApplyTimer, &QTimer::timeout, [](){
			EntityBrowser_applyCurrentFilterNow();
		} );

		QObject::connect( clearButton, &QToolButton::clicked, [entry, globalButton, usedButton](){
			EntityBrowser_cancelPendingFilterApply();
			g_EntityBrowser.setFilter( "" );
			g_EntityBrowser.m_filterGlobal = false;
			g_EntityBrowser.m_filterUsed = false;
			{
				const QSignalBlocker entryBlocker( entry );
				const QSignalBlocker globalBlocker( globalButton );
				const QSignalBlocker usedBlocker( usedButton );
				entry->clear();
				globalButton->setChecked( false );
				usedButton->setChecked( false );
			}
			EntityBrowser_applyCurrentFilterNow();
		} );
		QObject::connect( entry, &QLineEdit::textChanged, []( const QString& text ){
			g_EntityBrowser.setFilter( text.toLatin1().constData() );
			EntityBrowser_updateClearFiltersButton();
			EntityBrowser_scheduleFilterApply();
		} );
		QObject::connect( globalButton, &QToolButton::toggled, []( bool checked ){
			g_EntityBrowser.m_filterGlobal = checked;
			EntityBrowser_applyCurrentFilterNow();
		} );
		QObject::connect( usedButton, &QToolButton::toggled, []( bool checked ){
			g_EntityBrowser.m_filterUsed = checked;
			EntityBrowser_applyCurrentFilterNow();
		} );

		EntityBrowser_updateClearFiltersButton();

		vbox->addWidget( filterBar );
	}
	{	// TreeView
		g_EntityBrowser.m_treeView = new TexBro_QTreeView;
		g_EntityBrowser.m_treeView->setHeaderHidden( true );
		g_EntityBrowser.m_treeView->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
		g_EntityBrowser.m_treeView->setUniformRowHeights( true );
		g_EntityBrowser.m_treeView->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		g_EntityBrowser.m_treeView->setExpandsOnDoubleClick( false );
		g_EntityBrowser.m_treeView->header()->setStretchLastSection( false );
		g_EntityBrowser.m_treeView->header()->setSectionResizeMode( QHeaderView::ResizeMode::ResizeToContents );

		QObject::connect( g_EntityBrowser.m_treeView, &QAbstractItemView::clicked, []( const QModelIndex& index ){
			EntityBrowser_selectCategory( index.data( Qt::ItemDataRole::DisplayRole ).toString() );
		} );

		vbox->addWidget( g_EntityBrowser.m_treeView );
	}
	{
		auto* viewStack = g_EntityBrowser.m_viewStack = new QStackedWidget;
		vboxRight->addWidget( viewStack, 1 );

		auto* previewPage = new QWidget;
		auto* previewLayout = new QHBoxLayout( previewPage );
		previewLayout->setContentsMargins( 0, 0, 0, 0 );
		previewLayout->setSpacing( 0 );

		if ( disableOpenGL ) {
			g_EntityBrowser.m_gl_widget = nullptr;
			previewLayout->addWidget( glwidget_createDisabledPlaceholder( "OpenGL disabled (Entities)", nullptr ) );
			g_EntityBrowser.m_gl_scroll = nullptr;
		}
		else {
			g_EntityBrowser.m_gl_widget = new EntityBrowserGLWidget( g_EntityBrowser );
			previewLayout->addWidget( g_EntityBrowser.m_gl_widget );

			auto *scroll = g_EntityBrowser.m_gl_scroll = new QScrollBar;
			previewLayout->addWidget( scroll );
			QObject::connect( scroll, &QAbstractSlider::valueChanged, []( int value ){
				g_EntityBrowser.m_scrollAdjustment.value_changed( value );
			} );
		}
		viewStack->addWidget( previewPage );

		auto* listPage = new QWidget;
		auto* listLayout = new QVBoxLayout( listPage );
		listLayout->setContentsMargins( 0, 0, 0, 0 );
		listLayout->setSpacing( 0 );
		auto* listWidget = g_EntityBrowser.m_listWidget = new QTreeWidget;
		listWidget->setColumnCount( 1 );
		listWidget->setHeaderHidden( true );
		listWidget->setRootIsDecorated( false );
		listWidget->setUniformRowHeights( true );
		listWidget->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
		listWidget->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
		listWidget->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		QObject::connect( listWidget, &QTreeWidget::itemPressed, []( QTreeWidgetItem* item, int column ){
			(void)column;
			if ( item != nullptr ) {
				g_EntityBrowser.m_currentEntityId = item->data( 0, Qt::ItemDataRole::UserRole ).toInt();
			}
		} );
		listLayout->addWidget( listWidget, 1 );
		viewStack->addWidget( listPage );
		EntityBrowser_rebuildListWidget();
		viewStack->setCurrentIndex( g_EntityBrowser.m_listMode ? 1 : 0 );
	}

	g_guiSettings.addSplitter( splitter, "EntityBrowser/splitter", { 100, 500 } );

	return splitter;
}

void EntityBrowser_EnsureTree(){
	if ( g_EntityBrowser.m_referenceRefreshInProgress ) {
		return;
	}
	if ( !g_entityBrowserTreeConstructed ) {
		EntityBrowser_constructTree();
	}
}

void EntityBrowser_destroyWindow(){
	EntityBrowser_cancelPendingFilterApply();
	g_entityBrowserTreeConstructed = false;
	g_EntityBrowser.m_parent = nullptr;
	g_EntityBrowser.m_viewStack = nullptr;
	g_EntityBrowser.m_gl_widget = nullptr;
	g_EntityBrowser.m_gl_scroll = nullptr;
	g_EntityBrowser.m_listWidget = nullptr;
	g_EntityBrowser.m_treeView = nullptr;
	g_EntityBrowser.m_filterEntry = nullptr;
	g_EntityBrowser.m_globalFilterButton = nullptr;
	g_EntityBrowser.m_usedFilterButton = nullptr;
	g_EntityBrowser.m_clearFiltersButton = nullptr;
	g_EntityBrowser.m_listModeButton = nullptr;
	g_EntityBrowser.m_filterApplyTimer = nullptr;
	g_EntityBrowser.m_filterApplyPending = false;
	g_EntityBrowser.m_referenceRefreshInProgress = false;
	g_EntityBrowser.m_referencesDirty = false;
	g_EntityBrowser.m_previewLoadInProgress = false;
	g_EntityBrowser.m_treeReloadPending = false;
	g_EntityBrowser.m_treeReloadQueued = false;
}

void EntityBrowser_flushReferences(){
	EntityBrowser_cancelPendingFilterApply();
	if ( g_entityGraph == nullptr ) {
		return;
	}

	++g_EntityBrowser.m_previewGraphRevision;
	EntityGraph_clear();
	g_EntityBrowser.m_loadedPreviewCount = 0;
	g_EntityBrowser.queueDraw();
}

void EntityBrowser_beginReferenceRefresh(){
	EntityBrowser_pausePendingFilterApply();
	g_EntityBrowser.m_referenceRefreshInProgress = true;
}

bool EntityBrowser_canRefreshReferences(){
	return !g_EntityBrowser.m_previewLoadInProgress;
}

void EntityBrowser_endReferenceRefresh(){
	g_EntityBrowser.m_referenceRefreshInProgress = false;
	if ( std::exchange( g_EntityBrowser.m_referencesDirty, false ) ) {
		if ( g_entityGraph != nullptr ) {
			++g_EntityBrowser.m_previewGraphRevision;
			EntityGraph_clear();
		}
		g_EntityBrowser.m_loadedPreviewCount = 0;
		g_EntityBrowser.m_currentEntityId = -1;
		g_EntityBrowser.clearHover();
		if ( !g_EntityBrowser.m_filterApplyPending ) {
			g_EntityBrowser.queueDraw();
		}
	}
	if ( g_EntityBrowser.m_treeReloadPending ) {
		EntityBrowser_queuePendingTreeReload();
	}
	else if ( g_EntityBrowser.m_filterApplyPending ) {
		EntityBrowser_scheduleFilterApply();
	}
	if ( !g_entityBrowserTreeConstructed ) {
		if ( QTreeView* treeView = g_EntityBrowser.m_treeView; treeView != nullptr && treeView->isVisible() ) {
			QTimer::singleShot( 0, treeView, [treeView](){
				if ( treeView->isVisible() && !g_EntityBrowser.m_referenceRefreshInProgress ) {
					EntityBrowser_EnsureTree();
				}
			} );
		}
	}
}

#include "preferencesystem.h"
#include "stringio.h"

void EntityBrowser_Construct(){
	GlobalPreferenceSystem().registerPreference( "EntityBrowserFilter",
	                                             CopiedStringImportStringCaller( g_EntityBrowser.filterStorage() ),
	                                             CopiedStringExportStringCaller( g_EntityBrowser.filterStorage() ) );
	GlobalPreferenceSystem().registerPreference( "EntityBrowserFilterGlobal",
	                                             BoolImportStringCaller( g_EntityBrowser.m_filterGlobal ),
	                                             BoolExportStringCaller( g_EntityBrowser.m_filterGlobal ) );
	GlobalPreferenceSystem().registerPreference( "EntityBrowserFilterUsed",
	                                             BoolImportStringCaller( g_EntityBrowser.m_filterUsed ),
	                                             BoolExportStringCaller( g_EntityBrowser.m_filterUsed ) );
	GlobalPreferenceSystem().registerPreference( "EntityBrowserListMode",
	                                             BoolImportStringCaller( g_EntityBrowser.m_listMode ),
	                                             BoolExportStringCaller( g_EntityBrowser.m_listMode ) );
	g_entityGraph = new EntityGraph( g_EntityBrowser );
	g_entityGraph->insert_root( ( new EntityGraphRoot )->node() );
}

void EntityBrowser_Destroy(){
	EntityBrowser_cancelPendingFilterApply();
	++g_EntityBrowser.m_previewGraphRevision;
	g_entityGraph->erase_root();
	delete g_entityGraph;
	g_entityGraph = nullptr;
}
