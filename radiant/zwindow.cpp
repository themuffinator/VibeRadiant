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

#include "zwindow.h"

#include "debugging/debugging.h"

#include "igl.h"
#include "grid.h"
#include "mainframe.h"
#include "camwindow.h"
#include "brush.h"
#include "xywindow.h"
#include "selection.h"
#include "texturelib.h"
#include "windowobservers.h"
#include "iselection.h"

#include "gtkutil/fbo.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/widget.h"

#include "stream/stringstream.h"

#include <QOpenGLWidget>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace
{
const float k_minScale = 0.125f;
const float k_maxScale = 4.0f;
const int k_minZBarWidth = 10;

bool isCameraMoveModifier( Qt::KeyboardModifiers modifiers ){
	return ( modifiers & Qt::KeyboardModifier::ControlModifier )
	    && !( modifiers & ( Qt::KeyboardModifier::ShiftModifier | Qt::KeyboardModifier::AltModifier ) );
}

bool isCameraMoveButtons( Qt::MouseButtons buttons ){
	return buttons == Qt::MouseButton::LeftButton
	    || buttons == Qt::MouseButton::MiddleButton;
}
}

class ZGLWidget : public QOpenGLWidget
{
	ZWnd& m_zwnd;
	FBO *m_fbo{};
	qreal m_deviceScale{};
	bool m_selectionActive{};
public:
	ZGLWidget( ZWnd& zwnd ) : QOpenGLWidget(), m_zwnd( zwnd ){
		setMouseTracking( true );
		setMinimumSize( k_minZBarWidth, k_minZBarWidth );
	}

	~ZGLWidget() override {
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
		m_deviceScale = devicePixelRatioF();
		const int minSize = float_to_integer( k_minZBarWidth * m_deviceScale );
		m_zwnd.m_nWidth = std::max( float_to_integer( w * m_deviceScale ), minSize );
		m_zwnd.m_nHeight = std::max( float_to_integer( h * m_deviceScale ), minSize );
		m_zwnd.updateProjection();
		if ( m_zwnd.m_window_observer != nullptr ) {
			m_zwnd.m_window_observer->onSizeChanged( m_zwnd.m_nWidth, m_zwnd.m_nHeight );
		}
		m_zwnd.m_drawRequired = true;

		delete m_fbo;
		m_fbo = nullptr;
		if ( m_zwnd.m_nWidth <= 0 || m_zwnd.m_nHeight <= 0 ) {
			return;
		}
		m_fbo = new FBO( m_zwnd.m_nWidth, m_zwnd.m_nHeight, false, XYWnd_getMSAA() );
	}
	void paintGL() override
	{
		if ( m_fbo == nullptr ) {
			return;
		}
		if( m_fbo->m_samples != XYWnd_getMSAA() ){
			delete m_fbo;
			m_fbo = new FBO( m_zwnd.m_nWidth, m_zwnd.m_nHeight, false, XYWnd_getMSAA() );
		}

		if ( Map_Valid( g_map ) && ScreenUpdates_Enabled() && m_fbo->bind() ) {
			if( m_zwnd.m_drawRequired ){
				m_zwnd.m_drawRequired = false;
				m_zwnd.Z_Draw();
			}
			m_fbo->blit();
			m_fbo->release();
			GlobalOpenGL_debugAssertNoErrors();
		}
	}

	void mousePressEvent( QMouseEvent *event ) override {
		setFocus();
		m_zwnd.syncOriginXY();
		m_zwnd.updateModelview();

#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
		const QPointF pos = event->position();
#else
		const QPointF pos = event->localPos();
#endif
		const int y = float_to_integer( pos.y() * m_deviceScale );

		if ( event->button() == Qt::MouseButton::RightButton ) {
			m_zwnd.beginOriginDrag();
			m_zwnd.endSelectionDrag();
			m_selectionActive = false;
			return;
		}

		const float snappedZ = float_snapped( m_zwnd.screenToWorldZ( y ), GetGridSize() );
		if ( isCameraMoveModifier( event->modifiers() )
		     && ( event->button() == Qt::MouseButton::LeftButton
		          || event->button() == Qt::MouseButton::MiddleButton ) ) {
			Vector3 origin = Camera_getOrigin( *g_pParentWnd->GetCamWnd() );
			origin[2] = snappedZ;
			Camera_setOrigin( *g_pParentWnd->GetCamWnd(), origin );
			m_zwnd.endSelectionDrag();
			m_selectionActive = false;
			return;
		}

		if ( m_zwnd.m_window_observer != nullptr ) {
			m_zwnd.beginSelectionDrag();
			m_zwnd.m_window_observer->onMouseDown(
				WindowVector( m_zwnd.m_nWidth / 2.0f, y ),
				button_for_button( event->button() ),
				modifiers_for_state( event->modifiers() ) );
			m_selectionActive = true;
		}
	}
	void mouseMoveEvent( QMouseEvent *event ) override {
		m_zwnd.syncOriginXY();
		m_zwnd.updateModelview();
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
		const QPointF pos = event->position();
#else
		const QPointF pos = event->localPos();
#endif
		const int y = float_to_integer( pos.y() * m_deviceScale );
		const float snappedZ = float_snapped( m_zwnd.screenToWorldZ( y ), GetGridSize() );

		{
			const auto status = StringStream<64>( "z:: ", FloatFormat( snappedZ, 6, 1 ) );
			g_pParentWnd->SetStatusText( c_status_position, status );
		}

		if ( m_zwnd.m_originDragging || ( event->buttons() & Qt::MouseButton::RightButton ) ) {
			return;
		}

		if ( isCameraMoveModifier( event->modifiers() )
		     && isCameraMoveButtons( event->buttons() ) ) {
			Vector3 origin = Camera_getOrigin( *g_pParentWnd->GetCamWnd() );
			origin[2] = snappedZ;
			Camera_setOrigin( *g_pParentWnd->GetCamWnd(), origin );
			return;
		}

		if ( m_zwnd.m_window_observer != nullptr ) {
			m_zwnd.m_window_observer->onMouseMotion(
				WindowVector( m_zwnd.m_nWidth / 2.0f, y ),
				modifiers_for_state( event->modifiers() ) );
		}
	}
	void mouseReleaseEvent( QMouseEvent *event ) override {
		m_zwnd.syncOriginXY();
		m_zwnd.updateModelview();

		if ( event->button() == Qt::MouseButton::RightButton ) {
			m_zwnd.endOriginDrag();
			m_zwnd.endSelectionDrag();
			m_selectionActive = false;
			return;
		}

		if ( isCameraMoveModifier( event->modifiers() )
		     && ( event->button() == Qt::MouseButton::LeftButton
		          || event->button() == Qt::MouseButton::MiddleButton ) ) {
			m_zwnd.endSelectionDrag();
			m_selectionActive = false;
			return;
		}

		if ( m_selectionActive && m_zwnd.m_window_observer != nullptr ) {
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
			const QPointF pos = event->position();
#else
			const QPointF pos = event->localPos();
#endif
			const int y = float_to_integer( pos.y() * m_deviceScale );
			m_zwnd.m_window_observer->onMouseUp(
				WindowVector( m_zwnd.m_nWidth / 2.0f, y ),
				button_for_button( event->button() ),
				modifiers_for_state( event->modifiers() ) );
		}

		m_zwnd.endSelectionDrag();
		m_selectionActive = false;
	}
	void wheelEvent( QWheelEvent *event ) override {
		setFocus();
		if ( event->angleDelta().y() > 0 ) {
			m_zwnd.ZoomIn();
		}
		else if ( event->angleDelta().y() < 0 ) {
			m_zwnd.ZoomOut();
		}
	}
};

ZWnd::ZWnd() :
	m_gl_widget( OpenGLWidgetsDisabled()
		? glwidget_createDisabledPlaceholder( "OpenGL disabled (Z view)", nullptr )
		: static_cast<QWidget*>( new ZGLWidget( *this ) ) ),
	m_deferredDraw( WidgetQueueDrawCaller( *m_gl_widget ) ),
	m_window_observer( NewWindowObserver() )
{
	m_drawRequired = true;

	GlobalWindowObservers_add( m_window_observer );
	GlobalWindowObservers_connectWidget( m_gl_widget );
	m_window_observer->setView( m_view );

	Map_addValidCallback( g_map, DeferredDrawOnMapValidChangedCaller( m_deferredDraw ) );
	AddSceneChangeCallback( MemberCaller<ZWnd, void(), &ZWnd::queueDraw>( *this ) );
	AddCameraMovedCallback( MemberCaller<ZWnd, void(), &ZWnd::queueDraw>( *this ) );

	updateProjection();
	updateModelview();
}

ZWnd::~ZWnd(){
	endOriginDrag();
	if ( m_window_observer != nullptr ) {
		m_window_observer->release();
	}
}

void ZWnd::queueDraw(){
	m_drawRequired = true;
	m_deferredDraw.draw();
}

void ZWnd::setScale( float scale ){
	const float clamped = std::min( k_maxScale, std::max( k_minScale, scale ) );
	if ( m_scale != clamped ) {
		m_scale = clamped;
		updateProjection();
		updateModelview();
	}
	queueDraw();
}

void ZWnd::ZoomIn(){
	setScale( m_scale * 5.0f / 4.0f );
}

void ZWnd::ZoomOut(){
	setScale( m_scale * 4.0f / 5.0f );
}

void ZWnd::SetOriginZ( float z ){
	m_origin[2] = std::min( g_MaxWorldCoord, std::max( g_MinWorldCoord, z ) );
	updateModelview();
	queueDraw();
}

void ZWnd::syncOriginXY(){
	if ( g_pParentWnd == nullptr ) {
		return;
	}

	XYWnd* xywnd = g_pParentWnd->GetXYWnd();
	if ( xywnd == nullptr ) {
		xywnd = g_pParentWnd->ActiveXY();
	}
	if ( xywnd == nullptr ) {
		return;
	}

	const Vector3& origin = xywnd->GetOrigin();
	m_origin[0] = origin[0];
	m_origin[1] = origin[1];
}

void ZWnd::updateProjection(){
	m_projection[0] = 1.0f / ( m_nWidth / 2 );
	m_projection[5] = 1.0f / ( m_nHeight / 2 );
	m_projection[10] = 1.0f / ( g_MaxWorldCoord * m_scale );

	m_projection[12] = 0;
	m_projection[13] = 0;
	m_projection[14] = -1;

	m_projection[1] = m_projection[2] = m_projection[3] =
	m_projection[4] = m_projection[6] = m_projection[7] =
	m_projection[8] = m_projection[9] = m_projection[11] = 0;

	m_projection[15] = 1;

	m_view.Construct( m_projection, m_modelview, m_nWidth, m_nHeight );
}

void ZWnd::updateModelview(){
	const float originX = m_selectionDragging ? m_selectionOriginX : m_origin[0];

	m_modelview[12] = -originX * m_scale;
	m_modelview[13] = -m_origin[2] * m_scale;
	m_modelview[14] = g_MaxWorldCoord * m_scale;

	m_modelview[0] = m_scale;
	m_modelview[1] = 0;
	m_modelview[2] = 0;

	m_modelview[4] = 0;
	m_modelview[5] = 0;
	m_modelview[6] = m_scale;

	m_modelview[8] = 0;
	m_modelview[9] = m_scale;
	m_modelview[10] = 0;

	m_modelview[3] = m_modelview[7] = m_modelview[11] = 0;
	m_modelview[15] = 1;

	m_view.Construct( m_projection, m_modelview, m_nWidth, m_nHeight );
}

void ZWnd::beginSelectionDrag(){
	if ( m_selectionDragging ) {
		return;
	}

	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		m_selectionOriginX = GlobalSelectionSystem().getBoundsSelected().origin[0];
	}
	else {
		m_selectionOriginX = m_origin[0];
	}

	m_selectionDragging = true;
	updateModelview();
}

void ZWnd::endSelectionDrag(){
	if ( !m_selectionDragging ) {
		return;
	}

	m_selectionDragging = false;
	updateModelview();
}

float ZWnd::screenToWorldZ( int y ) const {
	return m_origin[2] + ( ( m_nHeight - 1 - y ) - ( m_nHeight / 2.0f ) ) / m_scale;
}

void ZWnd::drawGrid( float w, float h ){
	const float gridSize = GetGridSize();
	float zb = m_origin[2] - h;
	if ( zb < g_region_mins[2] ) {
		zb = g_region_mins[2];
	}
	zb = 64.0f * std::floor( zb / 64.0f );

	float ze = m_origin[2] + h;
	if ( ze > g_region_maxs[2] ) {
		ze = g_region_maxs[2];
	}
	ze = 64.0f * std::ceil( ze / 64.0f );

	if ( XYWnd_showGrid() ) {
		gl().glColor3fv( vector3_to_array( g_xywindow_globals.color_gridmajor ) );
		if ( gridSize < 128.0f ) {
			gl().glBegin( GL_LINES );
			gl().glVertex2f( 0, zb );
			gl().glVertex2f( 0, ze );
			for ( float zz = zb ; zz < ze ; zz += 64.0f )
			{
				gl().glVertex2f( -w, zz );
				gl().glVertex2f( w, zz );
			}
			gl().glEnd();
		}
		else
		{
			gl().glBegin( GL_LINES );
			gl().glVertex2f( 0, zb );
			gl().glVertex2f( 0, ze );
			for ( float zz = zb ; zz < ze ; zz += 64.0f )
			{
				if ( ( static_cast<int>( zz ) & ( static_cast<int>( gridSize ) - 1 ) ) != 0 ) {
					continue;
				}
				gl().glVertex2f( -w, zz );
				gl().glVertex2f( w, zz );
			}
			gl().glEnd();
		}
	}

	if ( XYWnd_showGrid() && gridSize * m_scale >= 4.0f
		&& g_xywindow_globals.color_gridminor != g_xywindow_globals.color_gridback ) {
		gl().glColor3fv( vector3_to_array( g_xywindow_globals.color_gridminor ) );
		gl().glBegin( GL_LINES );
		for ( float zz = zb ; zz < ze ; zz += gridSize )
		{
			if ( ( static_cast<int>( zz ) & 63 ) == 0 ) {
				continue;
			}
			gl().glVertex2f( -w, zz );
			gl().glVertex2f( w, zz );
		}
		gl().glEnd();
	}

	if ( XYWnd_showCoordinates() ) {
		gl().glColor3fv( vector3_to_array( g_xywindow_globals.color_gridtext ) );
		const float step = gridSize > 64.0f ? gridSize : 64.0f;
		float zc = m_origin[2] - h;
		if ( zc < g_region_mins[2] ) {
			zc = g_region_mins[2];
		}
		zc = step * std::floor( zc / step );

		StringOutputStream text( 32 );
		for ( float zz = zc ; zz < ze ; zz += step )
		{
			gl().glRasterPos2f( -w + ( 1.0f / m_scale ), zz );
			text( static_cast<int>( zz ) );
			GlobalOpenGL().drawString( text.c_str() );
		}
	}
}

void ZWnd::drawBrushes( float xcam ){
	Scene_forEachVisibleBrush( GlobalSceneGraph(), [this, xcam]( BrushInstance& brushInstance ){
		const AABB& aabb = brushInstance.worldAABB();
		const Vector3 mins = aabb.origin - aabb.extents;
		const Vector3 maxs = aabb.origin + aabb.extents;

		const bool intersects = !( mins[0] >= m_origin[0]
			|| maxs[0] <= m_origin[0]
			|| mins[1] >= m_origin[1]
			|| maxs[1] <= m_origin[1] );

		if ( intersects ) {
			Colour3 color = g_xywindow_globals.color_brushes;
			if ( const Face* face = brushInstance.getBrush().back() ) {
				if ( Shader* shader = face->getShader().state() ) {
					color = shader->getTexture().color;
				}
			}
			gl().glColor3fv( vector3_to_array( color ) );
			gl().glBegin( GL_QUADS );
			gl().glVertex2f( -xcam, mins[2] );
			gl().glVertex2f( xcam, mins[2] );
			gl().glVertex2f( xcam, maxs[2] );
			gl().glVertex2f( -xcam, maxs[2] );
			gl().glEnd();
		}

		if ( brushInstance.isSelected() ) {
			gl().glColor3fv( vector3_to_array( g_xywindow_globals.color_selbrushes ) );
			gl().glBegin( GL_LINE_LOOP );
			gl().glVertex2f( -xcam, mins[2] );
			gl().glVertex2f( xcam, mins[2] );
			gl().glVertex2f( xcam, maxs[2] );
			gl().glVertex2f( -xcam, maxs[2] );
			gl().glEnd();
		}
	} );
}

void ZWnd::drawCameraIcon() const {
	const float xcam = m_nWidth / 4.0f / m_scale;
	const float gizmo = 8.0f / m_scale;
	const float height = 48.0f / m_scale;
	const float y = Camera_getOrigin( *g_pParentWnd->GetCamWnd() )[2];

	gl().glColor3fv( vector3_to_array( g_xywindow_globals.color_camera ) );
	gl().glBegin( GL_LINE_STRIP );
	gl().glVertex3f( -xcam, y, 0 );
	gl().glVertex3f( 0, y + gizmo, 0 );
	gl().glVertex3f( xcam, y, 0 );
	gl().glVertex3f( 0, y - gizmo, 0 );
	gl().glVertex3f( -xcam, y, 0 );
	gl().glVertex3f( xcam, y, 0 );
	gl().glVertex3f( xcam, y - height, 0 );
	gl().glVertex3f( -xcam, y - height, 0 );
	gl().glVertex3f( -xcam, y, 0 );
	gl().glEnd();
}

void ZWnd::Z_Draw(){
	syncOriginXY();
	updateModelview();

	gl().glViewport( 0, 0, m_nWidth, m_nHeight );
	gl().glClearColor( g_xywindow_globals.color_gridback[0],
	                   g_xywindow_globals.color_gridback[1],
	                   g_xywindow_globals.color_gridback[2], 0 );
	gl().glClear( GL_COLOR_BUFFER_BIT );

	gl().glMatrixMode( GL_PROJECTION );
	gl().glLoadIdentity();
	const float w = m_nWidth / 2.0f / m_scale;
	const float h = m_nHeight / 2.0f / m_scale;
	gl().glOrtho( -w, w, m_origin[2] - h, m_origin[2] + h, -8, 8 );

	gl().glMatrixMode( GL_MODELVIEW );
	gl().glLoadIdentity();

	gl().glDisable( GL_LINE_STIPPLE );
	gl().glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	gl().glDisableClientState( GL_NORMAL_ARRAY );
	gl().glDisableClientState( GL_COLOR_ARRAY );
	gl().glDisable( GL_TEXTURE_2D );
	gl().glDisable( GL_LIGHTING );
	gl().glDisable( GL_COLOR_MATERIAL );
	gl().glDisable( GL_DEPTH_TEST );
	gl().glDisable( GL_BLEND );

	drawGrid( w, h );
	drawBrushes( w * 2.0f / 3.0f );
	drawCameraIcon();

	GlobalOpenGL_debugAssertNoErrors();
}

void ZWnd::beginOriginDrag(){
	if ( m_originDragging ) {
		return;
	}
	m_originDragging = true;
	m_originDrag.freeze_pointer( m_gl_widget,
		[this]( int dx, int dy, const QMouseEvent *motion ){
			(void)dx;
			(void)motion;
			if ( dy != 0 ) {
				SetOriginZ( OriginZ() + dy / m_scale );
			}
		},
		[this](){ endOriginDrag(); }
	);
}

void ZWnd::endOriginDrag(){
	if ( !m_originDragging ) {
		return;
	}
	m_originDragging = false;
	m_originDrag.unfreeze_pointer( false );
}
