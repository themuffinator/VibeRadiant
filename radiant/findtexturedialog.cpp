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

//
// Find/Replace dialogs
//
// Leonardo Zide (leo@lokigames.com)
//

#include "findtexturedialog.h"

#include "debugging/debugging.h"

#include <QHBoxLayout>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include "gtkutil/lineedit.h"
#include <QLabel>
#include <QCheckBox>
#include <QEvent>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include "gtkutil/guisettings.h"
#include "commands.h"
#include "dialog.h"
#include "select.h"
#include "textureentry.h"
#include "shaderlib.h"



class FindTextureDialog : public Dialog
{
public:
	static void setReplaceStr( const char* name );
	static void setFindStr( const char* name );
	static bool isOpen();
	static void showFind();
	static void showReplace();
	static void showReplaceTextures();
	static void showReplaceEntities();
	static void showReplaceSounds();
	static void showReplaceObjects();
	typedef FreeCaller<void(), &FindTextureDialog::showReplace> ShowCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showFind> ShowFindCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showReplace> ShowReplaceCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showReplaceTextures> ShowReplaceTexturesCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showReplaceEntities> ShowReplaceEntitiesCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showReplaceSounds> ShowReplaceSoundsCaller;
	typedef FreeCaller<void(), &FindTextureDialog::showReplaceObjects> ShowReplaceObjectsCaller;
	static void updateTextures( const char* name );

	FindTextureDialog();
	~FindTextureDialog() = default;
	void BuildDialog() override;
	void apply( bool replace );
	void focusFind();
	void focusReplace();

	void constructWindow( QWidget* parent ){
		Create( parent );
	}
	void destroyWindow(){
		Destroy();
	}


	CopiedString m_strFind;
	CopiedString m_strReplace;
	CopiedString m_strIncludeFilter;
	CopiedString m_strExcludeFilter;
	CopiedString m_surfaceFlagsRequire;
	CopiedString m_surfaceFlagsExclude;
	CopiedString m_contentFlagsRequire;
	CopiedString m_contentFlagsExclude;
	int m_matchMode;
	int m_replaceMode;
	int m_scope;
	int m_shaderFilter;
	int m_usageFilter;
	bool m_caseSensitive;
	bool m_matchNameOnly;
	bool m_autoPrefix;
	bool m_visibleOnly;
	bool m_includeBrushes;
	bool m_includePatches;
	int m_minWidth;
	int m_maxWidth;
	int m_minHeight;
	int m_maxHeight;

	CopiedString m_entityFind;
	CopiedString m_entityReplace;
	CopiedString m_entityKeyFilter;
	CopiedString m_entityClassFilter;
	int m_entityMatchMode;
	int m_entityReplaceMode;
	int m_entityScope;
	bool m_entityCaseSensitive;
	bool m_entityVisibleOnly;
	bool m_entitySearchKeys;
	bool m_entitySearchValues;
	bool m_entityReplaceKeys;
	bool m_entityReplaceValues;
	bool m_entityIncludeWorldspawn;

	QTabWidget* m_tabs = nullptr;
	LineEdit* m_textureFindEntry = nullptr;
	LineEdit* m_textureReplaceEntry = nullptr;
	LineEdit* m_entityFindEntry = nullptr;
	LineEdit* m_entityReplaceEntry = nullptr;
	QPushButton* m_findButton = nullptr;
	QPushButton* m_replaceButton = nullptr;

	void updateReplaceButtonState();
	QLineEdit* activeFindEntry() const;
	QLineEdit* activeReplaceEntry() const;
	bool isTextureTabActive() const;
};

FindTextureDialog g_FindTextureDialog;
static bool g_bFindActive = true;

namespace
{

class FindActiveTracker : public QObject
{
	const bool m_findActive;
public:
	FindActiveTracker( bool findActive ) : m_findActive( findActive ){}
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::FocusIn ) {
			g_bFindActive = m_findActive;
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
};
FindActiveTracker s_find_focus_in( true );
FindActiveTracker s_replace_focus_in( false );

class : public QObject
{
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ) {
			auto *keyEvent = static_cast<QKeyEvent *>( event );
			if( keyEvent->key() == Qt::Key_Tab ){
				event->accept();
			}
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
}
s_pressedKeysFilter;

}


// =============================================================================
// FindTextureDialog class

FindTextureDialog::FindTextureDialog()
	: m_matchMode( static_cast<int>( TextureFindMatchMode::Exact ) ),
	m_replaceMode( static_cast<int>( TextureReplaceMode::ReplaceFull ) ),
	m_scope( static_cast<int>( TextureFindScope::All ) ),
	m_shaderFilter( static_cast<int>( TextureShaderFilter::Any ) ),
	m_usageFilter( static_cast<int>( TextureUsageFilter::Any ) ),
	m_caseSensitive( false ),
	m_matchNameOnly( false ),
	m_autoPrefix( true ),
	m_visibleOnly( true ),
	m_includeBrushes( true ),
	m_includePatches( true ),
	m_minWidth( 0 ),
	m_maxWidth( 0 ),
	m_minHeight( 0 ),
	m_maxHeight( 0 ),
	m_entityMatchMode( static_cast<int>( TextureFindMatchMode::Exact ) ),
	m_entityReplaceMode( static_cast<int>( TextureReplaceMode::ReplaceFull ) ),
	m_entityScope( static_cast<int>( EntityFindScope::All ) ),
	m_entityCaseSensitive( false ),
	m_entityVisibleOnly( true ),
	m_entitySearchKeys( false ),
	m_entitySearchValues( true ),
	m_entityReplaceKeys( false ),
	m_entityReplaceValues( true ),
	m_entityIncludeWorldspawn( false ){
}

void FindTextureDialog::BuildDialog(){
	GetWidget()->setWindowTitle( "Find / Replace" );

	GetWidget()->installEventFilter( &s_pressedKeysFilter );

	g_guiSettings.addWindow( GetWidget(), "FindReplace" );

	auto *root = new QVBoxLayout( GetWidget() );

	const auto addMatchModeItems = []( QComboBox* combo ){
		combo->addItem( "Exact" );
		combo->addItem( "Contains" );
		combo->addItem( "Starts with" );
		combo->addItem( "Ends with" );
		combo->addItem( "Wildcard (* ?)" );
#if defined( __cpp_exceptions )
		combo->addItem( "Regex" );
#endif
	};

	m_tabs = new QTabWidget;
	root->addWidget( m_tabs );

	{
		auto *tab = new QWidget;
		m_tabs->addTab( tab, "Textures" );
		auto *layout = new QVBoxLayout( tab );

		{
			auto *criteriaGroup = new QGroupBox( "Criteria" );
			auto *criteria = new QFormLayout( criteriaGroup );

			{
				m_textureFindEntry = new LineEdit;
				criteria->addRow( "Find:", m_textureFindEntry );
				m_textureFindEntry->setPlaceholderText( "Pattern (see match mode)" );
				AddDialogData( *m_textureFindEntry, m_strFind );
				m_textureFindEntry->installEventFilter( &s_find_focus_in );
				GlobalTextureEntryCompletion::instance().connect( m_textureFindEntry );
			}
			{
				m_textureReplaceEntry = new LineEdit;
				criteria->addRow( "Replace:", m_textureReplaceEntry );
				m_textureReplaceEntry->setPlaceholderText( "Empty = select matches (use $1..$9 for wildcard/regex)" );
				AddDialogData( *m_textureReplaceEntry, m_strReplace );
				m_textureReplaceEntry->installEventFilter( &s_replace_focus_in );
				GlobalTextureEntryCompletion::instance().connect( m_textureReplaceEntry );
				QObject::connect( m_textureReplaceEntry, &QLineEdit::textChanged, [this](){ updateReplaceButtonState(); } );
			}

			{
				auto *combo = new QComboBox;
				addMatchModeItems( combo );
				AddDialogData( *combo, m_matchMode );
				criteria->addRow( "Match mode:", combo );
			}
			{
				auto *combo = new QComboBox;
				combo->addItem( "Replace whole shader" );
				combo->addItem( "Replace matched text" );
				AddDialogData( *combo, m_replaceMode );
				criteria->addRow( "Replace mode:", combo );
			}
			{
				auto *combo = new QComboBox;
				combo->addItem( "All" );
				combo->addItem( "Selected objects" );
				combo->addItem( "Selected faces (component)" );
				AddDialogData( *combo, m_scope );
				criteria->addRow( "Scope:", combo );
			}
			{
				auto *targetWidget = new QWidget;
				auto *targetLayout = new QHBoxLayout( targetWidget );
				targetLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *brushCheck = new QCheckBox( "Brush faces" );
				auto *patchCheck = new QCheckBox( "Patches" );
				AddDialogData( *brushCheck, m_includeBrushes );
				AddDialogData( *patchCheck, m_includePatches );
				targetLayout->addWidget( brushCheck );
				targetLayout->addWidget( patchCheck );
				targetLayout->addStretch( 1 );
				criteria->addRow( "Targets:", targetWidget );
			}
			{
				auto *row = new QWidget;
				auto *rowLayout = new QHBoxLayout( row );
				rowLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *caseCheck = new QCheckBox( "Case sensitive" );
				auto *visibleCheck = new QCheckBox( "Visible only" );
				AddDialogData( *caseCheck, m_caseSensitive );
				AddDialogData( *visibleCheck, m_visibleOnly );
				rowLayout->addWidget( caseCheck );
				rowLayout->addWidget( visibleCheck );
				rowLayout->addStretch( 1 );
				criteria->addRow( "", row );
			}
			{
				auto *row = new QWidget;
				auto *rowLayout = new QHBoxLayout( row );
				rowLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *matchNameCheck = new QCheckBox( "Match name only" );
				auto *autoPrefixCheck = new QCheckBox( "Auto-prefix textures/" );
				AddDialogData( *matchNameCheck, m_matchNameOnly );
				AddDialogData( *autoPrefixCheck, m_autoPrefix );
				rowLayout->addWidget( matchNameCheck );
				rowLayout->addWidget( autoPrefixCheck );
				rowLayout->addStretch( 1 );
				criteria->addRow( "", row );
			}

			layout->addWidget( criteriaGroup );
		}

		{
			auto *filtersGroup = new QGroupBox( "Filters" );
			auto *filters = new QFormLayout( filtersGroup );

			{
				auto *combo = new QComboBox;
				combo->addItem( "Any" );
				combo->addItem( "Missing (default) only" );
				combo->addItem( "Real shaders only" );
				AddDialogData( *combo, m_shaderFilter );
				filters->addRow( "Shader type:", combo );
			}
			{
				auto *combo = new QComboBox;
				combo->addItem( "Any" );
				combo->addItem( "In use" );
				combo->addItem( "Not in use" );
				AddDialogData( *combo, m_usageFilter );
				filters->addRow( "Usage:", combo );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Wildcard filters, comma-separated (e.g. textures/common/*)" );
				AddDialogData( *entry, m_strIncludeFilter );
				filters->addRow( "Include:", entry );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Wildcard filters, comma-separated" );
				AddDialogData( *entry, m_strExcludeFilter );
				filters->addRow( "Exclude:", entry );
			}
			{
				auto *rangeWidget = new QWidget;
				auto *rangeLayout = new QHBoxLayout( rangeWidget );
				rangeLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *minSpin = new QSpinBox;
				auto *maxSpin = new QSpinBox;
				minSpin->setRange( 0, 65535 );
				maxSpin->setRange( 0, 65535 );
				minSpin->setSpecialValueText( "Any" );
				maxSpin->setSpecialValueText( "Any" );
				AddDialogData( *minSpin, m_minWidth );
				AddDialogData( *maxSpin, m_maxWidth );
				rangeLayout->addWidget( new QLabel( "Min" ) );
				rangeLayout->addWidget( minSpin );
				rangeLayout->addWidget( new QLabel( "Max" ) );
				rangeLayout->addWidget( maxSpin );
				filters->addRow( "Width (px):", rangeWidget );
			}
			{
				auto *rangeWidget = new QWidget;
				auto *rangeLayout = new QHBoxLayout( rangeWidget );
				rangeLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *minSpin = new QSpinBox;
				auto *maxSpin = new QSpinBox;
				minSpin->setRange( 0, 65535 );
				maxSpin->setRange( 0, 65535 );
				minSpin->setSpecialValueText( "Any" );
				maxSpin->setSpecialValueText( "Any" );
				AddDialogData( *minSpin, m_minHeight );
				AddDialogData( *maxSpin, m_maxHeight );
				rangeLayout->addWidget( new QLabel( "Min" ) );
				rangeLayout->addWidget( minSpin );
				rangeLayout->addWidget( new QLabel( "Max" ) );
				rangeLayout->addWidget( maxSpin );
				filters->addRow( "Height (px):", rangeWidget );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Hex/dec mask (blank = ignore)" );
				AddDialogData( *entry, m_surfaceFlagsRequire );
				filters->addRow( "Surface flags require:", entry );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Hex/dec mask (blank = ignore)" );
				AddDialogData( *entry, m_surfaceFlagsExclude );
				filters->addRow( "Surface flags exclude:", entry );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Hex/dec mask (blank = ignore)" );
				AddDialogData( *entry, m_contentFlagsRequire );
				filters->addRow( "Content flags require:", entry );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Hex/dec mask (blank = ignore)" );
				AddDialogData( *entry, m_contentFlagsExclude );
				filters->addRow( "Content flags exclude:", entry );
			}

			layout->addWidget( filtersGroup );
		}

		layout->addStretch( 1 );
	}

	{
		auto *tab = new QWidget;
		m_tabs->addTab( tab, "Entities" );
		auto *layout = new QVBoxLayout( tab );

		{
			auto *criteriaGroup = new QGroupBox( "Criteria" );
			auto *criteria = new QFormLayout( criteriaGroup );

			{
				m_entityFindEntry = new LineEdit;
				criteria->addRow( "Find:", m_entityFindEntry );
				m_entityFindEntry->setPlaceholderText( "Pattern (keys/values)" );
				AddDialogData( *m_entityFindEntry, m_entityFind );
			}
			{
				m_entityReplaceEntry = new LineEdit;
				criteria->addRow( "Replace:", m_entityReplaceEntry );
				m_entityReplaceEntry->setPlaceholderText( "Empty = select matches (use $1..$9 for wildcard/regex)" );
				AddDialogData( *m_entityReplaceEntry, m_entityReplace );
				QObject::connect( m_entityReplaceEntry, &QLineEdit::textChanged, [this](){ updateReplaceButtonState(); } );
			}
			{
				auto *combo = new QComboBox;
				addMatchModeItems( combo );
				AddDialogData( *combo, m_entityMatchMode );
				criteria->addRow( "Match mode:", combo );
			}
			{
				auto *combo = new QComboBox;
				combo->addItem( "Replace whole value" );
				combo->addItem( "Replace matched text" );
				AddDialogData( *combo, m_entityReplaceMode );
				criteria->addRow( "Replace mode:", combo );
			}
			{
				auto *combo = new QComboBox;
				combo->addItem( "All entities" );
				combo->addItem( "Selected entities" );
				AddDialogData( *combo, m_entityScope );
				criteria->addRow( "Scope:", combo );
			}
			QCheckBox* searchKeysCheck = nullptr;
			QCheckBox* searchValuesCheck = nullptr;
			QCheckBox* replaceKeysCheck = nullptr;
			QCheckBox* replaceValuesCheck = nullptr;
			{
				auto *row = new QWidget;
				auto *rowLayout = new QHBoxLayout( row );
				rowLayout->setContentsMargins( 0, 0, 0, 0 );
				searchKeysCheck = new QCheckBox( "Keys" );
				searchValuesCheck = new QCheckBox( "Values" );
				AddDialogData( *searchKeysCheck, m_entitySearchKeys );
				AddDialogData( *searchValuesCheck, m_entitySearchValues );
				rowLayout->addWidget( searchKeysCheck );
				rowLayout->addWidget( searchValuesCheck );
				rowLayout->addStretch( 1 );
				criteria->addRow( "Search in:", row );
			}
			{
				auto *row = new QWidget;
				auto *rowLayout = new QHBoxLayout( row );
				rowLayout->setContentsMargins( 0, 0, 0, 0 );
				replaceKeysCheck = new QCheckBox( "Keys" );
				replaceValuesCheck = new QCheckBox( "Values" );
				AddDialogData( *replaceKeysCheck, m_entityReplaceKeys );
				AddDialogData( *replaceValuesCheck, m_entityReplaceValues );
				rowLayout->addWidget( replaceKeysCheck );
				rowLayout->addWidget( replaceValuesCheck );
				rowLayout->addStretch( 1 );
				criteria->addRow( "Replace in:", row );
			}
			{
				const auto updateReplaceTargets = [replaceKeysCheck, replaceValuesCheck, searchKeysCheck, searchValuesCheck](){
					replaceKeysCheck->setEnabled( searchKeysCheck->isChecked() );
					replaceValuesCheck->setEnabled( searchValuesCheck->isChecked() );
					if ( !searchKeysCheck->isChecked() ) {
						replaceKeysCheck->setChecked( false );
					}
					if ( !searchValuesCheck->isChecked() ) {
						replaceValuesCheck->setChecked( false );
					}
				};
				QObject::connect( searchKeysCheck, &QCheckBox::toggled, updateReplaceTargets );
				QObject::connect( searchValuesCheck, &QCheckBox::toggled, updateReplaceTargets );
				updateReplaceTargets();
			}
			{
				auto *row = new QWidget;
				auto *rowLayout = new QHBoxLayout( row );
				rowLayout->setContentsMargins( 0, 0, 0, 0 );
				auto *caseCheck = new QCheckBox( "Case sensitive" );
				auto *visibleCheck = new QCheckBox( "Visible only" );
				AddDialogData( *caseCheck, m_entityCaseSensitive );
				AddDialogData( *visibleCheck, m_entityVisibleOnly );
				rowLayout->addWidget( caseCheck );
				rowLayout->addWidget( visibleCheck );
				rowLayout->addStretch( 1 );
				criteria->addRow( "", row );
			}
			{
				auto *check = new QCheckBox( "Include worldspawn" );
				AddDialogData( *check, m_entityIncludeWorldspawn );
				criteria->addRow( "", check );
			}

			layout->addWidget( criteriaGroup );
		}

		{
			auto *filtersGroup = new QGroupBox( "Filters" );
			auto *filters = new QFormLayout( filtersGroup );
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Wildcard filters, comma-separated (e.g. light*, trigger_*)" );
				AddDialogData( *entry, m_entityClassFilter );
				filters->addRow( "Classname:", entry );
			}
			{
				auto *entry = new LineEdit;
				entry->setPlaceholderText( "Wildcard filters, comma-separated (e.g. target*, model)" );
				AddDialogData( *entry, m_entityKeyFilter );
				filters->addRow( "Keys:", entry );
			}
			layout->addWidget( filtersGroup );
		}

		layout->addStretch( 1 );
	}

	{
		auto *buttons = new QDialogButtonBox( Qt::Orientation::Horizontal );
		root->addWidget( buttons );

		m_findButton = buttons->addButton( "Find", QDialogButtonBox::ButtonRole::ActionRole );
		m_replaceButton = buttons->addButton( "Replace", QDialogButtonBox::ButtonRole::ActionRole );
		auto *closeButton = buttons->addButton( QDialogButtonBox::StandardButton::Close );

		QObject::connect( m_findButton, &QPushButton::clicked, [this](){
			apply( false );
		} );
		QObject::connect( m_replaceButton, &QPushButton::clicked, [this](){
			apply( true );
		} );
		QObject::connect( closeButton, &QPushButton::clicked, [this](){
			HideDlg();
		} );
	}

	QObject::connect( m_tabs, &QTabWidget::currentChanged, [this]( int ){ updateReplaceButtonState(); } );
	updateReplaceButtonState();
}

void FindTextureDialog::apply( bool replace ){
	exportData();
	if ( isTextureTabActive() ) {
		TextureFindReplaceOptions options;
		options.find = m_strFind.c_str();
		options.replace = replace ? m_strReplace.c_str() : "";
		options.includeFilter = m_strIncludeFilter.c_str();
		options.excludeFilter = m_strExcludeFilter.c_str();
		options.surfaceFlagsRequire = m_surfaceFlagsRequire.c_str();
		options.surfaceFlagsExclude = m_surfaceFlagsExclude.c_str();
		options.contentFlagsRequire = m_contentFlagsRequire.c_str();
		options.contentFlagsExclude = m_contentFlagsExclude.c_str();
		options.matchMode = static_cast<TextureFindMatchMode>( m_matchMode );
		options.replaceMode = static_cast<TextureReplaceMode>( m_replaceMode );
		options.scope = static_cast<TextureFindScope>( m_scope );
		options.shaderFilter = static_cast<TextureShaderFilter>( m_shaderFilter );
		options.usageFilter = static_cast<TextureUsageFilter>( m_usageFilter );
		options.caseSensitive = m_caseSensitive;
		options.matchNameOnly = m_matchNameOnly;
		options.autoPrefix = m_autoPrefix;
		options.visibleOnly = m_visibleOnly;
		options.includeBrushes = m_includeBrushes;
		options.includePatches = m_includePatches;
		options.minWidth = m_minWidth;
		options.maxWidth = m_maxWidth;
		options.minHeight = m_minHeight;
		options.maxHeight = m_maxHeight;
		FindReplaceTextures( options );
	}
	else {
		EntityFindReplaceOptions options;
		options.find = m_entityFind.c_str();
		options.replace = replace ? m_entityReplace.c_str() : "";
		options.keyFilter = m_entityKeyFilter.c_str();
		options.classFilter = m_entityClassFilter.c_str();
		options.matchMode = static_cast<TextureFindMatchMode>( m_entityMatchMode );
		options.replaceMode = static_cast<TextureReplaceMode>( m_entityReplaceMode );
		options.scope = static_cast<EntityFindScope>( m_entityScope );
		options.caseSensitive = m_entityCaseSensitive;
		options.visibleOnly = m_entityVisibleOnly;
		options.searchKeys = m_entitySearchKeys;
		options.searchValues = m_entitySearchValues;
		options.replaceKeys = m_entityReplaceKeys;
		options.replaceValues = m_entityReplaceValues;
		options.includeWorldspawn = m_entityIncludeWorldspawn;
		FindReplaceEntities( options );
	}
}

void FindTextureDialog::focusFind(){
	if ( m_findButton != nullptr ) {
		m_findButton->setDefault( true );
	}
	if ( m_replaceButton != nullptr ) {
		m_replaceButton->setDefault( false );
	}
	if ( QLineEdit* entry = activeFindEntry() ) {
		QTimer::singleShot( 0, [entry](){
			entry->setFocus();
			entry->selectAll();
		} );
	}
}

void FindTextureDialog::focusReplace(){
	if ( m_replaceButton != nullptr ) {
		m_replaceButton->setDefault( true );
	}
	if ( m_findButton != nullptr ) {
		m_findButton->setDefault( false );
	}
	if ( QLineEdit* entry = activeReplaceEntry() ) {
		QTimer::singleShot( 0, [entry](){
			entry->setFocus();
			entry->selectAll();
		} );
	}
}

void FindTextureDialog::updateReplaceButtonState(){
	if ( m_replaceButton == nullptr ) {
		return;
	}
	QLineEdit* entry = activeReplaceEntry();
	const bool enabled = entry != nullptr && !entry->text().trimmed().isEmpty();
	m_replaceButton->setEnabled( enabled );
}

QLineEdit* FindTextureDialog::activeFindEntry() const {
	return isTextureTabActive() ? m_textureFindEntry : m_entityFindEntry;
}

QLineEdit* FindTextureDialog::activeReplaceEntry() const {
	return isTextureTabActive() ? m_textureReplaceEntry : m_entityReplaceEntry;
}

bool FindTextureDialog::isTextureTabActive() const {
	return m_tabs == nullptr || m_tabs->currentIndex() == 0;
}

void FindTextureDialog::updateTextures( const char* name ){
	if ( isOpen() && g_FindTextureDialog.isTextureTabActive() ) {
		const char* prefix = GlobalTexturePrefix_get();
		const char* trimmed = name;
		if ( shader_equal_prefix( name, prefix ) ) {
			trimmed = name + string_length( prefix );
		}
		if ( g_bFindActive ) {
			setFindStr( trimmed );
		}
		else
		{
			setReplaceStr( trimmed );
		}
	}
}

bool FindTextureDialog::isOpen(){
	return g_FindTextureDialog.GetWidget()->isVisible();
}

void FindTextureDialog::setFindStr( const char* name ){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_strFind = name;
	g_FindTextureDialog.importData();
}

void FindTextureDialog::setReplaceStr( const char* name ){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_strReplace = name;
	g_FindTextureDialog.importData();
}

void FindTextureDialog::showFind(){
	g_FindTextureDialog.ShowDlg();
	g_FindTextureDialog.focusFind();
}

void FindTextureDialog::showReplace(){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_entityKeyFilter = "";
	g_FindTextureDialog.ShowDlg();
	g_FindTextureDialog.importData();
	g_FindTextureDialog.focusReplace();
}

void FindTextureDialog::showReplaceTextures(){
	g_FindTextureDialog.ShowDlg();
	if ( g_FindTextureDialog.m_tabs != nullptr ) {
		g_FindTextureDialog.m_tabs->setCurrentIndex( 0 );
	}
	g_FindTextureDialog.focusReplace();
}

void FindTextureDialog::showReplaceEntities(){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_entityKeyFilter = "";
	g_FindTextureDialog.ShowDlg();
	if ( g_FindTextureDialog.m_tabs != nullptr ) {
		g_FindTextureDialog.m_tabs->setCurrentIndex( 1 );
	}
	g_FindTextureDialog.importData();
	g_FindTextureDialog.focusReplace();
}

void FindTextureDialog::showReplaceSounds(){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_entityKeyFilter = "noise,sound*,s_*";
	g_FindTextureDialog.ShowDlg();
	if ( g_FindTextureDialog.m_tabs != nullptr ) {
		g_FindTextureDialog.m_tabs->setCurrentIndex( 1 );
	}
	g_FindTextureDialog.importData();
	g_FindTextureDialog.focusReplace();
}

void FindTextureDialog::showReplaceObjects(){
	g_FindTextureDialog.exportData();
	g_FindTextureDialog.m_entityKeyFilter = "model*";
	g_FindTextureDialog.ShowDlg();
	if ( g_FindTextureDialog.m_tabs != nullptr ) {
		g_FindTextureDialog.m_tabs->setCurrentIndex( 1 );
	}
	g_FindTextureDialog.importData();
	g_FindTextureDialog.focusReplace();
}


void FindTextureDialog_constructWindow( QWidget* main_window ){
	g_FindTextureDialog.constructWindow( main_window );
}

void FindTextureDialog_destroyWindow(){
	g_FindTextureDialog.destroyWindow();
}

bool FindTextureDialog_isOpen(){
	return g_FindTextureDialog.isOpen();
}

void FindTextureDialog_selectTexture( const char* name ){
	g_FindTextureDialog.updateTextures( name );
}

#include "preferencesystem.h"

void FindTextureDialog_Construct(){
	GlobalCommands_insert( "Find", FindTextureDialog::ShowFindCaller(), QKeySequence( "Ctrl+F" ) );
	GlobalCommands_insert( "FindReplace", FindTextureDialog::ShowReplaceCaller(), QKeySequence( "Ctrl+H" ) );
	GlobalCommands_insert( "FindReplaceTextures", FindTextureDialog::ShowReplaceTexturesCaller() );
	GlobalCommands_insert( "FindReplaceEntities", FindTextureDialog::ShowReplaceEntitiesCaller() );
	GlobalCommands_insert( "FindReplaceSounds", FindTextureDialog::ShowReplaceSoundsCaller() );
	GlobalCommands_insert( "FindReplaceObjects", FindTextureDialog::ShowReplaceObjectsCaller() );
}

void FindTextureDialog_Destroy(){
}
