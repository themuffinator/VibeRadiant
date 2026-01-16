#include "soundbrowser.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDrag>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMimeData>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardItemModel>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QUrl>

#include "assetdrop.h"
#include "iarchive.h"
#include "idatastream.h"
#include "ifiletypes.h"
#include "ifilesystem.h"
#include "igl.h"
#include "mainframe.h"
#include "math/vector.h"
#include "os/file.h"
#include "string/string.h"
#include "stream/stringstream.h"
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

#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
#include <QAudioOutput>
#endif

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

constexpr float kAssetBrowserHoverScale = 1.05f;
constexpr float kAssetBrowserHoverLerp = 0.2f;
constexpr float kAssetBrowserHoverEpsilon = 0.001f;

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
} // namespace

class SoundFS
{
public:
	const CopiedString m_folderName;
	SoundFS() = default;
	SoundFS( const StringRange range ) : m_folderName( range ){
	}
	bool operator<( const SoundFS& other ) const {
		return string_less( m_folderName.c_str(), other.m_folderName.c_str() );
	}
	mutable std::set<SoundFS> m_folders;
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

class SoundBrowser
{
public:
	SoundBrowser() : m_scrollAdjustment( [this]( int value ){
		setOriginZ( -value );
	} ){
	}

	const int m_MSAA = 8;
	Vector3 m_background_color = Vector3( .25f );

	QWidget* m_parent = nullptr;
	QOpenGLWidget* m_gl_widget = nullptr;
	QScrollBar* m_gl_scroll = nullptr;
	QTreeView* m_treeView = nullptr;
	QLineEdit* m_filterEntry = nullptr;

	SoundFS m_soundFS;
	CopiedString m_currentFolderPath;
	const SoundFS* m_currentFolder = nullptr;
	std::vector<CopiedString> m_visibleFiles;
	CopiedString m_filter;

	int m_width = 0;
	int m_height = 0;
	int m_originZ = 0;
	DeferredAdjustment m_scrollAdjustment;
	int m_cellSize = 80;
	int m_currentSoundId = -1;
	int m_hoverSoundId = -1;
	float m_hoverScale = 1.0f;
	float m_hoverScaleTarget = 1.0f;

	QMediaPlayer* m_player = nullptr;
	QBuffer* m_soundBuffer = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
	QAudioOutput* m_audioOutput = nullptr;
#endif
	int m_playingSoundId = -1;

	CellPos constructCellPos() const {
		return CellPos( m_width, m_cellSize, GlobalOpenGL().m_font->getPixelHeight() );
	}
	void testSelect( int x, int z ){
		m_currentSoundId = constructCellPos().testSelect( x, z - m_originZ );
		if( m_currentSoundId >= static_cast<int>( m_visibleFiles.size() ) )
			m_currentSoundId = -1;
	}
	void updateHover( int x, int z ){
		setHoverId( constructCellPos().testSelect( x, z - m_originZ ) );
	}
	void clearHover(){
		setHoverId( -1 );
	}
	int hoverSoundId() const {
		return m_hoverSoundId;
	}
	float hoverScale() const {
		return m_hoverScale;
	}
	float hoverScaleForIndex( int index ) const {
		return index == m_hoverSoundId ? m_hoverScale : 1.0f;
	}
	float hoverAlpha() const {
		if ( m_hoverSoundId < 0 ) {
			return 0.0f;
		}
		return std::clamp( ( m_hoverScale - 1.0f ) / ( kAssetBrowserHoverScale - 1.0f ), 0.0f, 1.0f );
	}
	bool updateHoverAnimation(){
		if ( m_hoverSoundId < 0 ) {
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			return false;
		}
		if ( m_hoverSoundId >= static_cast<int>( m_visibleFiles.size() ) ) {
			m_hoverSoundId = -1;
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			return false;
		}
		const float previous = m_hoverScale;
		m_hoverScale = AssetBrowser_approachHoverScale( m_hoverScale, m_hoverScaleTarget );
		if ( std::fabs( m_hoverScale - m_hoverScaleTarget ) < kAssetBrowserHoverEpsilon ) {
			m_hoverScale = m_hoverScaleTarget;
		}
		else{
			queueDraw();
		}
		if ( m_hoverScaleTarget <= 1.0f + kAssetBrowserHoverEpsilon
		  && m_hoverScale <= 1.0f + kAssetBrowserHoverEpsilon ) {
			m_hoverScale = 1.0f;
			m_hoverScaleTarget = 1.0f;
			m_hoverSoundId = -1;
		}
		return std::fabs( m_hoverScale - previous ) > kAssetBrowserHoverEpsilon;
	}
	CopiedString currentSoundPath() const {
		return soundPathForIndex( m_currentSoundId );
	}
	CopiedString soundPathForIndex( int index ) const {
		if ( index < 0 || index >= static_cast<int>( m_visibleFiles.size() ) ) {
			return CopiedString( "" );
		}
		return CopiedString( StringStream<256>( "sound/", m_currentFolderPath, m_visibleFiles[index].c_str() ) );
	}
	bool isPlayingIndex( int index ) const {
		return index >= 0 && index == m_playingSoundId;
	}
	void stopPlayback(){
		if ( m_player == nullptr ) {
			return;
		}
		m_player->stop();
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
		m_player->setSource( QUrl() );
		m_player->setSourceDevice( nullptr, QUrl() );
#else
		m_player->setMedia( QMediaContent() );
#endif
		if ( m_soundBuffer != nullptr ) {
			m_soundBuffer->close();
			delete m_soundBuffer;
			m_soundBuffer = nullptr;
		}
		m_playingSoundId = -1;
		queueDraw();
	}
	bool playSoundForIndex( int index ){
		if ( index < 0 || index >= static_cast<int>( m_visibleFiles.size() ) ) {
			return false;
		}
		ensureAudio();
		stopPlayback();
		const CopiedString soundPath = soundPathForIndex( index );
		if ( soundPath.empty() || !loadSound( soundPath.c_str() ) ) {
			return false;
		}
		m_playingSoundId = index;
		queueDraw();
		m_player->play();
		return true;
	}
private:
	void ensureAudio(){
		if ( m_player != nullptr ) {
			return;
		}
		m_player = new QMediaPlayer( m_parent );
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
		m_audioOutput = new QAudioOutput( m_parent );
		m_player->setAudioOutput( m_audioOutput );
#else
		m_player->setVolume( 100 );
#endif
		QObject::connect( m_player, &QMediaPlayer::mediaStatusChanged, [this]( QMediaPlayer::MediaStatus status ){
			if ( status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia ) {
				m_playingSoundId = -1;
				queueDraw();
			}
		} );
	}
	bool loadSound( const char* soundPath ){
		const char* root = GlobalFileSystem().findFile( soundPath );
		if ( !string_empty( root ) ) {
			const auto absolute = StringStream<512>( root, soundPath );
			if ( file_exists( absolute ) ) {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
				m_player->setSource( QUrl::fromLocalFile( QString::fromLatin1( absolute.c_str() ) ) );
#else
				m_player->setMedia( QUrl::fromLocalFile( QString::fromLatin1( absolute.c_str() ) ) );
#endif
				return true;
			}
		}

		ArchiveFile* file = GlobalFileSystem().openFile( soundPath );
		if ( file == nullptr ) {
			return false;
		}

		const std::size_t size = file->size();
		QByteArray data;
		if ( size > 0 ) {
			data.resize( static_cast<int>( size ) );
			InputStream& stream = file->getInputStream();
			std::size_t total = 0;
			while ( total < size ) {
				const std::size_t read = stream.read( reinterpret_cast<InputStream::byte_type*>( data.data() + total ), size - total );
				if ( read == 0 ) {
					break;
				}
				total += read;
			}
			if ( total < size ) {
				data.truncate( static_cast<int>( total ) );
			}
		}
		file->release();
		if ( data.isEmpty() ) {
			return false;
		}

		if ( m_soundBuffer != nullptr ) {
			m_soundBuffer->close();
			delete m_soundBuffer;
			m_soundBuffer = nullptr;
		}
		m_soundBuffer = new QBuffer( m_player );
		m_soundBuffer->setData( data );
		if ( !m_soundBuffer->open( QIODevice::ReadOnly ) ) {
			delete m_soundBuffer;
			m_soundBuffer = nullptr;
			return false;
		}
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
		m_player->setSourceDevice( m_soundBuffer, QUrl( QString::fromLatin1( soundPath ) ) );
#else
		m_player->setMedia( QUrl( QString::fromLatin1( soundPath ) ), m_soundBuffer );
#endif
		return true;
	}
	void setHoverId( int hoverId ){
		if ( hoverId >= static_cast<int>( m_visibleFiles.size() ) ) {
			hoverId = -1;
		}
		if ( hoverId < 0 ) {
			if ( m_hoverSoundId >= 0 && m_hoverScaleTarget != 1.0f ) {
				m_hoverScaleTarget = 1.0f;
				queueDraw();
			}
			return;
		}

		const bool idChanged = hoverId != m_hoverSoundId;
		if ( idChanged ) {
			m_hoverSoundId = hoverId;
			m_hoverScale = 1.0f;
		}
		if ( idChanged || m_hoverScaleTarget != kAssetBrowserHoverScale ) {
			m_hoverScaleTarget = kAssetBrowserHoverScale;
			queueDraw();
		}
	}
	int totalHeight() const {
		return constructCellPos().totalHeight( m_height, m_visibleFiles.size() );
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
	void setFilter( const char* filter ){
		m_filter = filter;
	}
	const char* filter() const {
		return m_filter.c_str();
	}
};

SoundBrowser g_SoundBrowser;

static void SoundBrowser_updateVisibleFiles(){
	g_SoundBrowser.m_visibleFiles.clear();
	g_SoundBrowser.m_currentSoundId = -1;
	g_SoundBrowser.stopPlayback();
	g_SoundBrowser.clearHover();
	if ( g_SoundBrowser.m_currentFolder == nullptr ) {
		g_SoundBrowser.queueDraw();
		return;
	}

	for ( const CopiedString& file : g_SoundBrowser.m_currentFolder->m_files ) {
		if ( string_contains_nocase( file.c_str(), g_SoundBrowser.filter() ) ) {
			g_SoundBrowser.m_visibleFiles.push_back( file );
		}
	}
	g_SoundBrowser.m_originZ = 0;
	g_SoundBrowser.m_originInvalid = true;
	g_SoundBrowser.queueDraw();
}

static QRect SoundBrowser_cellRectPixels( const SoundBrowser& browser, int index ){
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

static QPixmap SoundBrowser_dragPixmap( SoundBrowser& browser ){
	if ( browser.m_gl_widget == nullptr || browser.m_currentSoundId < 0 ) {
		return QPixmap();
	}
	QImage frame = browser.m_gl_widget->grabFramebuffer();
	QRect rect = SoundBrowser_cellRectPixels( browser, browser.m_currentSoundId ).intersected( frame.rect() );
	if ( rect.isEmpty() ) {
		return QPixmap();
	}
	QImage tile = applyOpacity( frame.copy( rect ), 0.6f );
	QPixmap pixmap = QPixmap::fromImage( tile );
	pixmap.setDevicePixelRatio( browser.m_gl_widget->devicePixelRatioF() );
	return pixmap;
}

static void SoundBrowser_drawSpeaker( const Vector3& origin, const CellPos& cellPos, bool selected, bool playing, float scale ){
	const float size = cellPos.getCellSize() * 0.7f * scale;
	const float half = size * 0.5f;
	const float bodyWidth = size * 0.35f;
	const float bodyHalf = size * 0.35f;

	const float bodyMinX = origin.x() - half;
	const float bodyMaxX = bodyMinX + bodyWidth;
	const float bodyMinZ = origin.z() - bodyHalf;
	const float bodyMaxZ = origin.z() + bodyHalf;

	const float coneBaseX = bodyMaxX + size * 0.05f;
	const float coneTipX = origin.x() + half;

	if ( playing ) {
		gl().glColor4f( 0.9f, 0.2f, 0.2f, 1.f );
	}
	else{
		if ( selected ) {
			gl().glColor4f( 1.f, 0.9f, 0.2f, 1.f );
		}
		else{
			gl().glColor4f( 0.9f, 0.9f, 0.9f, 1.f );
		}
	}

	if ( playing ) {
		const float stopHalf = size * 0.3f;
		gl().glBegin( GL_QUADS );
		gl().glVertex3f( origin.x() - stopHalf, 0, origin.z() + stopHalf );
		gl().glVertex3f( origin.x() - stopHalf, 0, origin.z() - stopHalf );
		gl().glVertex3f( origin.x() + stopHalf, 0, origin.z() - stopHalf );
		gl().glVertex3f( origin.x() + stopHalf, 0, origin.z() + stopHalf );
		gl().glEnd();
	}
	else{
		gl().glBegin( GL_QUADS );
		gl().glVertex3f( bodyMinX, 0, bodyMaxZ );
		gl().glVertex3f( bodyMinX, 0, bodyMinZ );
		gl().glVertex3f( bodyMaxX, 0, bodyMinZ );
		gl().glVertex3f( bodyMaxX, 0, bodyMaxZ );
		gl().glEnd();

		gl().glBegin( GL_TRIANGLES );
		gl().glVertex3f( coneBaseX, 0, bodyMinZ );
		gl().glVertex3f( coneBaseX, 0, bodyMaxZ );
		gl().glVertex3f( coneTipX, 0, origin.z() );
		gl().glEnd();
	}
}

void SoundBrowser_render(){
	g_SoundBrowser.validate();
	g_SoundBrowser.updateHoverAnimation();

	const int W = g_SoundBrowser.m_width;
	const int H = g_SoundBrowser.m_height;
	gl().glViewport( 0, 0, W, H );

	gl().glDepthMask( GL_TRUE );
	gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	gl().glClearColor( g_SoundBrowser.m_background_color[0],
	                   g_SoundBrowser.m_background_color[1],
	                   g_SoundBrowser.m_background_color[2], 0 );
	gl().glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

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
	m_modelview[13] = H / 2.f - g_SoundBrowser.m_originZ;
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

	if( g_SoundBrowser.m_currentFolder != nullptr ){
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

		{	// brighter background squares
			gl().glColor4f( g_SoundBrowser.m_background_color[0] + .05f,
			                g_SoundBrowser.m_background_color[1] + .05f,
			                g_SoundBrowser.m_background_color[2] + .05f, 1.f );
			gl().glDepthMask( GL_FALSE );
			gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
			gl().glDisable( GL_CULL_FACE );

			CellPos cellPos = g_SoundBrowser.constructCellPos();
			gl().glBegin( GL_QUADS );
			for( std::size_t i = g_SoundBrowser.m_visibleFiles.size(); i != 0; --i ){
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

		{	// icons
			CellPos cellPos = g_SoundBrowser.constructCellPos();
			int index = 0;
			for ( const CopiedString& file : g_SoundBrowser.m_visibleFiles ) {
				(void)file;
				const Vector3 origin = cellPos.getOrigin();
				const bool selected = index == g_SoundBrowser.m_currentSoundId;
				const bool playing = g_SoundBrowser.isPlayingIndex( index );
				const float scale = g_SoundBrowser.hoverScaleForIndex( index );
				SoundBrowser_drawSpeaker( origin, cellPos, selected, playing, scale );
				++cellPos;
				++index;
			}
		}
		{	// hover outline
			const int hoverId = g_SoundBrowser.hoverSoundId();
			if ( hoverId >= 0 ) {
				const CellPos cellPos = g_SoundBrowser.constructCellPos();
				const Vector3 origin = cellPos.getOrigin( hoverId );
				const float cellSize = cellPos.getCellSize() * g_SoundBrowser.hoverScale();
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

		{	// render sound file names
			gl().glColor4f( 1, 1, 1, 1 );
			CellPos cellPos = g_SoundBrowser.constructCellPos();
			for( const CopiedString& file : g_SoundBrowser.m_visibleFiles ){
				const Vector3 pos = cellPos.getTextPos();
				if( m_view.TestPoint( pos ) ){
					gl().glRasterPos3f( pos.x(), pos.y(), pos.z() );
					GlobalOpenGL().drawString( file.c_str() );
				}
				++cellPos;
			}
		}
	}

	gl().glBindTexture( GL_TEXTURE_2D, 0 );
}

class SoundBrowserGLWidget : public QOpenGLWidget
{
	SoundBrowser& m_sndBro;
	FBO *m_fbo{};
	qreal m_scale = 1.0;
	MousePresses m_mouse;
	QPoint m_dragStart;
public:
	SoundBrowserGLWidget( SoundBrowser& soundBrowser ) : QOpenGLWidget(), m_sndBro( soundBrowser ){
		setMouseTracking( true );
	}

	~SoundBrowserGLWidget() override {
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
		m_sndBro.m_width = float_to_integer( w * m_scale );
		m_sndBro.m_height = float_to_integer( h * m_scale );
		m_sndBro.m_originInvalid = true;

		delete m_fbo;
		m_fbo = nullptr;
		if ( m_sndBro.m_width <= 0 || m_sndBro.m_height <= 0 ) {
			return;
		}
		m_fbo = new FBO( m_sndBro.m_width, m_sndBro.m_height, true, m_sndBro.m_MSAA );
	}
	void paintGL() override
	{
		if ( m_fbo == nullptr ) {
			return;
		}
		if( ScreenUpdates_Enabled() && m_fbo->bind() ){
			GlobalOpenGL_debugAssertNoErrors();
			SoundBrowser_render();
			GlobalOpenGL_debugAssertNoErrors();
			m_fbo->blit();
			m_fbo->release();
		}
	}

	void mousePressEvent( QMouseEvent *event ) override {
		setFocus();
		const auto press = m_mouse.press( event );
		if ( press == MousePresses::Left2x ) {
			const QPoint localPos = mouseEventLocalPos( event );
			m_sndBro.testSelect( localPos.x() * m_scale, localPos.y() * m_scale );
			m_sndBro.playSoundForIndex( m_sndBro.m_currentSoundId );
			return;
		}
		if ( press == MousePresses::Left || press == MousePresses::Right ) {
			m_sndBro.tracking_MouseDown();
			if ( press == MousePresses::Left ) {
				m_sndBro.stopPlayback();
				const QPoint localPos = mouseEventLocalPos( event );
				m_dragStart = localPos;
				m_sndBro.testSelect( localPos.x() * m_scale, localPos.y() * m_scale );
			}
		}
	}
	void mouseMoveEvent( QMouseEvent *event ) override {
		if ( event->buttons() == Qt::MouseButton::NoButton ) {
			const QPoint localPos = mouseEventLocalPos( event );
			m_sndBro.updateHover( localPos.x() * m_scale, localPos.y() * m_scale );
			return;
		}
		if ( !( event->buttons() & Qt::MouseButton::LeftButton ) ) {
			return;
		}
		const QPoint localPos = mouseEventLocalPos( event );
		if ( ( localPos - m_dragStart ).manhattanLength() < QApplication::startDragDistance() ) {
			return;
		}

		const CopiedString soundPath = m_sndBro.currentSoundPath();
		if ( soundPath.empty() ) {
			return;
		}

		m_sndBro.tracking_MouseUp();
		auto* mimeData = new QMimeData;
		mimeData->setData( kSoundBrowserMimeType, QByteArray( soundPath.c_str() ) );
		mimeData->setText( soundPath.c_str() );

		auto* drag = new QDrag( this );
		drag->setMimeData( mimeData );
		const QPixmap pixmap = SoundBrowser_dragPixmap( m_sndBro );
		if ( !pixmap.isNull() ) {
			drag->setPixmap( pixmap );
			drag->setHotSpot( pixmap.rect().center() );
		}
		drag->exec( Qt::CopyAction );
	}
	void leaveEvent( QEvent *event ) override {
		m_sndBro.clearHover();
		QOpenGLWidget::leaveEvent( event );
	}
	void mouseReleaseEvent( QMouseEvent *event ) override {
		const auto release = m_mouse.release( event );
		if ( release == MousePresses::Left || release == MousePresses::Right ) {
			m_sndBro.tracking_MouseUp();
		}
	}
	void wheelEvent( QWheelEvent *event ) override {
		setFocus();
		m_sndBro.setOriginZ( m_sndBro.m_originZ + std::copysign( 64, event->angleDelta().y() ) );
	}
};

static void SoundBrowser_selectFolder( const QModelIndex& index ){
	StringOutputStream sstream( 64 );
	const SoundFS *soundFS = &g_SoundBrowser.m_soundFS;
	{
		std::deque<QModelIndex> iters;
		iters.push_front( index );
		while( iters.front().parent().isValid() )
			iters.push_front( iters.front().parent() );
		for( const QModelIndex& i : iters ){
			const auto dir = i.data( Qt::ItemDataRole::DisplayRole ).toByteArray();
			if ( dir.isEmpty() ) {
				continue;
			}
			const auto found = soundFS->m_folders.find( SoundFS( StringRange( dir.constData(), strlen( dir.constData() ) ) ) );
			if( found != soundFS->m_folders.end() ){
				soundFS = &( *found );
				sstream << dir.constData() << '/';
			}
		}
	}

	g_SoundBrowser.m_currentFolder = soundFS;
	g_SoundBrowser.m_currentFolderPath = sstream;
	SoundBrowser_updateVisibleFiles();

	if ( g_SoundBrowser.m_treeView != nullptr ) {
		g_SoundBrowser.m_treeView->clearFocus();
	}
}

static void SoundBrowser_constructTreeModel( const SoundFS& soundFS, QStandardItemModel* model, QStandardItem* parent ){
	auto *item = new QStandardItem( soundFS.m_folderName.c_str() );
	parent->appendRow( item );
	for( const SoundFS& folder : soundFS.m_folders ){
		SoundBrowser_constructTreeModel( folder, model, item );
	}
}

static void SoundBrowser_addFromFileSystem( const char* name ){
	const char* relative = name;
	if ( string_equal_prefix( name, "sound/" ) ) {
		relative = name + 6;
	}
	g_SoundBrowser.m_soundFS.insert( relative );
}

static void SoundBrowser_constructTree(){
	g_SoundBrowser.m_soundFS.m_folders.clear();
	g_SoundBrowser.m_soundFS.m_files.clear();

	class : public IFileTypeList
	{
	public:
		struct StringLessNoCase
		{
			bool operator()( const CopiedString& a, const CopiedString& b ) const {
				return string_less_nocase( a.c_str(), b.c_str() );
			}
		};
		using StringSetNoCase = std::set<CopiedString, StringLessNoCase>;

		StringSetNoCase m_soundExtensions;
		void addType( const char* moduleName, filetype_t type ) override {
			m_soundExtensions.emplace( moduleName );
		}
	} typelist;
	GlobalFiletypes().getTypeList( "sound", &typelist, true, false, false );

	for ( const CopiedString& ext : typelist.m_soundExtensions ) {
		GlobalFileSystem().forEachFile( "sound/", ext.c_str(), makeCallbackF( SoundBrowser_addFromFileSystem ), 99 );
	}

	auto *model = new QStandardItemModel( g_SoundBrowser.m_treeView );

	{
		if( !g_SoundBrowser.m_soundFS.m_files.empty() ){
			model->invisibleRootItem()->appendRow( new QStandardItem( "" ) );
		}

		for( const SoundFS& folder : g_SoundBrowser.m_soundFS.m_folders )
			SoundBrowser_constructTreeModel( folder, model, model->invisibleRootItem() );
	}

	g_SoundBrowser.m_treeView->setModel( model );

	if ( model->rowCount() > 0 ) {
		const QModelIndex first = model->index( 0, 0 );
		g_SoundBrowser.m_treeView->setCurrentIndex( first );
		SoundBrowser_selectFolder( first );
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

QWidget* SoundBrowser_constructWindow( QWidget* toplevel ){
	g_SoundBrowser.m_parent = toplevel;

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

		toolbar_append_button( toolbar, "Reload Sound Tree", "refresh_modelstree.png", FreeCaller<void(), SoundBrowser_constructTree>() );
	}
	{	// filter bar
		auto *filterBar = new QWidget;
		auto *filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		QLineEdit *entry = g_SoundBrowser.m_filterEntry = new QLineEdit;
		filterLayout->addWidget( entry, 1 );
		entry->setClearButtonEnabled( true );
		entry->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		entry->setPlaceholderText( "Filter sounds" );

		auto *clearButton = new QToolButton;
		clearButton->setAutoRaise( true );
		clearButton->setFocusPolicy( Qt::NoFocus );
		clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		clearButton->setToolTip( "Clear filter" );
		filterLayout->addWidget( clearButton );

		QObject::connect( clearButton, &QToolButton::clicked, [](){
			if ( g_SoundBrowser.m_filterEntry != nullptr ) {
				g_SoundBrowser.m_filterEntry->clear();
			}
		} );
		QObject::connect( entry, &QLineEdit::textChanged, []( const QString& text ){
			g_SoundBrowser.setFilter( text.toLatin1().constData() );
			SoundBrowser_updateVisibleFiles();
		} );

		vbox->addWidget( filterBar );
	}
	{	// TreeView
		g_SoundBrowser.m_treeView = new TexBro_QTreeView;
		g_SoundBrowser.m_treeView->setHeaderHidden( true );
		g_SoundBrowser.m_treeView->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
		g_SoundBrowser.m_treeView->setUniformRowHeights( true );
		g_SoundBrowser.m_treeView->setFocusPolicy( Qt::FocusPolicy::ClickFocus );
		g_SoundBrowser.m_treeView->setExpandsOnDoubleClick( false );
		g_SoundBrowser.m_treeView->header()->setStretchLastSection( false );
		g_SoundBrowser.m_treeView->header()->setSectionResizeMode( QHeaderView::ResizeMode::ResizeToContents );

		QObject::connect( g_SoundBrowser.m_treeView, &QAbstractItemView::clicked, []( const QModelIndex& index ){
			SoundBrowser_selectFolder( index );
		} );

		SoundBrowser_constructTree();

		vbox->addWidget( g_SoundBrowser.m_treeView );
	}
	{	// gl_widget
		if ( disableOpenGL ) {
			g_SoundBrowser.m_gl_widget = nullptr;
			hbox->addWidget( glwidget_createDisabledPlaceholder( "OpenGL disabled (Sounds)", nullptr ) );
		}
		else {
			g_SoundBrowser.m_gl_widget = new SoundBrowserGLWidget( g_SoundBrowser );
			hbox->addWidget( g_SoundBrowser.m_gl_widget );
		}
	}
	{	// gl_widget scrollbar
		if ( !disableOpenGL ) {
			auto *scroll = g_SoundBrowser.m_gl_scroll = new QScrollBar;
			hbox->addWidget( scroll );

			QObject::connect( scroll, &QAbstractSlider::valueChanged, []( int value ){
				g_SoundBrowser.m_scrollAdjustment.value_changed( value );
			} );
		}
		else {
			g_SoundBrowser.m_gl_scroll = nullptr;
		}
	}

	g_guiSettings.addSplitter( splitter, "SoundBrowser/splitter", { 100, 500 } );

	return splitter;
}

void SoundBrowser_destroyWindow(){
	g_SoundBrowser.stopPlayback();
	g_SoundBrowser.m_gl_widget = nullptr;
}

void SoundBrowser_Construct(){
}

void SoundBrowser_Destroy(){
	g_SoundBrowser.stopPlayback();
}
