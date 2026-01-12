#include "entitybrowser.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDrag>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QOpenGLWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardItemModel>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "assetdrop.h"
#include "eclasslib.h"
#include "ieclass.h"
#include "ientity.h"
#include "igl.h"
#include "irender.h"
#include "iscenegraph.h"
#include "instancelib.h"
#include "mainframe.h"
#include "renderable.h"
#include "renderer.h"
#include "scenelib.h"
#include "selectionlib.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "traverselib.h"
#include "view.h"

#include "generic/callback.h"

#include "gtkutil/cursor.h"
#include "gtkutil/fbo.h"
#include "gtkutil/glfont.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/guisettings.h"
#include "gtkutil/image.h"
#include "gtkutil/mousepresses.h"
#include "gtkutil/toolbar.h"
#include "gtkutil/widget.h"

namespace {
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
	CellPos( int width, int cellSize, int fontHeight ) :
		m_cellSize( cellSize ), m_fontHeight( fontHeight ),
		m_fontDescent( GlobalOpenGL().m_font->getPixelDescent() ),
		m_plusWidth( 8 ),
		m_plusHeight( 0 ),
		m_cellsInRow( std::max( 1, ( width - m_plusWidth ) / ( m_cellSize * 2 + m_plusWidth ) ) ){
	}
	void operator++(){
		++m_index;
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
	int totalHeight( int height, int cellCount ) const {
		return std::max( height, ( ( cellCount - 1 ) / m_cellsInRow + 1 ) * ( m_cellSize * 2 + m_fontHeight + m_plusHeight ) + m_fontHeight );
	}
	int testSelect( int x, int z ) const {
		if( x < 0 || z > 0 ) {
			return -1;
		}
		const int col = x / ( m_cellSize * 2 + m_plusWidth );
		const int row = -z / ( m_cellSize * 2 + m_fontHeight + m_plusHeight );
		const int index = row * m_cellsInRow + col;
		if ( index < 0 ) {
			return -1;
		}
		return index;
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
	QOpenGLWidget* m_gl_widget = nullptr;
	QScrollBar* m_gl_scroll = nullptr;
	QTreeView* m_treeView = nullptr;
	QLineEdit* m_filterEntry = nullptr;

	int m_width = 0;
	int m_height = 0;

	int m_originZ = 0;
	DeferredAdjustment m_scrollAdjustment;

	int m_cellSize = 80;
	int m_currentEntityId = -1;

	CellPos constructCellPos() const {
		return CellPos( m_width, m_cellSize, GlobalOpenGL().m_font->getPixelHeight() );
	}
	void testSelect( int x, int z ){
		m_currentEntityId = constructCellPos().testSelect( x, z - m_originZ );
		if( m_currentEntityId >= static_cast<int>( m_visibleClasses.size() ) )
			m_currentEntityId = -1;
	}
	const EntityClass* currentEntityClass() const {
		if ( m_currentEntityId < 0 || m_currentEntityId >= static_cast<int>( m_visibleClasses.size() ) ) {
			return nullptr;
		}
		return m_visibleClasses[m_currentEntityId];
	}
private:
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

class entities_set_transforms
{
	mutable CellPos m_cellPos = g_EntityBrowser.constructCellPos();
public:
	void operator()( scene::Instance* instance ) const {
		if( TransformNode *transformNode = Node_getTransformNode( instance->path().parent() ) ){
			if( Bounded *bounded = Instance_getBounded( *instance ) ){
				AABB aabb = bounded->localAABB();
				const float max_extent = std::max( { aabb.extents[0], aabb.extents[1], aabb.extents[2] } );
				const float scale = max_extent > 0.0f ? m_cellPos.getCellSize() / max_extent : 1.0f;
				const_cast<Matrix4&>( transformNode->localToParent() ) =
				        matrix4_multiplied_by_matrix4(
				            matrix4_translation_for_vec3( m_cellPos.getOrigin() ),
				            matrix4_multiplied_by_matrix4(
				                matrix4_scale_for_vec3( Vector3( scale, scale, scale ) ),
				                matrix4_translation_for_vec3( -aabb.origin )
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

void EntityBrowser_render(){
	g_EntityBrowser.validate();

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

		{	// one directional light source directly behind the viewer
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
			EntityRenderer renderer( globalstate );

			g_EntityBrowser.forEachEntityInstance( [&renderer, &m_view]( scene::Instance* instance ){
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
		{	// render entity class names
			CellPos cellPos = g_EntityBrowser.constructCellPos();
			for( const EntityClass* eclass : g_EntityBrowser.visibleClasses() ){
				const Vector3 pos = cellPos.getTextPos();
				if( m_view.TestPoint( pos ) ){
					gl().glRasterPos3f( pos.x(), pos.y(), pos.z() );
					GlobalOpenGL().drawString( eclass->name() );
				}
				++cellPos;
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
		m_entBro.forEachEntityInstance( entities_set_transforms() );

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
			m_entBro.tracking_MouseDown();
			if ( press == MousePresses::Left ) {
				const QPoint localPos = mouseEventLocalPos( event );
				m_dragStart = localPos;
				m_entBro.testSelect( localPos.x() * m_scale, localPos.y() * m_scale );
			}
		}
	}
	void mouseMoveEvent( QMouseEvent *event ) override {
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

		auto* drag = new QDrag( this );
		drag->setMimeData( mimeData );
		drag->exec( Qt::CopyAction );
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

static void EntityBrowser_selectCategory( const QString& name ){
	const EntityCategory* category = g_EntityBrowser.findCategory( name.toLatin1().constData() );
	g_EntityBrowser.setCurrentCategory( category );

	EntityGraph_clear();
	g_EntityBrowser.visibleClasses().clear();
	if ( category != nullptr ) {
		for ( EntityClass* eclass : category->classes ) {
			if ( string_contains_nocase( eclass->name(), g_EntityBrowser.filter() ) ) {
				g_EntityBrowser.visibleClasses().push_back( eclass );
			}
		}

		if ( scene::Traversable* traversable = Node_getTraversable( g_entityGraph->root() ) ) {
			for ( EntityClass* eclass : g_EntityBrowser.visibleClasses() ) {
				NodeSmartReference node( GlobalEntityCreator().createEntity( eclass ) );
				traversable->insert( node );
			}
		}
		g_EntityBrowser.forEachEntityInstance( entities_set_transforms() );
	}
	g_EntityBrowser.queueDraw();
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
	EntityBrowser_constructCategories();

	auto *model = new QStandardItemModel( g_EntityBrowser.m_treeView );
	for ( const EntityCategory& category : g_EntityBrowser.categories() ) {
		model->invisibleRootItem()->appendRow( new QStandardItem( category.name.c_str() ) );
	}

	g_EntityBrowser.m_treeView->setModel( model );

	if ( model->rowCount() > 0 ) {
		const QModelIndex first = model->index( 0, 0 );
		g_EntityBrowser.m_treeView->setCurrentIndex( first );
		EntityBrowser_selectCategory( first.data( Qt::ItemDataRole::DisplayRole ).toString() );
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

	const bool disableOpenGL = OpenGLWidgetsDisabled();

	auto *splitter = new QSplitter;
	auto *containerWidgetLeft = new QWidget;
	auto *containerWidgetRight = new QWidget;
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

		toolbar_append_button( toolbar, "Reload Entity Classes", "refresh_modelstree.png", FreeCaller<void(), EntityBrowser_constructTree>() );
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
		entry->setPlaceholderText( "Filter entities" );

		auto *clearButton = new QToolButton;
		clearButton->setAutoRaise( true );
		clearButton->setFocusPolicy( Qt::NoFocus );
		clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		clearButton->setToolTip( "Clear filter" );
		filterLayout->addWidget( clearButton );

		QObject::connect( clearButton, &QToolButton::clicked, [](){
			if ( g_EntityBrowser.m_filterEntry != nullptr ) {
				g_EntityBrowser.m_filterEntry->clear();
			}
		} );
		QObject::connect( entry, &QLineEdit::textChanged, []( const QString& text ){
			g_EntityBrowser.setFilter( text.toLatin1().constData() );
			if ( const EntityCategory* category = g_EntityBrowser.currentCategory() ) {
				EntityBrowser_selectCategory( category->name.c_str() );
			}
		} );

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

		EntityBrowser_constructTree();

		vbox->addWidget( g_EntityBrowser.m_treeView );
	}
	{	// gl_widget
		if ( disableOpenGL ) {
			g_EntityBrowser.m_gl_widget = nullptr;
			hbox->addWidget( glwidget_createDisabledPlaceholder( "OpenGL disabled (Entities)", nullptr ) );
		}
		else {
			g_EntityBrowser.m_gl_widget = new EntityBrowserGLWidget( g_EntityBrowser );
			hbox->addWidget( g_EntityBrowser.m_gl_widget );
		}
	}
	{	// gl_widget scrollbar
		if ( !disableOpenGL ) {
			auto *scroll = g_EntityBrowser.m_gl_scroll = new QScrollBar;
			hbox->addWidget( scroll );

			QObject::connect( scroll, &QAbstractSlider::valueChanged, []( int value ){
				g_EntityBrowser.m_scrollAdjustment.value_changed( value );
			} );
		}
		else {
			g_EntityBrowser.m_gl_scroll = nullptr;
		}
	}

	g_guiSettings.addSplitter( splitter, "EntityBrowser/splitter", { 100, 500 } );

	return splitter;
}

void EntityBrowser_destroyWindow(){
	g_EntityBrowser.m_gl_widget = nullptr;
}

void EntityBrowser_flushReferences(){
	if ( g_entityGraph == nullptr ) {
		return;
	}

	EntityGraph_clear();
	g_EntityBrowser.queueDraw();
}

void EntityBrowser_Construct(){
	g_entityGraph = new EntityGraph( g_EntityBrowser );
	g_entityGraph->insert_root( ( new EntityGraphRoot )->node() );
}

void EntityBrowser_Destroy(){
	g_entityGraph->erase_root();
	delete g_entityGraph;
	g_entityGraph = nullptr;
}
