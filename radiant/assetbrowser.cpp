#include "assetbrowser.h"

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "assetbrowserprefs.h"
#include "eclasslib.h"
#include "entity.h"
#include "texwindow.h"
#include "entitybrowser.h"
#include "soundbrowser.h"
#include "modelwindow.h"
#include "brushnode.h"
#include "commands.h"
#include "ieclass.h"
#include "ientity.h"
#include "ifilesystem.h"
#include "ishaders.h"
#include "iscenegraph.h"
#include "map.h"
#include "os/path.h"
#include "patch.h"
#include "plugin.h"
#include "qerplugin.h"
#include "select.h"
#include "signal/isignal.h"
#include "string/string.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "traverselib.h"
#include "gtkmisc.h"
#include "generic/callback.h"
#include "gtkutil/i18n.h"
#include "gtkutil/image.h"
#include "gtkutil/toolbar.h"

static QTabWidget* g_assetBrowserTabs = nullptr;
static int g_assetBrowserEntitiesTab = -1;
static int g_assetBrowserModelsTab = -1;
static int g_assetBrowserSoundsTab = -1;
static QWidget* g_assetBrowserGlobalsTab = nullptr;
static QWidget* g_assetBrowserSurfacesTab = nullptr;
static QWidget* g_assetBrowserAITab = nullptr;
static QWidget* g_assetBrowserScriptTab = nullptr;

namespace {
constexpr bool kAssetBrowserEnabled = true;
constexpr int kAssetFilterRebuildDelayMilliseconds = 150;
Vector3 g_assetBrowserDefaultAngles( 0.0f, 40.0f, -60.0f );
CopiedString g_assetSurfacesFilter;
bool g_assetSurfacesFilterGlobal = false;
bool g_assetSurfacesFilterUsed = false;
CopiedString g_assetAIFilter;
bool g_assetAIFilterGlobal = false;
bool g_assetAIFilterUsed = false;
CopiedString g_assetScriptFilter;
bool g_assetScriptFilterGlobal = false;
bool g_assetScriptFilterUsed = false;
std::size_t g_assetGlobalsSceneRevision = 1;

void AssetBrowser_markGlobalsSceneChanged(){
	++g_assetGlobalsSceneRevision;
}
using AssetBrowserSceneChangedCaller = BindFirstOpaque<detail::FreeCallerWrapper<void()>>;

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

struct StringLessNoCaseLocal
{
	bool operator()( const CopiedString& a, const CopiedString& b ) const {
		return string_less_nocase( a.c_str(), b.c_str() );
	}
};

bool AssetBrowser_isIdTech4Game(){
	return string_equal( GlobalRadiant().getRequiredGameDescriptionKeyValue( "brushtypes" ), "doom3" );
}

bool AssetBrowser_isIdTech2Game(){
	return string_equal( GlobalRadiant().getRequiredGameDescriptionKeyValue( "brushtypes" ), "quake2" );
}

bool AssetBrowser_isQuake2RereleaseGame(){
	if ( !AssetBrowser_isIdTech2Game() ) {
		return false;
	}
	const char* name = GlobalRadiant().getRequiredGameDescriptionKeyValue( "name" );
	const char* unknown = GlobalRadiant().getGameDescriptionKeyValue( "unknowngamename" );
	if ( name == nullptr ) {
		name = "";
	}
	if ( unknown == nullptr ) {
		unknown = "";
	}
	return string_contains_nocase( name, "rerelease" )
	       || string_contains_nocase( unknown, "rerelease" );
}

const char* AssetBrowser_worldspawnExpectedAudioKey(){
	return AssetBrowser_isIdTech2Game() && !AssetBrowser_isQuake2RereleaseGame()
	       ? "sounds"
	       : "music";
}

using StringSetNoCaseLocal = std::set<CopiedString, StringLessNoCaseLocal>;

struct AssetSurfaceUsage
{
	int brushFaces = 0;
	int patches = 0;
	int total() const {
		return brushFaces + patches;
	}
};

using AssetSurfaceUsageMap = std::map<CopiedString, AssetSurfaceUsage, StringLessNoCaseLocal>;

class AssetSurfaceUsageWalker : public scene::Graph::Walker
{
	AssetSurfaceUsageMap& m_usage;
public:
	explicit AssetSurfaceUsageWalker( AssetSurfaceUsageMap& usage ) : m_usage( usage ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		scene::Node& node = path.top();
		if ( Brush* brush = Node_getBrush( node ) ) {
			Brush_forEachFace( *brush, [this]( Face& face ){
				++m_usage[face.GetShader()].brushFaces;
			} );
		}
		if ( Patch* patch = Node_getPatch( node ) ) {
			++m_usage[patch->GetShader()].patches;
		}
		return true;
	}
};

AssetSurfaceUsageMap AssetBrowser_collectSurfaceUsage(){
	AssetSurfaceUsageMap usage;
	if ( Map_Valid( g_map ) ) {
		GlobalSceneGraph().traverse( AssetSurfaceUsageWalker( usage ) );
	}
	return usage;
}

class AssetSurfaceShaderCollector
{
	StringSetNoCaseLocal& m_allShaders;
public:
	explicit AssetSurfaceShaderCollector( StringSetNoCaseLocal& allShaders ) : m_allShaders( allShaders ){
	}
	void collect( const char* shader ){
		if ( string_equal_prefix_nocase( shader, "textures/" ) ) {
			m_allShaders.emplace( shader );
		}
	}
};
typedef MemberCaller<AssetSurfaceShaderCollector, void( const char* ), &AssetSurfaceShaderCollector::collect> AssetSurfaceShaderCollectorCaller;

StringSetNoCaseLocal AssetBrowser_collectAllSurfaceShaders(){
	StringSetNoCaseLocal allShaders;
	AssetSurfaceShaderCollector collector( allShaders );
	GlobalShaderSystem().foreachShaderName( AssetSurfaceShaderCollectorCaller( collector ) );
	return allShaders;
}

class AssetWorldspawnFinder : public scene::Graph::Walker
{
	mutable Entity* m_worldspawn = nullptr;
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		if ( m_worldspawn != nullptr ) {
			return false;
		}
		if ( Entity* entity = Node_getEntity( path.top() ) ) {
			if ( string_equal_nocase( entity->getClassName(), "worldspawn" ) ) {
				m_worldspawn = entity;
				return false;
			}
		}
		return true;
	}
	Entity* result() const {
		return m_worldspawn;
	}
};

Entity* AssetBrowser_findWorldspawnEntity(){
	if ( !Map_Valid( g_map ) ) {
		return nullptr;
	}
	AssetWorldspawnFinder finder;
	GlobalSceneGraph().traverse( finder );
	return finder.result();
}

void AssetBrowser_setWorldspawnKey( const char* key, const char* value ){
	if ( Entity* worldspawn = AssetBrowser_findWorldspawnEntity() ) {
		UndoableCommand undo( StringStream<128>( "setWorldspawnKey ", key ) );
		worldspawn->setKeyValue( key, value );
	}
}

void AssetBrowser_addPointEntities( const char* classname, int count, int startIndex = 0 ){
	for ( int i = 0; i < count; ++i ) {
		const float offset = static_cast<float>( ( startIndex + i ) * 64 );
		Entity_createFromSelection( classname, Vector3( offset, 0, 0 ) );
	}
}

bool AssetBrowser_isAIClassStrict( const char* classname ){
	return string_equal_prefix_nocase( classname, "ai_" )
	       || string_equal_prefix_nocase( classname, "monster_" )
	       || string_equal_prefix_nocase( classname, "npc_" )
	       || string_equal_prefix_nocase( classname, "atdm:ai" );
}

bool AssetBrowser_isAIClassBroad( const char* classname ){
	return AssetBrowser_isAIClassStrict( classname )
	       || string_contains_nocase( classname, "ai" )
	       || string_contains_nocase( classname, "monster" )
	       || string_contains_nocase( classname, "npc" )
	       || string_contains_nocase( classname, "bot" );
}

StringSetNoCaseLocal AssetBrowser_collectAIClasses( bool globalScope ){
	StringSetNoCaseLocal classes;
	class Visitor : public EntityClassVisitor
	{
		StringSetNoCaseLocal& m_classes;
		bool m_globalScope;
	public:
		Visitor( StringSetNoCaseLocal& classes, bool globalScope )
			: m_classes( classes ), m_globalScope( globalScope ){
		}
		void visit( EntityClass* eclass ) override {
			const char* name = eclass->name();
			if ( m_globalScope ? AssetBrowser_isAIClassBroad( name ) : AssetBrowser_isAIClassStrict( name ) ) {
				m_classes.emplace( name );
			}
		}
	} visitor( classes, globalScope );
	GlobalEntityClassManager().forEach( visitor );
	return classes;
}

class AssetUsedAIClassWalker : public scene::Graph::Walker
{
	StringSetNoCaseLocal& m_used;
	bool m_globalScope;
public:
	AssetUsedAIClassWalker( StringSetNoCaseLocal& used, bool globalScope ) : m_used( used ), m_globalScope( globalScope ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		if ( Entity* entity = Node_getEntity( path.top() ) ) {
			const char* classname = entity->getClassName();
			if ( m_globalScope ? AssetBrowser_isAIClassBroad( classname ) : AssetBrowser_isAIClassStrict( classname ) ) {
				m_used.emplace( classname );
			}
		}
		return true;
	}
};

StringSetNoCaseLocal AssetBrowser_collectUsedAIClasses( bool globalScope ){
	StringSetNoCaseLocal used;
	if ( Map_Valid( g_map ) ) {
		GlobalSceneGraph().traverse( AssetUsedAIClassWalker( used, globalScope ) );
	}
	return used;
}

class AssetScriptFileCollector
{
	StringSetNoCaseLocal& m_scripts;
public:
	explicit AssetScriptFileCollector( StringSetNoCaseLocal& scripts ) : m_scripts( scripts ){
	}
	void collect( const char* name ){
		const auto cleaned = StringStream<256>( PathCleaned( name ) );
		m_scripts.emplace( cleaned.c_str() );
	}
};
typedef MemberCaller<AssetScriptFileCollector, void( const char* ), &AssetScriptFileCollector::collect> AssetScriptFileCollectorCaller;

StringSetNoCaseLocal AssetBrowser_collectScriptFiles( bool globalScope ){
	StringSetNoCaseLocal scripts;
	AssetScriptFileCollector collector( scripts );
	GlobalFileSystem().forEachFile( "script/", "script", AssetScriptFileCollectorCaller( collector ), globalScope ? 99 : 1 );
	return scripts;
}

bool AssetBrowser_normalizeScriptReference( const char* value, CopiedString& outRelativePath ){
	if ( string_empty( value ) ) {
		return false;
	}
	const auto cleaned = StringStream<256>( PathCleaned( value ) );
	const char* relative = cleaned.c_str();
	if ( string_equal_prefix_nocase( relative, "script/" ) ) {
		relative += string_length( "script/" );
	}
	if ( string_empty( relative ) || !string_equal_suffix_nocase( relative, ".script" ) ) {
		return false;
	}
	outRelativePath = relative;
	return true;
}

class AssetUsedScriptCollector : public Entity::Visitor
{
	StringSetNoCaseLocal& m_usedScripts;
public:
	explicit AssetUsedScriptCollector( StringSetNoCaseLocal& usedScripts ) : m_usedScripts( usedScripts ){
	}
	void visit( const char* key, const char* value ) override {
		(void)key;
		CopiedString relative;
		if ( AssetBrowser_normalizeScriptReference( value, relative ) ) {
			m_usedScripts.emplace( relative );
		}
	}
};

class AssetUsedScriptWalker : public scene::Graph::Walker
{
	StringSetNoCaseLocal& m_usedScripts;
public:
	explicit AssetUsedScriptWalker( StringSetNoCaseLocal& usedScripts ) : m_usedScripts( usedScripts ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		(void)instance;
		if ( Entity* entity = Node_getEntity( path.top() ) ) {
			AssetUsedScriptCollector collector( m_usedScripts );
			entity->forEachKeyValue( collector );
		}
		return true;
	}
};

StringSetNoCaseLocal AssetBrowser_collectUsedScripts(){
	StringSetNoCaseLocal usedScripts;
	if ( Map_Valid( g_map ) ) {
		GlobalSceneGraph().traverse( AssetUsedScriptWalker( usedScripts ) );
	}
	return usedScripts;
}

class AssetWorldspawnKeyCollector : public Entity::Visitor
{
	std::map<CopiedString, CopiedString, StringLessNoCaseLocal>& m_keys;
public:
	explicit AssetWorldspawnKeyCollector( std::map<CopiedString, CopiedString, StringLessNoCaseLocal>& keys ) : m_keys( keys ){
	}
	void visit( const char* key, const char* value ) override {
		if ( !string_empty( key ) ) {
			m_keys[CopiedString( key )] = value;
		}
	}
};

const char* AssetBrowser_firstNonEmptyKey( const Entity& entity, const std::initializer_list<const char*>& keys ){
	for ( const char* key : keys ) {
		const char* value = entity.getKeyValue( key );
		if ( !string_empty( value ) ) {
			return value;
		}
	}
	return "";
}

struct AssetGlobalsStats
{
	int brushes = 0;
	int patches = 0;
	int entities = 0;
	int entitiesIngame = 0;
	int groupEntities = 0;
	int groupEntitiesIngame = 0;

	int playerStart = 0;
	int deathmatchStarts = 0;
	int ctfRedStarts = 0;
	int ctfBlueStarts = 0;

	bool worldspawnFound = false;
	CopiedString worldspawnMessage;
	CopiedString worldspawnAuthor;
	CopiedString worldspawnMusic;
	CopiedString worldspawnSounds;
	std::map<CopiedString, CopiedString, StringLessNoCaseLocal> worldspawnKeys;

	std::map<CopiedString, int, StringLessNoCaseLocal> entityBreakdown;
};

class AssetGlobalsWalker : public scene::Graph::Walker
{
	AssetGlobalsStats& m_stats;
public:
	explicit AssetGlobalsWalker( AssetGlobalsStats& stats ) : m_stats( stats ){
	}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		scene::Node& node = path.top();

		if ( Node_getBrush( node ) != nullptr ) {
			++m_stats.brushes;
		}
		if ( Node_getPatch( node ) != nullptr ) {
			++m_stats.patches;
		}

		if ( Entity* entity = Node_getEntity( node ) ) {
			++m_stats.entities;
			const char* classname = entity->getClassName();
			++m_stats.entityBreakdown[CopiedString( classname )];

			if( entity->isContainer() ){
				++m_stats.groupEntities;
				if( !string_equal_nocase( "func_group", classname ) &&
				    !string_equal_nocase( "_decal", classname ) &&
				    !string_equal_nocase_n( "func_detail", classname, 11 ) ){
					++m_stats.groupEntitiesIngame;
					++m_stats.entitiesIngame;
				}
			}
			else if( !string_equal_nocase_n( "light", classname, 5 ) &&
			         !string_equal_nocase( "misc_model", classname ) &&
			         !string_equal_nocase( "info_null", classname ) ){
				++m_stats.entitiesIngame;
			}

			if ( string_equal_nocase( classname, "worldspawn" ) ) {
				m_stats.worldspawnFound = true;
				m_stats.worldspawnMessage = AssetBrowser_firstNonEmptyKey( *entity, { "message" } );
				m_stats.worldspawnAuthor = AssetBrowser_firstNonEmptyKey( *entity, { "author", "_author", "mapauthor" } );
				m_stats.worldspawnMusic = AssetBrowser_firstNonEmptyKey( *entity, { "music", "_music", "soundtrack", "ambient" } );
				m_stats.worldspawnSounds = AssetBrowser_firstNonEmptyKey( *entity, { "sounds" } );
				AssetWorldspawnKeyCollector collector( m_stats.worldspawnKeys );
				entity->forEachKeyValue( collector );
			}
			else if ( string_equal_nocase( classname, "info_player_start" ) ) {
				++m_stats.playerStart;
			}
			else if ( string_equal_nocase( classname, "info_player_deathmatch" ) ) {
				++m_stats.deathmatchStarts;
			}
			else if ( string_equal_nocase( classname, "team_CTF_redplayer" )
			       || string_equal_nocase( classname, "team_CTF_redspawn" ) ) {
				++m_stats.ctfRedStarts;
			}
			else if ( string_equal_nocase( classname, "team_CTF_blueplayer" )
			       || string_equal_nocase( classname, "team_CTF_bluespawn" ) ) {
				++m_stats.ctfBlueStarts;
			}
		}

		return true;
	}
};

AssetGlobalsStats AssetBrowser_collectGlobalsStats(){
	AssetGlobalsStats stats;
	if ( Map_Valid( g_map ) ) {
		GlobalSceneGraph().traverse( AssetGlobalsWalker( stats ) );
	}
	return stats;
}

QIcon AssetGlobals_statusIcon( bool ok ){
	return QApplication::style()->standardIcon( ok ? QStyle::SP_DialogApplyButton : QStyle::SP_DialogCancelButton );
}

class AssetGlobalsPanel final : public QWidget
{
	QListWidget* m_categories = nullptr;
	QStackedWidget* m_pages = nullptr;

	QTreeWidget* m_checklistTree = nullptr;
	QLabel* m_totalBrushes = nullptr;
	QLabel* m_totalPatches = nullptr;
	QLabel* m_totalEntities = nullptr;
	QLabel* m_ingameEntities = nullptr;
	QLabel* m_groupEntities = nullptr;
	QLabel* m_ingameGroupEntities = nullptr;

	QTreeWidget* m_spawnTree = nullptr;
	QTreeWidget* m_worldspawnTree = nullptr;
	QTreeWidget* m_entityBreakdownTree = nullptr;
	QTimer* m_autoRefreshTimer = nullptr;
	bool m_populatingWorldspawn = false;
	std::size_t m_lastSceneRevision = 0;
	AssetGlobalsStats m_lastStats;

	struct ChecklistRow
	{
		QTreeWidgetItem* item = nullptr;
		QPushButton* actionButton = nullptr;
	};

	ChecklistRow m_worldspawnMessageRow;
	ChecklistRow m_worldspawnAuthorRow;
	ChecklistRow m_worldspawnAudioRow;
	ChecklistRow m_playerStartRow;
	ChecklistRow m_deathmatchRow;
	ChecklistRow m_ctfBalanceRow;

	void setChecklistRow( ChecklistRow& row, const QString& check, bool ok, const QString& detail, bool showAction ){
		if ( row.item == nullptr ) {
			return;
		}
		row.item->setIcon( 0, AssetGlobals_statusIcon( ok ) );
		row.item->setText( 1, check );
		row.item->setText( 2, detail );
		if ( row.actionButton != nullptr ) {
			row.actionButton->setVisible( showAction );
			row.actionButton->setEnabled( showAction );
		}
	}

	void createChecklistRows(){
		m_checklistTree->clear();

		auto createRow = [this]( ChecklistRow& row, const QString& actionLabel, const std::function<void()>& action ){
			row.item = new QTreeWidgetItem( m_checklistTree );
			row.item->setText( 1, "" );
			row.item->setText( 2, "" );
			row.actionButton = new QPushButton( actionLabel );
			row.actionButton->setFlat( true );
			QObject::connect( row.actionButton, &QPushButton::clicked, this, [this, action](){
				if ( action ) {
					action();
					refresh();
				}
			} );
			m_checklistTree->setItemWidget( row.item, 3, row.actionButton );
		};

		createRow( m_worldspawnMessageRow, i18n::tr( "Add key" ), [](){
			AssetBrowser_setWorldspawnKey( "message", "Untitled Map" );
		} );
		createRow( m_worldspawnAuthorRow, i18n::tr( "Add key" ), [](){
			QString author = QString::fromLocal8Bit( qgetenv( "USERNAME" ) );
			if ( author.trimmed().isEmpty() ) {
				author = "Unknown Author";
			}
			AssetBrowser_setWorldspawnKey( "author", author.toUtf8().constData() );
		} );
		createRow( m_worldspawnAudioRow, i18n::tr( "Add key" ), [](){
			const char* key = AssetBrowser_worldspawnExpectedAudioKey();
			AssetBrowser_setWorldspawnKey( key, string_equal_nocase( key, "sounds" ) ? "world/" : "music/" );
		} );
		createRow( m_playerStartRow, i18n::tr( "Create" ), [](){
			AssetBrowser_addPointEntities( "info_player_start", 1 );
		} );
		createRow( m_deathmatchRow, i18n::tr( "Top up" ), [this](){
			const int needed = std::max( 0, 8 - m_lastStats.deathmatchStarts );
			if ( needed > 0 ) {
				AssetBrowser_addPointEntities( "info_player_deathmatch", needed, m_lastStats.deathmatchStarts );
			}
		} );
		createRow( m_ctfBalanceRow, i18n::tr( "Balance" ), [this](){
			int red = m_lastStats.ctfRedStarts;
			int blue = m_lastStats.ctfBlueStarts;
			const int target = std::max( 2, std::max( red, blue ) );
			if ( red < target ) {
				AssetBrowser_addPointEntities( "team_CTF_redspawn", target - red, red );
			}
			if ( blue < target ) {
				AssetBrowser_addPointEntities( "team_CTF_bluespawn", target - blue, blue );
			}
		} );
	}

	void updateChecklistRows( const AssetGlobalsStats& stats ){
		const bool hasWorldspawnMessage = !stats.worldspawnMessage.empty();
		const bool hasWorldspawnAuthor = !stats.worldspawnAuthor.empty();
		const bool hasAuthorKey = stats.worldspawnKeys.contains( CopiedString( "author" ) );

		const bool hasMusic = !stats.worldspawnMusic.empty();
		const bool hasSounds = !stats.worldspawnSounds.empty();
		const bool expectsSounds = string_equal_nocase( AssetBrowser_worldspawnExpectedAudioKey(), "sounds" );
		const bool hasExpectedAudio = expectsSounds ? hasSounds : hasMusic;

		const bool hasPlayerStart = stats.playerStart > 0;
		const bool hasDeathmatch = stats.deathmatchStarts > 0;
		const bool enoughDeathmatch = stats.deathmatchStarts >= 8;
		const bool hasCtf = ( stats.ctfRedStarts + stats.ctfBlueStarts ) > 0;
		const bool ctfBalanced = !hasCtf || ( stats.ctfRedStarts == stats.ctfBlueStarts && stats.ctfRedStarts >= 2 );

		QString authorDetail;
		if ( hasWorldspawnAuthor ) {
			authorDetail = hasAuthorKey
			               ? i18n::tr( "Present." )
			               : i18n::tr( "Present via legacy key (_author/mapauthor)." );
		}
		else
		{
			authorDetail = i18n::tr( "Missing author key." );
		}

		QString audioDetail;
		if ( hasExpectedAudio ) {
			audioDetail = i18n::tr( "Present." );
		}
		else if ( expectsSounds && hasMusic ) {
			audioDetail = i18n::tr( "Found \"music\", but classic idTech2 expects \"sounds\"." );
		}
		else if ( !expectsSounds && hasSounds ) {
			audioDetail = i18n::tr( "Found \"sounds\", but this game expects \"music\"." );
		}
		else{
			audioDetail = i18n::tr( "Missing \"%1\" key." ).arg( AssetBrowser_worldspawnExpectedAudioKey() );
		}

		setChecklistRow( m_worldspawnMessageRow, i18n::tr( "Worldspawn message" ), hasWorldspawnMessage,
		                 hasWorldspawnMessage ? i18n::tr( "Present." ) : i18n::tr( "Missing \"message\" key." ),
		                 !hasWorldspawnMessage );
		setChecklistRow( m_worldspawnAuthorRow, i18n::tr( "Worldspawn author" ), hasWorldspawnAuthor,
		                 authorDetail, !hasWorldspawnAuthor );
		setChecklistRow( m_worldspawnAudioRow,
		                 string_equal_nocase( AssetBrowser_worldspawnExpectedAudioKey(), "sounds" )
		                 ? i18n::tr( "Worldspawn sounds" )
		                 : i18n::tr( "Worldspawn music" ),
		                 hasExpectedAudio, audioDetail, !hasExpectedAudio );
		setChecklistRow( m_playerStartRow, i18n::tr( "Player start" ), hasPlayerStart,
		                 hasPlayerStart ? i18n::tr( "At least one player start exists." ) : i18n::tr( "No player start found." ),
		                 !hasPlayerStart );
		setChecklistRow( m_deathmatchRow, i18n::tr( "Deathmatch spawns" ), hasDeathmatch && enoughDeathmatch,
		                 i18n::tr( "%1 spawn(s)." ).arg( stats.deathmatchStarts ),
		                 stats.deathmatchStarts < 8 );
		setChecklistRow( m_ctfBalanceRow, i18n::tr( "CTF team balance" ), ctfBalanced,
		                 hasCtf ? i18n::tr( "Red: %1, Blue: %2." ).arg( stats.ctfRedStarts ).arg( stats.ctfBlueStarts )
		                        : i18n::tr( "No CTF spawns found." ),
		                 hasCtf && !ctfBalanced );
	}

	void updateOverviewLabels( const AssetGlobalsStats& stats ){
		m_totalBrushes->setText( QString::number( stats.brushes ) );
		m_totalPatches->setText( QString::number( stats.patches ) );
		m_totalEntities->setText( QString::number( stats.entities ) );
		m_ingameEntities->setText( QString::number( stats.entitiesIngame ) );
		m_groupEntities->setText( QString::number( stats.groupEntities ) );
		m_ingameGroupEntities->setText( QString::number( stats.groupEntitiesIngame ) );
	}

	void updateSpawnRows( const AssetGlobalsStats& stats ){
		m_spawnTree->clear();
		auto addSpawn = [this]( const char* label, int value ){
			auto* item = new QTreeWidgetItem( m_spawnTree );
			item->setText( 0, label );
			item->setText( 1, QString::number( value ) );
		};
		addSpawn( "info_player_start", stats.playerStart );
		addSpawn( "info_player_deathmatch", stats.deathmatchStarts );
		addSpawn( "team_CTF_redplayer/redspawn", stats.ctfRedStarts );
		addSpawn( "team_CTF_blueplayer/bluespawn", stats.ctfBlueStarts );
	}

	void updateWorldspawnRows( const AssetGlobalsStats& stats ){
		m_populatingWorldspawn = true;
		m_worldspawnTree->clear();

		StringSetNoCaseLocal addedKeys;
		auto addWorldspawnKey = [this, &addedKeys]( const char* key, const char* value, bool editable, bool important ){
			auto* item = new QTreeWidgetItem( m_worldspawnTree );
			item->setText( 0, key );
			item->setText( 1, value );
			item->setData( 0, Qt::ItemDataRole::UserRole, QByteArray( key ) );
			item->setData( 0, Qt::ItemDataRole::UserRole + 1, QString::fromLatin1( key ) );
			Qt::ItemFlags flags = item->flags();
			if ( editable ) {
				flags |= Qt::ItemFlag::ItemIsEditable;
			}
			else
			{
				flags &= ~Qt::ItemFlag::ItemIsEditable;
			}
			item->setFlags( flags );
			if ( important ) {
				QFont bold = item->font( 0 );
				bold.setBold( true );
				item->setFont( 0, bold );
				item->setFont( 1, bold );
			}
			addedKeys.emplace( key );
		};

		for ( const auto& [key, value] : stats.worldspawnKeys ) {
			const char* keyText = key.c_str();
			const bool important = string_equal_nocase( keyText, "message" )
			                    || string_equal_nocase( keyText, "author" )
			                    || string_equal_nocase( keyText, "music" )
			                    || string_equal_nocase( keyText, "sounds" );
			const bool editable = !string_equal_nocase( keyText, "classname" );
			addWorldspawnKey( keyText, value.c_str(), editable, important );
		}

		auto ensureImportantKey = [&]( const char* key ){
			if ( !addedKeys.contains( CopiedString( key ) ) ) {
				addWorldspawnKey( key, "", true, true );
			}
		};
		ensureImportantKey( "message" );
		ensureImportantKey( "author" );
		ensureImportantKey( AssetBrowser_worldspawnExpectedAudioKey() );

		m_populatingWorldspawn = false;
	}

	void updateEntityBreakdownRows( const AssetGlobalsStats& stats ){
		m_entityBreakdownTree->clear();
		for ( const auto& pair : stats.entityBreakdown ) {
			auto* item = new QTreeWidgetItem( m_entityBreakdownTree );
			item->setData( 0, Qt::ItemDataRole::DisplayRole, pair.first.c_str() );
			item->setData( 1, Qt::ItemDataRole::DisplayRole, pair.second );
		}
	}

	void refreshChecklistOnly(){
		m_lastStats = AssetBrowser_collectGlobalsStats();
		updateChecklistRows( m_lastStats );
		m_lastSceneRevision = g_assetGlobalsSceneRevision;
	}

public:
	explicit AssetGlobalsPanel( QWidget* parent ) : QWidget( parent ){
		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 0, 0, 0, 0 );
		root->setSpacing( 0 );

		{
			auto* toolbar = new QToolBar;
			root->addWidget( toolbar );
			const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
			toolbar->setIconSize( QSize( iconSize, iconSize ) );
			toolbar_append_button( toolbar, i18n::tr( "Refresh" ).toUtf8().constData(), "texbro_refresh.png",
			                       MemberCaller<AssetGlobalsPanel, void(), &AssetGlobalsPanel::refresh>( *this ) );
		}

		auto* splitter = new QSplitter;
		root->addWidget( splitter, 1 );

		m_categories = new QListWidget;
		m_categories->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
		m_categories->addItems( { i18n::tr( "Checklist" ), i18n::tr( "Overview" ), i18n::tr( "Spawns" ), i18n::tr( "Worldspawn" ), i18n::tr( "Entities" ) } );
		splitter->addWidget( m_categories );

		m_pages = new QStackedWidget;
		splitter->addWidget( m_pages );
		splitter->setStretchFactor( 0, 0 );
		splitter->setStretchFactor( 1, 1 );

		{
			auto* page = new QWidget;
			auto* pageLayout = new QVBoxLayout( page );
			pageLayout->setContentsMargins( 8, 8, 8, 8 );

			auto* checklistGroup = new QGroupBox( i18n::tr( "Map Checklist" ) );
			auto* checklistLayout = new QVBoxLayout( checklistGroup );
			m_checklistTree = new QTreeWidget;
			m_checklistTree->setColumnCount( 4 );
			m_checklistTree->setHeaderLabels( { "", i18n::tr( "Check" ), i18n::tr( "Details" ), i18n::tr( "Action" ) } );
			m_checklistTree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::ResizeToContents );
			m_checklistTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
			m_checklistTree->header()->setSectionResizeMode( 2, QHeaderView::ResizeMode::Stretch );
			m_checklistTree->header()->setSectionResizeMode( 3, QHeaderView::ResizeMode::ResizeToContents );
			m_checklistTree->setRootIsDecorated( false );
			m_checklistTree->setUniformRowHeights( true );
			checklistLayout->addWidget( m_checklistTree );
			pageLayout->addWidget( checklistGroup, 1 );

			m_pages->addWidget( page );
		}

		{
			auto* page = new QWidget;
			auto* pageLayout = new QVBoxLayout( page );
			pageLayout->setContentsMargins( 8, 8, 8, 8 );

			auto* summary = new QGroupBox( i18n::tr( "Summary" ) );
			auto* form = new QFormLayout( summary );
			m_totalBrushes = new QLabel;
			m_totalPatches = new QLabel;
			m_totalEntities = new QLabel;
			m_ingameEntities = new QLabel;
			m_groupEntities = new QLabel;
			m_ingameGroupEntities = new QLabel;
			form->addRow( i18n::tr( "Brushes:" ), m_totalBrushes );
			form->addRow( i18n::tr( "Patches:" ), m_totalPatches );
			form->addRow( i18n::tr( "Entities:" ), m_totalEntities );
			form->addRow( i18n::tr( "Ingame Entities:" ), m_ingameEntities );
			form->addRow( i18n::tr( "Group Entities:" ), m_groupEntities );
			form->addRow( i18n::tr( "Ingame Group Entities:" ), m_ingameGroupEntities );
			pageLayout->addWidget( summary );
			pageLayout->addStretch( 1 );

			m_pages->addWidget( page );
		}

		{
			auto* page = new QWidget;
			auto* pageLayout = new QVBoxLayout( page );
			pageLayout->setContentsMargins( 8, 8, 8, 8 );
			m_spawnTree = new QTreeWidget;
			m_spawnTree->setColumnCount( 2 );
			m_spawnTree->setHeaderLabels( { i18n::tr( "Spawn Type" ), i18n::tr( "Count" ) } );
			m_spawnTree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::Stretch );
			m_spawnTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
			m_spawnTree->setRootIsDecorated( false );
			m_spawnTree->setUniformRowHeights( true );
			pageLayout->addWidget( m_spawnTree, 1 );
			m_pages->addWidget( page );
		}

		{
			auto* page = new QWidget;
			auto* pageLayout = new QVBoxLayout( page );
			pageLayout->setContentsMargins( 8, 8, 8, 8 );
			m_worldspawnTree = new QTreeWidget;
			m_worldspawnTree->setColumnCount( 2 );
			m_worldspawnTree->setHeaderLabels( { i18n::tr( "Worldspawn Key" ), i18n::tr( "Value" ) } );
			m_worldspawnTree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::ResizeToContents );
			m_worldspawnTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::Stretch );
			m_worldspawnTree->setRootIsDecorated( false );
			m_worldspawnTree->setUniformRowHeights( true );
			m_worldspawnTree->setEditTriggers( QAbstractItemView::EditTrigger::DoubleClicked
			                                 | QAbstractItemView::EditTrigger::EditKeyPressed
			                                 | QAbstractItemView::EditTrigger::SelectedClicked );
			pageLayout->addWidget( m_worldspawnTree, 1 );
			m_pages->addWidget( page );
		}

		{
			auto* page = new QWidget;
			auto* pageLayout = new QVBoxLayout( page );
			pageLayout->setContentsMargins( 8, 8, 8, 8 );
			m_entityBreakdownTree = new QTreeWidget;
			m_entityBreakdownTree->setColumnCount( 2 );
			m_entityBreakdownTree->setHeaderLabels( { i18n::tr( "Entity Class" ), i18n::tr( "Count" ) } );
			m_entityBreakdownTree->setSortingEnabled( true );
			m_entityBreakdownTree->sortByColumn( 0, Qt::SortOrder::AscendingOrder );
			m_entityBreakdownTree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::Stretch );
			m_entityBreakdownTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
			m_entityBreakdownTree->setRootIsDecorated( false );
			m_entityBreakdownTree->setUniformRowHeights( true );
			pageLayout->addWidget( m_entityBreakdownTree, 1 );
			m_pages->addWidget( page );
		}

		QObject::connect( m_categories, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex );
		QObject::connect( m_worldspawnTree, &QTreeWidget::itemChanged, this, [this]( QTreeWidgetItem* item, int column ){
			if ( m_populatingWorldspawn || item == nullptr ) {
				return;
			}
			if ( column == 0 ) {
				const QString keyLabel = item->data( 0, Qt::ItemDataRole::UserRole + 1 ).toString();
				if ( !keyLabel.isEmpty() && item->text( 0 ) != keyLabel ) {
					m_populatingWorldspawn = true;
					item->setText( 0, keyLabel );
					m_populatingWorldspawn = false;
				}
				return;
			}
			if ( column != 1 ) {
				return;
			}
			const QByteArray key = item->data( 0, Qt::ItemDataRole::UserRole ).toByteArray();
			if ( key.isEmpty() ) {
				return;
			}
			if ( string_equal_nocase( key.constData(), "classname" ) ) {
				m_populatingWorldspawn = true;
				item->setText( 1, "worldspawn" );
				m_populatingWorldspawn = false;
				return;
			}
			const QByteArray value = item->text( 1 ).toUtf8();
			AssetBrowser_setWorldspawnKey( key.constData(), value.constData() );
			m_lastStats = AssetBrowser_collectGlobalsStats();
			updateChecklistRows( m_lastStats );
			updateWorldspawnRows( m_lastStats );
		} );

		m_autoRefreshTimer = new QTimer( this );
		m_autoRefreshTimer->setInterval( 250 );
		QObject::connect( m_autoRefreshTimer, &QTimer::timeout, this, [this](){
			if ( isVisible() && m_lastSceneRevision != g_assetGlobalsSceneRevision ) {
				refreshChecklistOnly();
			}
		} );
		m_autoRefreshTimer->start();

		createChecklistRows();
		m_categories->setCurrentRow( 0 );
		refresh();
	}

	void refresh(){
		m_lastStats = AssetBrowser_collectGlobalsStats();
		m_lastSceneRevision = g_assetGlobalsSceneRevision;
		updateChecklistRows( m_lastStats );
		updateOverviewLabels( m_lastStats );
		updateSpawnRows( m_lastStats );
		updateWorldspawnRows( m_lastStats );
		updateEntityBreakdownRows( m_lastStats );
	}
};

class AssetSurfacesPanel final : public QWidget
{
	QTreeWidget* m_tree = nullptr;
	QLineEdit* m_filterEntry = nullptr;
	QToolButton* m_globalButton = nullptr;
	QToolButton* m_usedButton = nullptr;
	QToolButton* m_clearButton = nullptr;
	QTimer* m_filterRebuildTimer = nullptr;

	CopiedString m_filter = g_assetSurfacesFilter;
	bool m_filterGlobal = g_assetSurfacesFilterGlobal;
	bool m_filterUsed = g_assetSurfacesFilterUsed;

	void updateClearButton(){
		if ( m_clearButton != nullptr ) {
			m_clearButton->setEnabled( !m_filter.empty() || m_filterGlobal || m_filterUsed );
		}
	}

	void updateUsedButtonLabel( std::size_t usedCount ){
		if ( m_usedButton != nullptr ) {
			m_usedButton->setText( StringStream<64>( "Used (", usedCount, ')' ).c_str() );
		}
	}

	void persistState() const {
		g_assetSurfacesFilter = m_filter;
		g_assetSurfacesFilterGlobal = m_filterGlobal;
		g_assetSurfacesFilterUsed = m_filterUsed;
	}

public:
	explicit AssetSurfacesPanel( QWidget* parent ) : QWidget( parent ){
		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 0, 0, 0, 0 );
		root->setSpacing( 0 );

		auto* toolbar = new QToolBar;
		root->addWidget( toolbar );
		const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
		toolbar->setIconSize( QSize( iconSize, iconSize ) );
		toolbar_append_button( toolbar, "UV Editor", "select_mouseuv.png", "ToggleUVView" );
		toolbar_append_button( toolbar, "Refresh Surfaces", "texbro_refresh.png",
		                       MemberCaller<AssetSurfacesPanel, void(), &AssetSurfacesPanel::rebuild>( *this ) );

		auto* filterBar = new QWidget;
		auto* filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		m_filterEntry = new QLineEdit;
		m_filterEntry->setClearButtonEnabled( true );
		m_filterEntry->setPlaceholderText( "Filter by name" );
		filterLayout->addWidget( m_filterEntry, 1 );

		m_globalButton = new QToolButton;
		m_globalButton->setAutoRaise( true );
		m_globalButton->setCheckable( true );
		m_globalButton->setIcon( new_local_icon( "f-world.png" ) );
		m_globalButton->setToolTip( "Global switch: include all materials" );
		filterLayout->addWidget( m_globalButton );

		m_usedButton = new QToolButton;
		m_usedButton->setAutoRaise( true );
		m_usedButton->setCheckable( true );
		m_usedButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		m_usedButton->setText( "Used (0)" );
		m_usedButton->setToolTip( "Show only surfaces used in the current level" );
		filterLayout->addWidget( m_usedButton );

		m_clearButton = new QToolButton;
		m_clearButton->setAutoRaise( true );
		m_clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		m_clearButton->setToolTip( "Clear filters" );
		filterLayout->addWidget( m_clearButton );

		root->addWidget( filterBar );

		m_tree = new QTreeWidget;
		m_tree->setColumnCount( 4 );
		m_tree->setHeaderLabels( { i18n::tr( "Surface" ), i18n::tr( "Brush Faces" ), i18n::tr( "Patches" ), i18n::tr( "Total" ) } );
		m_tree->setRootIsDecorated( false );
		m_tree->setSortingEnabled( true );
		m_tree->sortByColumn( 0, Qt::SortOrder::AscendingOrder );
		m_tree->setUniformRowHeights( true );
		m_tree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::Stretch );
		m_tree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
		m_tree->header()->setSectionResizeMode( 2, QHeaderView::ResizeMode::ResizeToContents );
		m_tree->header()->setSectionResizeMode( 3, QHeaderView::ResizeMode::ResizeToContents );
		root->addWidget( m_tree, 1 );

		m_filterEntry->setText( m_filter.c_str() );
		m_globalButton->setChecked( m_filterGlobal );
		m_usedButton->setChecked( m_filterUsed );
		m_filterRebuildTimer = new QTimer( this );
		m_filterRebuildTimer->setSingleShot( true );
		QObject::connect( m_filterRebuildTimer, &QTimer::timeout, this, [this](){
			rebuild();
		} );

		QObject::connect( m_filterEntry, &QLineEdit::textChanged, this, [this]( const QString& text ){
			m_filter = text.toLatin1().constData();
			persistState();
			updateClearButton();
			m_filterRebuildTimer->start( kAssetFilterRebuildDelayMilliseconds );
		} );
		QObject::connect( m_globalButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterGlobal = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_usedButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterUsed = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_clearButton, &QToolButton::clicked, this, [this](){
			m_filterRebuildTimer->stop();
			m_filter = "";
			m_filterGlobal = false;
			m_filterUsed = false;
			{
				const QSignalBlocker filterBlocker( m_filterEntry );
				const QSignalBlocker globalBlocker( m_globalButton );
				const QSignalBlocker usedBlocker( m_usedButton );
				m_filterEntry->clear();
				m_globalButton->setChecked( false );
				m_usedButton->setChecked( false );
			}
			persistState();
			updateClearButton();
			rebuild();
		} );
		QObject::connect( m_tree, &QTreeWidget::itemActivated, this, []( QTreeWidgetItem* item, int column ){
			(void)column;
			const QByteArray shader = item->data( 0, Qt::ItemDataRole::UserRole ).toByteArray();
			if ( !shader.isEmpty() ) {
				Select_FacesAndPatchesByShader( shader.constData() );
			}
		} );

	}

	void cancelScheduledRebuild(){
		m_filterRebuildTimer->stop();
	}

	void rebuild(){
		m_filterRebuildTimer->stop();
		if ( m_tree == nullptr ) {
			return;
		}

		m_tree->clear();
		const AssetSurfaceUsageMap usage = AssetBrowser_collectSurfaceUsage();
		StringSetNoCaseLocal allSurfaces;

		if ( m_filterGlobal ) {
			allSurfaces = AssetBrowser_collectAllSurfaceShaders();
		}
		for ( const auto& [shader, stats] : usage ) {
			(void)stats;
			allSurfaces.emplace( shader );
		}

		updateUsedButtonLabel( usage.size() );

		for ( const CopiedString& shader : allSurfaces ) {
			const char* shaderName = shader.c_str();
			const char* leaf = path_get_filename_start( shaderName );
			if ( !string_contains_nocase( shaderName, m_filter.c_str() ) && !string_contains_nocase( leaf, m_filter.c_str() ) ) {
				continue;
			}

			const auto found = usage.find( shader );
			AssetSurfaceUsage counts;
			if ( found != usage.end() ) {
				counts = found->second;
			}

			if ( m_filterUsed && counts.total() <= 0 ) {
				continue;
			}

			auto* item = new QTreeWidgetItem( m_tree );
			item->setText( 0, shaderName );
			item->setData( 0, Qt::ItemDataRole::UserRole, shaderName );
			item->setData( 1, Qt::ItemDataRole::DisplayRole, counts.brushFaces );
			item->setData( 2, Qt::ItemDataRole::DisplayRole, counts.patches );
			item->setData( 3, Qt::ItemDataRole::DisplayRole, counts.total() );
		}

		updateClearButton();
	}
};

class AssetAIListPanel final : public QWidget
{
	QTreeWidget* m_tree = nullptr;
	QLineEdit* m_filterEntry = nullptr;
	QToolButton* m_globalButton = nullptr;
	QToolButton* m_usedButton = nullptr;
	QToolButton* m_clearButton = nullptr;
	QTimer* m_filterRebuildTimer = nullptr;

	CopiedString m_filter = g_assetAIFilter;
	bool m_filterGlobal = g_assetAIFilterGlobal;
	bool m_filterUsed = g_assetAIFilterUsed;

	void persistState() const {
		g_assetAIFilter = m_filter;
		g_assetAIFilterGlobal = m_filterGlobal;
		g_assetAIFilterUsed = m_filterUsed;
	}

	void updateClearButton(){
		if ( m_clearButton != nullptr ) {
			m_clearButton->setEnabled( !m_filter.empty() || m_filterGlobal || m_filterUsed );
		}
	}

	void updateUsedButtonLabel( std::size_t usedCount ){
		if ( m_usedButton != nullptr ) {
			m_usedButton->setText( StringStream<64>( "Used (", usedCount, ')' ).c_str() );
		}
	}

public:
	explicit AssetAIListPanel( QWidget* parent ) : QWidget( parent ){
		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 0, 0, 0, 0 );
		root->setSpacing( 0 );

		auto* toolbar = new QToolBar;
		root->addWidget( toolbar );
		const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
		toolbar->setIconSize( QSize( iconSize, iconSize ) );
		toolbar_append_button( toolbar, "Find / Replace...", "texbro_find-replace.png", "FindReplaceEntities" );
		toolbar_append_button( toolbar, "Flush / Reload AI", "refresh_models.png",
		                       MemberCaller<AssetAIListPanel, void(), &AssetAIListPanel::rebuild>( *this ) );

		auto* filterBar = new QWidget;
		auto* filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		m_filterEntry = new QLineEdit;
		m_filterEntry->setClearButtonEnabled( true );
		m_filterEntry->setPlaceholderText( "Filter by name" );
		filterLayout->addWidget( m_filterEntry, 1 );

		m_globalButton = new QToolButton;
		m_globalButton->setAutoRaise( true );
		m_globalButton->setCheckable( true );
		m_globalButton->setIcon( new_local_icon( "f-world.png" ) );
		m_globalButton->setToolTip( "Global switch: broaden AI class matching" );
		filterLayout->addWidget( m_globalButton );

		m_usedButton = new QToolButton;
		m_usedButton->setAutoRaise( true );
		m_usedButton->setCheckable( true );
		m_usedButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		m_usedButton->setText( "Used (0)" );
		m_usedButton->setToolTip( "Show only AI classes used in the current level" );
		filterLayout->addWidget( m_usedButton );

		m_clearButton = new QToolButton;
		m_clearButton->setAutoRaise( true );
		m_clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		m_clearButton->setToolTip( "Clear filters" );
		filterLayout->addWidget( m_clearButton );

		root->addWidget( filterBar );

		m_tree = new QTreeWidget;
		m_tree->setColumnCount( 2 );
		m_tree->setHeaderLabels( { i18n::tr( "AI Class" ), i18n::tr( "Used" ) } );
		m_tree->setRootIsDecorated( false );
		m_tree->setUniformRowHeights( true );
		m_tree->setSortingEnabled( true );
		m_tree->sortByColumn( 0, Qt::SortOrder::AscendingOrder );
		m_tree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::Stretch );
		m_tree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
		root->addWidget( m_tree, 1 );

		m_filterEntry->setText( m_filter.c_str() );
		m_globalButton->setChecked( m_filterGlobal );
		m_usedButton->setChecked( m_filterUsed );
		m_filterRebuildTimer = new QTimer( this );
		m_filterRebuildTimer->setSingleShot( true );
		QObject::connect( m_filterRebuildTimer, &QTimer::timeout, this, [this](){
			rebuild();
		} );

		QObject::connect( m_filterEntry, &QLineEdit::textChanged, this, [this]( const QString& text ){
			m_filter = text.toLatin1().constData();
			persistState();
			updateClearButton();
			m_filterRebuildTimer->start( kAssetFilterRebuildDelayMilliseconds );
		} );
		QObject::connect( m_globalButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterGlobal = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_usedButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterUsed = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_clearButton, &QToolButton::clicked, this, [this](){
			m_filterRebuildTimer->stop();
			m_filter = "";
			m_filterGlobal = false;
			m_filterUsed = false;
			{
				const QSignalBlocker filterBlocker( m_filterEntry );
				const QSignalBlocker globalBlocker( m_globalButton );
				const QSignalBlocker usedBlocker( m_usedButton );
				m_filterEntry->clear();
				m_globalButton->setChecked( false );
				m_usedButton->setChecked( false );
			}
			persistState();
			updateClearButton();
			rebuild();
		} );

	}

	void cancelScheduledRebuild(){
		m_filterRebuildTimer->stop();
	}

	void rebuild(){
		m_filterRebuildTimer->stop();
		m_tree->clear();

		const StringSetNoCaseLocal allClasses = AssetBrowser_collectAIClasses( m_filterGlobal );
		const StringSetNoCaseLocal usedClasses = AssetBrowser_collectUsedAIClasses( m_filterGlobal );
		updateUsedButtonLabel( usedClasses.size() );

		for ( const CopiedString& classname : allClasses ) {
			const char* name = classname.c_str();
			if ( !string_contains_nocase( name, m_filter.c_str() ) ) {
				continue;
			}
			const bool used = usedClasses.contains( classname );
			if ( m_filterUsed && !used ) {
				continue;
			}

			auto* item = new QTreeWidgetItem( m_tree );
			item->setText( 0, name );
			item->setText( 1, used ? "Yes" : "No" );
		}

		updateClearButton();
	}
};

class AssetScriptListPanel final : public QWidget
{
	QTreeWidget* m_tree = nullptr;
	QLineEdit* m_filterEntry = nullptr;
	QToolButton* m_globalButton = nullptr;
	QToolButton* m_usedButton = nullptr;
	QToolButton* m_clearButton = nullptr;
	QTimer* m_filterRebuildTimer = nullptr;

	CopiedString m_filter = g_assetScriptFilter;
	bool m_filterGlobal = g_assetScriptFilterGlobal;
	bool m_filterUsed = g_assetScriptFilterUsed;

	void persistState() const {
		g_assetScriptFilter = m_filter;
		g_assetScriptFilterGlobal = m_filterGlobal;
		g_assetScriptFilterUsed = m_filterUsed;
	}

	void updateClearButton(){
		if ( m_clearButton != nullptr ) {
			m_clearButton->setEnabled( !m_filter.empty() || m_filterGlobal || m_filterUsed );
		}
	}

	void updateUsedButtonLabel( std::size_t usedCount ){
		if ( m_usedButton != nullptr ) {
			m_usedButton->setText( StringStream<64>( "Used (", usedCount, ')' ).c_str() );
		}
	}

public:
	explicit AssetScriptListPanel( QWidget* parent ) : QWidget( parent ){
		auto* root = new QVBoxLayout( this );
		root->setContentsMargins( 0, 0, 0, 0 );
		root->setSpacing( 0 );

		auto* toolbar = new QToolBar;
		root->addWidget( toolbar );
		const int iconSize = toolbar->style()->pixelMetric( QStyle::PixelMetric::PM_SmallIconSize );
		toolbar->setIconSize( QSize( iconSize, iconSize ) );
		toolbar_append_button( toolbar, "Find / Replace...", "texbro_find-replace.png", "FindReplaceEntities" );
		toolbar_append_button( toolbar, "Flush / Reload Script List", "refresh_models.png",
		                       MemberCaller<AssetScriptListPanel, void(), &AssetScriptListPanel::rebuild>( *this ) );

		auto* filterBar = new QWidget;
		auto* filterLayout = new QHBoxLayout( filterBar );
		filterLayout->setContentsMargins( 4, 4, 4, 4 );
		filterLayout->setSpacing( 6 );

		m_filterEntry = new QLineEdit;
		m_filterEntry->setClearButtonEnabled( true );
		m_filterEntry->setPlaceholderText( "Filter by name" );
		filterLayout->addWidget( m_filterEntry, 1 );

		m_globalButton = new QToolButton;
		m_globalButton->setAutoRaise( true );
		m_globalButton->setCheckable( true );
		m_globalButton->setIcon( new_local_icon( "f-world.png" ) );
		m_globalButton->setToolTip( "Global switch: include subfolders" );
		filterLayout->addWidget( m_globalButton );

		m_usedButton = new QToolButton;
		m_usedButton->setAutoRaise( true );
		m_usedButton->setCheckable( true );
		m_usedButton->setToolButtonStyle( Qt::ToolButtonStyle::ToolButtonTextOnly );
		m_usedButton->setText( "Used (0)" );
		m_usedButton->setToolTip( "Show only script files referenced in the current level" );
		filterLayout->addWidget( m_usedButton );

		m_clearButton = new QToolButton;
		m_clearButton->setAutoRaise( true );
		m_clearButton->setIcon( new_local_icon( "f-reset.png" ) );
		m_clearButton->setToolTip( "Clear filters" );
		filterLayout->addWidget( m_clearButton );

		root->addWidget( filterBar );

		m_tree = new QTreeWidget;
		m_tree->setColumnCount( 2 );
		m_tree->setHeaderLabels( { i18n::tr( "Script" ), i18n::tr( "Used" ) } );
		m_tree->setRootIsDecorated( false );
		m_tree->setUniformRowHeights( true );
		m_tree->setSortingEnabled( true );
		m_tree->sortByColumn( 0, Qt::SortOrder::AscendingOrder );
		m_tree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::Stretch );
		m_tree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::ResizeToContents );
		root->addWidget( m_tree, 1 );

		m_filterEntry->setText( m_filter.c_str() );
		m_globalButton->setChecked( m_filterGlobal );
		m_usedButton->setChecked( m_filterUsed );
		m_filterRebuildTimer = new QTimer( this );
		m_filterRebuildTimer->setSingleShot( true );
		QObject::connect( m_filterRebuildTimer, &QTimer::timeout, this, [this](){
			rebuild();
		} );

		QObject::connect( m_filterEntry, &QLineEdit::textChanged, this, [this]( const QString& text ){
			m_filter = text.toLatin1().constData();
			persistState();
			updateClearButton();
			m_filterRebuildTimer->start( kAssetFilterRebuildDelayMilliseconds );
		} );
		QObject::connect( m_globalButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterGlobal = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_usedButton, &QToolButton::toggled, this, [this]( bool checked ){
			m_filterUsed = checked;
			persistState();
			rebuild();
		} );
		QObject::connect( m_clearButton, &QToolButton::clicked, this, [this](){
			m_filterRebuildTimer->stop();
			m_filter = "";
			m_filterGlobal = false;
			m_filterUsed = false;
			{
				const QSignalBlocker filterBlocker( m_filterEntry );
				const QSignalBlocker globalBlocker( m_globalButton );
				const QSignalBlocker usedBlocker( m_usedButton );
				m_filterEntry->clear();
				m_globalButton->setChecked( false );
				m_usedButton->setChecked( false );
			}
			persistState();
			updateClearButton();
			rebuild();
		} );

	}

	void cancelScheduledRebuild(){
		m_filterRebuildTimer->stop();
	}

	void rebuild(){
		m_filterRebuildTimer->stop();
		m_tree->clear();

		const StringSetNoCaseLocal allScripts = AssetBrowser_collectScriptFiles( m_filterGlobal );
		const StringSetNoCaseLocal usedScripts = AssetBrowser_collectUsedScripts();
		updateUsedButtonLabel( usedScripts.size() );

		for ( const CopiedString& script : allScripts ) {
			const char* scriptPath = script.c_str();
			const char* leaf = path_get_filename_start( scriptPath );
			if ( !string_contains_nocase( scriptPath, m_filter.c_str() ) && !string_contains_nocase( leaf, m_filter.c_str() ) ) {
				continue;
			}

			const bool used = usedScripts.contains( script );
			if ( m_filterUsed && !used ) {
				continue;
			}

			auto* item = new QTreeWidgetItem( m_tree );
			item->setText( 0, scriptPath );
			item->setText( 1, used ? "Yes" : "No" );
		}

		updateClearButton();
	}
};
}

bool AssetBrowser_isEnabled(){
	return kAssetBrowserEnabled;
}

Vector3& AssetBrowser_defaultAngles(){
	return g_assetBrowserDefaultAngles;
}

QWidget* AssetBrowser_constructWindow( QWidget* toplevel ){
	if ( !AssetBrowser_isEnabled() ) {
		g_assetBrowserTabs = nullptr;
		g_assetBrowserEntitiesTab = -1;
		g_assetBrowserModelsTab = -1;
		g_assetBrowserSoundsTab = -1;
		g_assetBrowserGlobalsTab = nullptr;
		g_assetBrowserSurfacesTab = nullptr;
		g_assetBrowserAITab = nullptr;
		g_assetBrowserScriptTab = nullptr;
		return new QWidget( toplevel );
	}

	auto* tabs = new QTabWidget;
	g_assetBrowserTabs = tabs;
	g_assetBrowserEntitiesTab = -1;
	g_assetBrowserModelsTab = -1;
	g_assetBrowserSoundsTab = -1;
	g_assetBrowserGlobalsTab = nullptr;
	g_assetBrowserSurfacesTab = nullptr;
	g_assetBrowserAITab = nullptr;
	g_assetBrowserScriptTab = nullptr;
	tabs->setTabPosition( QTabWidget::North );

	g_assetBrowserGlobalsTab = new AssetGlobalsPanel( toplevel );
	tabs->addTab( g_assetBrowserGlobalsTab, "Globals" );
	tabs->addTab( TextureBrowser_constructWindow( toplevel ), "Materials" );
	g_assetBrowserSurfacesTab = new AssetSurfacesPanel( toplevel );
	tabs->addTab( g_assetBrowserSurfacesTab, "Surfaces" );
	g_assetBrowserEntitiesTab = tabs->addTab( EntityBrowser_constructWindow( toplevel ), "Entities" );
	g_assetBrowserSoundsTab = tabs->addTab( SoundBrowser_constructWindow( toplevel ), "Sounds" );
	g_assetBrowserModelsTab = tabs->addTab( ModelBrowser_constructWindow( toplevel ), "Objects" );
	if ( AssetBrowser_isIdTech4Game() ) {
		g_assetBrowserAITab = new AssetAIListPanel( toplevel );
		g_assetBrowserScriptTab = new AssetScriptListPanel( toplevel );
		tabs->addTab( g_assetBrowserAITab, "AI" );
		tabs->addTab( g_assetBrowserScriptTab, "Script" );
	}

	QObject::connect( tabs, &QTabWidget::currentChanged, [tabs]( int index ){
		if ( index < 0 || tabs->widget( index ) == nullptr ) {
			return;
		}
		if ( auto* surfaces = static_cast<AssetSurfacesPanel*>( g_assetBrowserSurfacesTab ) ) {
			surfaces->cancelScheduledRebuild();
		}
		if ( auto* ai = static_cast<AssetAIListPanel*>( g_assetBrowserAITab ) ) {
			ai->cancelScheduledRebuild();
		}
		if ( auto* script = static_cast<AssetScriptListPanel*>( g_assetBrowserScriptTab ) ) {
			script->cancelScheduledRebuild();
		}
		if ( tabs->widget( index ) == g_assetBrowserGlobalsTab ) {
			if ( auto* globals = static_cast<AssetGlobalsPanel*>( g_assetBrowserGlobalsTab ) ) {
				globals->refresh();
			}
		}
		if ( tabs->widget( index ) == g_assetBrowserSurfacesTab ) {
			if ( auto* surfaces = static_cast<AssetSurfacesPanel*>( g_assetBrowserSurfacesTab ) ) {
				surfaces->rebuild();
			}
		}
		if ( tabs->widget( index ) == g_assetBrowserAITab ) {
			if ( auto* ai = static_cast<AssetAIListPanel*>( g_assetBrowserAITab ) ) {
				ai->rebuild();
			}
		}
		if ( tabs->widget( index ) == g_assetBrowserScriptTab ) {
			if ( auto* script = static_cast<AssetScriptListPanel*>( g_assetBrowserScriptTab ) ) {
				script->rebuild();
			}
		}
		if ( index == g_assetBrowserEntitiesTab ) {
			EntityBrowser_EnsureTree();
		}
		if ( index == g_assetBrowserSoundsTab ) {
			SoundBrowser_EnsureTree();
		}
		if ( index == g_assetBrowserModelsTab ) {
			ModelBrowser_EnsureTree();
		}
	} );

	return tabs;
}

void AssetBrowser_destroyWindow(){
	g_assetBrowserTabs = nullptr;
	g_assetBrowserEntitiesTab = -1;
	g_assetBrowserModelsTab = -1;
	g_assetBrowserSoundsTab = -1;
	g_assetBrowserGlobalsTab = nullptr;
	g_assetBrowserSurfacesTab = nullptr;
	g_assetBrowserAITab = nullptr;
	g_assetBrowserScriptTab = nullptr;
}

void AssetBrowser_Construct(){
	AddSceneChangeCallback( makeSignalHandler( AssetBrowserSceneChangedCaller( reinterpret_cast<void*>( AssetBrowser_markGlobalsSceneChanged ) ) ) );
	GlobalPreferenceSystem().registerPreference( "AssetSurfacesFilter",
	                                             CopiedStringImportStringCaller( g_assetSurfacesFilter ),
	                                             CopiedStringExportStringCaller( g_assetSurfacesFilter ) );
	GlobalPreferenceSystem().registerPreference( "AssetSurfacesFilterGlobal",
	                                             BoolImportStringCaller( g_assetSurfacesFilterGlobal ),
	                                             BoolExportStringCaller( g_assetSurfacesFilterGlobal ) );
	GlobalPreferenceSystem().registerPreference( "AssetSurfacesFilterUsed",
	                                             BoolImportStringCaller( g_assetSurfacesFilterUsed ),
	                                             BoolExportStringCaller( g_assetSurfacesFilterUsed ) );
	GlobalPreferenceSystem().registerPreference( "AssetAIFilter",
	                                             CopiedStringImportStringCaller( g_assetAIFilter ),
	                                             CopiedStringExportStringCaller( g_assetAIFilter ) );
	GlobalPreferenceSystem().registerPreference( "AssetAIFilterGlobal",
	                                             BoolImportStringCaller( g_assetAIFilterGlobal ),
	                                             BoolExportStringCaller( g_assetAIFilterGlobal ) );
	GlobalPreferenceSystem().registerPreference( "AssetAIFilterUsed",
	                                             BoolImportStringCaller( g_assetAIFilterUsed ),
	                                             BoolExportStringCaller( g_assetAIFilterUsed ) );
	GlobalPreferenceSystem().registerPreference( "AssetScriptFilter",
	                                             CopiedStringImportStringCaller( g_assetScriptFilter ),
	                                             CopiedStringExportStringCaller( g_assetScriptFilter ) );
	GlobalPreferenceSystem().registerPreference( "AssetScriptFilterGlobal",
	                                             BoolImportStringCaller( g_assetScriptFilterGlobal ),
	                                             BoolExportStringCaller( g_assetScriptFilterGlobal ) );
	GlobalPreferenceSystem().registerPreference( "AssetScriptFilterUsed",
	                                             BoolImportStringCaller( g_assetScriptFilterUsed ),
	                                             BoolExportStringCaller( g_assetScriptFilterUsed ) );
}

void AssetBrowser_Destroy(){
}

void AssetBrowser_selectModelsTab(){
	if ( g_assetBrowserTabs == nullptr || g_assetBrowserModelsTab < 0 ) {
		return;
	}
	g_assetBrowserTabs->setCurrentIndex( g_assetBrowserModelsTab );
}
