/*
   Copyright (C) 2026
*/

#include "genai.h"

#include "brushmanip.h"
#include "camwindow.h"
#include "commands.h"
#include "eclasslib.h"
#include "filterbar.h"
#include "grid.h"
#include "ieclass.h"
#include "ientity.h"
#include "iselection.h"
#include "iundo.h"
#include "map.h"
#include "scenelib.h"
#include "preferences.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "url.h"
#include "mainframe.h"

#include "gtkutil/i18n.h"
#include "gtkutil/menu.h"
#include "gtkutil/messagebox.h"
#include "gtkutil/widget.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace
{
bool g_genAIEnabled = false;
bool g_genAIAllowResponseStore = false;
CopiedString g_genAIBaseUrl( "https://api.openai.com/v1" );
CopiedString g_genAIResponsesPath( "/responses" );
CopiedString g_genAIApiKey;
CopiedString g_genAIModel( "gpt-5-mini" );
CopiedString g_genAIOrganization;
CopiedString g_genAIProject;
int g_genAITimeoutMs = 60000;
int g_genAIMaxOutputTokens = 2048;

ToggleItem g_genAIEnabledItem{ BoolExportCaller( g_genAIEnabled ) };

CopiedString g_genAIBlockoutDefaultPrompt( "Create a medium-sized arena with a central space and two flanking routes." );
CopiedString g_genAIBlockoutShader;
CopiedString g_genAIBlockoutFloorShader;
CopiedString g_genAIBlockoutWallShader;
CopiedString g_genAIBlockoutCeilingShader;
CopiedString g_genAIBlockoutSkyShader;
int g_genAIBlockoutRoomCount = 6;
int g_genAIBlockoutRoomMinSize = 256;
int g_genAIBlockoutRoomMaxSize = 512;
int g_genAIBlockoutRoomHeight = 192;
int g_genAIBlockoutCorridorWidth = 128;
int g_genAIBlockoutCorridorHeight = 128;
int g_genAIBlockoutRoomSpacing = 384;
int g_genAIBlockoutSeed = 0;
bool g_genAIBlockoutUseOpenAIPlanner = true;
bool g_genAIBlockoutUseSkyCeiling = false;
bool g_genAIBlockoutIdTech3Caulk = true;

struct PromptToBlockoutOptions
{
	QString prompt;
	QString shader;
	QString floorShader;
	QString wallShader;
	QString ceilingShader;
	QString skyShader;
	int roomCount = 6;
	int roomMinSize = 256;
	int roomMaxSize = 512;
	int roomHeight = 192;
	int corridorWidth = 128;
	int corridorHeight = 128;
	int roomSpacing = 384;
	int seed = 0;
	bool useOpenAIPlanner = true;
	bool useSkyCeiling = false;
	bool idTech3Caulk = true;
};

struct NamedRoom
{
	QString name;
	QString plannerRole;
	AABB bounds;
	bool defendable = false;
	bool isStart = false;
	bool isGoal = false;
	bool isSideRoute = false;
	bool isHub = false;
	bool isCombatBeat = false;
	bool isTransition = false;
	bool hasPlatform = false;
	bool movingPlatform = false;
	bool hasLiquid = false;
	bool useStairs = false;
	bool useRamp = false;
	int progressionRank = -1;
	int connectivity = 0;
};

enum class TraversalKind
{
	Corridor,
	Door,
	Stairs,
	Ramp,
	FuncPlat,
	JumpPad,
	Teleporter
};

struct RoomLink
{
	int from = -1;
	int to = -1;
	bool xFirst = true;
	bool chokePoint = false;
	float widthScale = 1.0f;
	TraversalKind traversal = TraversalKind::Corridor;
};

struct CorridorSegment
{
	AABB bounds;
	int linkIndex = -1;
	bool chokePoint = false;
};

struct BlockoutPlan
{
	std::vector<NamedRoom> rooms;
	std::vector<RoomLink> links;
	std::vector<CorridorSegment> corridors;
	bool usedOpenAI = false;
};

enum class BlockoutLayout
{
	Linear,
	Branching,
	Hub
};

enum class BlockoutPlayStyle
{
	Deathmatch,
	Campaign,
	Hybrid
};

float GenAI_safeGridSize(){
	const float grid = GetSnapGridSize();
	return grid > 0.0f ? grid : 1.0f;
}

float GenAI_clampf( float value, float minimum, float maximum ){
	return std::max( minimum, std::min( value, maximum ) );
}

int GenAI_clampi( int value, int minimum, int maximum ){
	return std::max( minimum, std::min( value, maximum ) );
}

float GenAI_snapToGrid( float value, float grid ){
	if ( grid <= 0.0f ) {
		return value;
	}
	return std::round( value / grid ) * grid;
}

float GenAI_snapDimension( float value, float grid, float minimum, float maximum ){
	float snapped = GenAI_snapToGrid( value, grid );
	snapped = GenAI_clampf( snapped, minimum, maximum );
	if ( snapped < grid * 2.0f ) {
		snapped = grid * 2.0f;
	}
	return snapped;
}

float GenAI_clampWorld( float value, float extent ){
	const float minimum = g_MinWorldCoord + extent + 1.0f;
	const float maximum = g_MaxWorldCoord - extent - 1.0f;
	if ( minimum > maximum ) {
		return value;
	}
	return GenAI_clampf( value, minimum, maximum );
}

QString GenAI_commonShader( const char* name ){
	if ( name == nullptr || name[0] == '\0' ) {
		return QString();
	}
	return QString::fromUtf8( GetCommonShader( name ).c_str() ).trimmed();
}

QString GenAI_pickDefaultShader( const QString& configured, std::initializer_list<const char*> commonNames, const QString& fallback ){
	const QString configuredTrimmed = configured.trimmed();
	if ( !configuredTrimmed.isEmpty() ) {
		return configuredTrimmed;
	}
	for ( const char* commonName : commonNames ) {
		const QString shader = GenAI_commonShader( commonName );
		if ( !shader.isEmpty() ) {
			return shader;
		}
	}
	return fallback.trimmed();
}

bool GenAI_isIdTech3Game(){
	if ( g_pGameDescription == nullptr ) {
		return false;
	}
	const QString brushTypes = QString::fromUtf8( g_pGameDescription->getKeyValue( "brushtypes" ) ).trimmed().toLower();
	if ( brushTypes == "quake3" ) {
		return true;
	}
	const QString gameType = QString::fromUtf8( g_pGameDescription->mGameType.c_str() ).trimmed().toLower();
	return gameType == "q3" || gameType == "wolf" || gameType == "et" || gameType == "nexuiz" || gameType == "xonotic";
}

QString GenAI_defaultBlockoutShader(){
	return GenAI_pickDefaultShader(
		QString::fromUtf8( g_genAIBlockoutShader.c_str() ),
		{ "notex", "caulk" },
		QStringLiteral( "textures/common/caulk" )
	);
}

QString GenAI_defaultFloorShader(){
	return GenAI_pickDefaultShader(
		QString::fromUtf8( g_genAIBlockoutFloorShader.c_str() ),
		{ "notex" },
		GenAI_defaultBlockoutShader()
	);
}

QString GenAI_defaultWallShader(){
	return GenAI_pickDefaultShader(
		QString::fromUtf8( g_genAIBlockoutWallShader.c_str() ),
		{ "notex" },
		GenAI_defaultBlockoutShader()
	);
}

QString GenAI_defaultCeilingShader(){
	return GenAI_pickDefaultShader(
		QString::fromUtf8( g_genAIBlockoutCeilingShader.c_str() ),
		{ "notex" },
		GenAI_defaultBlockoutShader()
	);
}

QString GenAI_defaultSkyShader(){
	return GenAI_pickDefaultShader(
		QString::fromUtf8( g_genAIBlockoutSkyShader.c_str() ),
		{ "sky", "skyportal" },
		GenAI_defaultCeilingShader()
	);
}

QString GenAI_defaultCaulkShader(){
	return GenAI_pickDefaultShader(
		QString(),
		{ "caulk" },
		QStringLiteral( "textures/common/caulk" )
	);
}

PromptToBlockoutOptions GenAI_defaultPromptToBlockoutOptions(){
	PromptToBlockoutOptions options;
	options.prompt = QString::fromUtf8( g_genAIBlockoutDefaultPrompt.c_str() ).trimmed();
	options.shader = QString::fromUtf8( g_genAIBlockoutShader.c_str() ).trimmed();
	options.floorShader = QString::fromUtf8( g_genAIBlockoutFloorShader.c_str() ).trimmed();
	options.wallShader = QString::fromUtf8( g_genAIBlockoutWallShader.c_str() ).trimmed();
	options.ceilingShader = QString::fromUtf8( g_genAIBlockoutCeilingShader.c_str() ).trimmed();
	options.skyShader = QString::fromUtf8( g_genAIBlockoutSkyShader.c_str() ).trimmed();
	options.roomCount = g_genAIBlockoutRoomCount;
	options.roomMinSize = g_genAIBlockoutRoomMinSize;
	options.roomMaxSize = g_genAIBlockoutRoomMaxSize;
	options.roomHeight = g_genAIBlockoutRoomHeight;
	options.corridorWidth = g_genAIBlockoutCorridorWidth;
	options.corridorHeight = g_genAIBlockoutCorridorHeight;
	options.roomSpacing = g_genAIBlockoutRoomSpacing;
	options.seed = g_genAIBlockoutSeed;
	options.useOpenAIPlanner = g_genAIBlockoutUseOpenAIPlanner;
	options.useSkyCeiling = g_genAIBlockoutUseSkyCeiling;
	options.idTech3Caulk = g_genAIBlockoutIdTech3Caulk;
	return options;
}

PromptToBlockoutOptions GenAI_sanitisePromptToBlockoutOptions( PromptToBlockoutOptions options ){
	options.roomCount = GenAI_clampi( options.roomCount, 2, 24 );
	options.roomMinSize = GenAI_clampi( options.roomMinSize, 64, 4096 );
	options.roomMaxSize = GenAI_clampi( options.roomMaxSize, options.roomMinSize, 8192 );
	options.roomHeight = GenAI_clampi( options.roomHeight, 64, 2048 );
	options.corridorWidth = GenAI_clampi( options.corridorWidth, 32, 1024 );
	options.corridorHeight = GenAI_clampi( options.corridorHeight, 32, 1024 );
	options.roomSpacing = GenAI_clampi( options.roomSpacing, 64, 4096 );
	options.seed = GenAI_clampi( options.seed, 0, 2147483647 );
	options.prompt = options.prompt.trimmed();
	options.shader = options.shader.trimmed();
	options.floorShader = options.floorShader.trimmed();
	options.wallShader = options.wallShader.trimmed();
	options.ceilingShader = options.ceilingShader.trimmed();
	options.skyShader = options.skyShader.trimmed();
	if ( options.prompt.isEmpty() ) {
		options.prompt = "Create a medium-sized arena with a central space and two flanking routes.";
	}
	if ( options.shader.isEmpty() ) {
		options.shader = GenAI_defaultBlockoutShader();
	}
	if ( options.floorShader.isEmpty() ) {
		options.floorShader = GenAI_defaultFloorShader();
	}
	if ( options.wallShader.isEmpty() ) {
		options.wallShader = GenAI_defaultWallShader();
	}
	if ( options.ceilingShader.isEmpty() ) {
		options.ceilingShader = GenAI_defaultCeilingShader();
	}
	if ( options.skyShader.isEmpty() ) {
		options.skyShader = GenAI_defaultSkyShader();
	}
	if ( options.useSkyCeiling && options.skyShader.isEmpty() ) {
		options.useSkyCeiling = false;
	}
	options.idTech3Caulk = options.idTech3Caulk && GenAI_isIdTech3Game();
	return options;
}

void GenAI_storePromptToBlockoutOptions( const PromptToBlockoutOptions& options ){
	g_genAIBlockoutDefaultPrompt = options.prompt.toUtf8().constData();
	g_genAIBlockoutShader = options.shader.toUtf8().constData();
	g_genAIBlockoutFloorShader = options.floorShader.toUtf8().constData();
	g_genAIBlockoutWallShader = options.wallShader.toUtf8().constData();
	g_genAIBlockoutCeilingShader = options.ceilingShader.toUtf8().constData();
	g_genAIBlockoutSkyShader = options.skyShader.toUtf8().constData();
	g_genAIBlockoutRoomCount = options.roomCount;
	g_genAIBlockoutRoomMinSize = options.roomMinSize;
	g_genAIBlockoutRoomMaxSize = options.roomMaxSize;
	g_genAIBlockoutRoomHeight = options.roomHeight;
	g_genAIBlockoutCorridorWidth = options.corridorWidth;
	g_genAIBlockoutCorridorHeight = options.corridorHeight;
	g_genAIBlockoutRoomSpacing = options.roomSpacing;
	g_genAIBlockoutSeed = options.seed;
	g_genAIBlockoutUseOpenAIPlanner = options.useOpenAIPlanner;
	g_genAIBlockoutUseSkyCeiling = options.useSkyCeiling;
	g_genAIBlockoutIdTech3Caulk = options.idTech3Caulk;
}

int GenAI_extractPromptRoomCount( const QString& prompt, int fallback ){
	const QString lower = prompt.toLower();
	for ( int i = 0; i < lower.size(); ++i ) {
		if ( !lower.at( i ).isDigit() ) {
			continue;
		}
		int value = 0;
		int j = i;
		while ( j < lower.size() && lower.at( j ).isDigit() ) {
			value = value * 10 + lower.at( j ).digitValue();
			++j;
		}
		if ( value <= 0 ) {
			i = j;
			continue;
		}
		const QString suffix = lower.mid( j, 16 );
		if ( suffix.contains( "room" ) || suffix.contains( "area" ) || suffix.contains( "zone" ) ) {
			return GenAI_clampi( value, 2, 24 );
		}
		i = j;
	}
	return fallback;
}

BlockoutLayout GenAI_pickLayout( const QString& prompt ){
	const QString lower = prompt.toLower();
	if ( lower.contains( "hub" ) || lower.contains( "arena" ) || lower.contains( "central" ) ) {
		return BlockoutLayout::Hub;
	}
	if ( lower.contains( "branch" ) || lower.contains( "flank" ) || lower.contains( "nonlinear" ) || lower.contains( "loop" ) ) {
		return BlockoutLayout::Branching;
	}
	return BlockoutLayout::Linear;
}

int GenAI_hashPromptSeed( const QString& prompt ){
	uint32_t hash = 2166136261u;
	const QByteArray bytes = prompt.toUtf8();
	for ( const char byte : bytes ) {
		hash ^= static_cast<unsigned char>( byte );
		hash *= 16777619u;
	}
	return static_cast<int>( hash & 0x7fffffff );
}

Vector3 GenAI_blockoutAnchor( float grid, float& floorZ ){
	Vector3 anchor( 0.0f, 0.0f, 0.0f );

	if ( g_pParentWnd != nullptr && g_pParentWnd->GetCamWnd() != nullptr ) {
		CamWnd& camwnd = *g_pParentWnd->GetCamWnd();
		anchor = Camera_getOrigin( camwnd );
		Vector3 view = Camera_getViewVector( camwnd );
		if ( std::isfinite( view.x() ) && std::isfinite( view.y() ) && std::isfinite( view.z() ) ) {
			const float viewLen2 = vector3_length_squared( view );
			if ( viewLen2 > 1e-4f ) {
				anchor += vector3_normalised( view ) * 384.0f;
			}
		}
	}

	vector3_snap( anchor, grid );
	floorZ = GenAI_snapToGrid( anchor.z() - 64.0f, grid );
	return anchor;
}

AABB GenAI_makeBlockoutBounds( const Vector3& center, float width, float depth, float height, float grid ){
	const float clampedWidth = GenAI_snapDimension( width, grid, 32.0f, g_MaxWorldCoord );
	const float clampedDepth = GenAI_snapDimension( depth, grid, 32.0f, g_MaxWorldCoord );
	const float clampedHeight = GenAI_snapDimension( height, grid, 32.0f, g_MaxWorldCoord );

	Vector3 extents( clampedWidth * 0.5f, clampedDepth * 0.5f, clampedHeight * 0.5f );
	Vector3 origin = center;
	origin.x() = GenAI_snapToGrid( origin.x(), grid );
	origin.y() = GenAI_snapToGrid( origin.y(), grid );
	origin.z() = GenAI_snapToGrid( origin.z(), grid );
	origin.x() = GenAI_clampWorld( origin.x(), extents.x() );
	origin.y() = GenAI_clampWorld( origin.y(), extents.y() );
	origin.z() = GenAI_clampWorld( origin.z(), extents.z() );

	return AABB( origin, extents );
}

void GenAI_addRoom( BlockoutPlan& plan, const QString& name, const Vector3& center, float width, float depth, float height, float grid ){
	NamedRoom room;
	room.name = name;
	room.plannerRole.clear();
	room.bounds = GenAI_makeBlockoutBounds( center, width, depth, height, grid );
	plan.rooms.push_back( room );
}

void GenAI_applyPlannerRoleHints( NamedRoom& room ){
	const QString role = room.plannerRole.trimmed().toLower();
	if ( role.isEmpty() ) {
		return;
	}

	if ( role == "start" || role == "entry" ) {
		room.isStart = true;
		room.isTransition = true;
	}
	else if ( role == "goal" || role == "boss" || role == "objective" ) {
		room.isGoal = true;
		room.isCombatBeat = true;
		room.defendable = true;
	}
	else if ( role == "hub" || role == "atrium" || role == "control" ) {
		room.isHub = true;
		room.defendable = true;
		room.hasPlatform = true;
	}
	else if ( role == "arena" || role == "combat" || role == "fight" ) {
		room.isCombatBeat = true;
		room.defendable = true;
	}
	else if ( role == "connector" || role == "transition" || role == "hall" ) {
		room.isTransition = true;
	}
	else if ( role == "secret" || role == "reward" || role == "sideroute" || role == "side" ) {
		room.isSideRoute = true;
	}
}

TraversalKind GenAI_parseTraversalKind( const QString& rawType ){
	const QString type = rawType.trimmed().toLower();
	if ( type == "door" ) {
		return TraversalKind::Door;
	}
	if ( type == "stairs" || type == "stair" ) {
		return TraversalKind::Stairs;
	}
	if ( type == "ramp" ) {
		return TraversalKind::Ramp;
	}
	if ( type == "func_plat" || type == "platform" || type == "plat" ) {
		return TraversalKind::FuncPlat;
	}
	if ( type == "jumppad" || type == "jump_pad" || type == "jump-pad" || type == "trigger_push" ) {
		return TraversalKind::JumpPad;
	}
	if ( type == "teleporter" || type == "teleport" || type == "trigger_teleport" ) {
		return TraversalKind::Teleporter;
	}
	return TraversalKind::Corridor;
}

const char* GenAI_traversalKindName( TraversalKind traversal ){
	switch ( traversal )
	{
	case TraversalKind::Door:
		return "door";
	case TraversalKind::Stairs:
		return "stairs";
	case TraversalKind::Ramp:
		return "ramp";
	case TraversalKind::FuncPlat:
		return "func_plat";
	case TraversalKind::JumpPad:
		return "jumppad";
	case TraversalKind::Teleporter:
		return "teleporter";
	case TraversalKind::Corridor:
	default:
		return "corridor";
	}
}

bool GenAI_linkUsesPhysicalCorridor( const RoomLink& link ){
	// Teleport links are represented by entities only.
	return link.traversal != TraversalKind::Teleporter;
}

void GenAI_addAxisCorridorSegment( std::vector<CorridorSegment>& corridors, const Vector3& from, const Vector3& to, float width, float height, float grid, int linkIndex, bool chokePoint ){
	const bool axisX = std::fabs( from.x() - to.x() ) >= std::fabs( from.y() - to.y() );
	Vector3 mins;
	Vector3 maxs;
	if ( axisX ) {
		mins = Vector3( std::min( from.x(), to.x() ), ( from.y() + to.y() ) * 0.5f - width * 0.5f, std::min( from.z(), to.z() ) - height * 0.5f );
		maxs = Vector3( std::max( from.x(), to.x() ), ( from.y() + to.y() ) * 0.5f + width * 0.5f, std::max( from.z(), to.z() ) + height * 0.5f );
	}
	else{
		mins = Vector3( ( from.x() + to.x() ) * 0.5f - width * 0.5f, std::min( from.y(), to.y() ), std::min( from.z(), to.z() ) - height * 0.5f );
		maxs = Vector3( ( from.x() + to.x() ) * 0.5f + width * 0.5f, std::max( from.y(), to.y() ), std::max( from.z(), to.z() ) + height * 0.5f );
	}

	Vector3 center = ( mins + maxs ) * 0.5f;
	Vector3 size = maxs - mins;
	if ( size.x() < grid ) {
		size.x() = grid;
	}
	if ( size.y() < grid ) {
		size.y() = grid;
	}
	if ( size.z() < grid ) {
		size.z() = grid;
	}
	CorridorSegment segment;
	segment.bounds = GenAI_makeBlockoutBounds( center, size.x(), size.y(), size.z(), grid );
	segment.linkIndex = linkIndex;
	segment.chokePoint = chokePoint;
	corridors.push_back( segment );
}

enum class BoundarySide
{
	West,
	East,
	South,
	North
};

struct CorridorRouteEndpoint
{
	BoundarySide side = BoundarySide::North;
	float along = 0.0f;
	Vector3 point = Vector3( 0.0f, 0.0f, 0.0f );
};

struct CorridorRoute
{
	CorridorRouteEndpoint from;
	CorridorRouteEndpoint to;
	bool hasCorner = false;
	Vector3 corner = Vector3( 0.0f, 0.0f, 0.0f );
	bool valid = false;
};

Vector3 GenAI_roomBoundaryPoint( const AABB& room, BoundarySide side, float along, float z, float grid ){
	const Vector3 mins = room.origin - room.extents;
	const Vector3 maxs = room.origin + room.extents;
	const float clampedZ = GenAI_clampf( z, mins.z() + grid * 0.5f, maxs.z() - grid * 0.5f );
	Vector3 point = room.origin;
	point.z() = clampedZ;

	if ( side == BoundarySide::West || side == BoundarySide::East ) {
		point.x() = ( side == BoundarySide::West ) ? mins.x() : maxs.x();
		point.y() = GenAI_clampf( along, mins.y() + grid, maxs.y() - grid );
	}
	else{
		point.y() = ( side == BoundarySide::South ) ? mins.y() : maxs.y();
		point.x() = GenAI_clampf( along, mins.x() + grid, maxs.x() - grid );
	}
	return point;
}

CorridorRoute GenAI_makeCorridorRoute( const AABB& from, const AABB& to, bool xFirst, float grid ){
	CorridorRoute route;
	const Vector3 fromCenter = from.origin;
	const Vector3 toCenter = to.origin;
	const float dx = toCenter.x() - fromCenter.x();
	const float dy = toCenter.y() - fromCenter.y();
	const float absDx = std::fabs( dx );
	const float absDy = std::fabs( dy );
	const float midZ = ( fromCenter.z() + toCenter.z() ) * 0.5f;

	if ( absDx < grid * 0.5f && absDy < grid * 0.5f ) {
		return route;
	}

	if ( absDy < grid * 0.5f ) {
		route.from.side = dx >= 0.0f ? BoundarySide::East : BoundarySide::West;
		route.to.side = dx >= 0.0f ? BoundarySide::West : BoundarySide::East;
		route.from.along = ( fromCenter.y() + toCenter.y() ) * 0.5f;
		route.to.along = route.from.along;
	}
	else if ( absDx < grid * 0.5f ) {
		route.from.side = dy >= 0.0f ? BoundarySide::North : BoundarySide::South;
		route.to.side = dy >= 0.0f ? BoundarySide::South : BoundarySide::North;
		route.from.along = ( fromCenter.x() + toCenter.x() ) * 0.5f;
		route.to.along = route.from.along;
	}
	else if ( xFirst ) {
		route.from.side = dx >= 0.0f ? BoundarySide::East : BoundarySide::West;
		route.to.side = dy >= 0.0f ? BoundarySide::South : BoundarySide::North;
		route.from.along = fromCenter.y();
		route.to.along = toCenter.x();
		route.hasCorner = true;
	}
	else{
		route.from.side = dy >= 0.0f ? BoundarySide::North : BoundarySide::South;
		route.to.side = dx >= 0.0f ? BoundarySide::West : BoundarySide::East;
		route.from.along = fromCenter.x();
		route.to.along = toCenter.y();
		route.hasCorner = true;
	}

	route.from.point = GenAI_roomBoundaryPoint( from, route.from.side, route.from.along, fromCenter.z(), grid );
	route.to.point = GenAI_roomBoundaryPoint( to, route.to.side, route.to.along, toCenter.z(), grid );
	if ( route.hasCorner ) {
		if ( xFirst ) {
			route.corner = Vector3( route.to.point.x(), route.from.point.y(), midZ );
		}
		else{
			route.corner = Vector3( route.from.point.x(), route.to.point.y(), midZ );
		}
	}
	route.valid = true;
	return route;
}

void GenAI_addCorridorBetweenRooms( std::vector<CorridorSegment>& corridors, const AABB& from, const AABB& to, float width, float height, float grid, bool xFirst, int linkIndex, bool chokePoint ){
	const CorridorRoute route = GenAI_makeCorridorRoute( from, to, xFirst, grid );
	if ( !route.valid ) {
		return;
	}

	if ( route.hasCorner ) {
		GenAI_addAxisCorridorSegment( corridors, route.from.point, route.corner, width, height, grid, linkIndex, chokePoint );
		GenAI_addAxisCorridorSegment( corridors, route.corner, route.to.point, width, height, grid, linkIndex, chokePoint );
	}
	else{
		GenAI_addAxisCorridorSegment( corridors, route.from.point, route.to.point, width, height, grid, linkIndex, chokePoint );
	}
}

void GenAI_addLink( BlockoutPlan& plan, int from, int to, bool xFirst, TraversalKind traversal = TraversalKind::Corridor, bool chokePoint = false, float widthScale = 1.0f ){
	if ( from == to ) {
		return;
	}
	if ( from < 0 || to < 0 || from >= static_cast<int>( plan.rooms.size() ) || to >= static_cast<int>( plan.rooms.size() ) ) {
		return;
	}
	const int a = std::min( from, to );
	const int b = std::max( from, to );
	for ( const RoomLink& existing : plan.links ) {
		const int ea = std::min( existing.from, existing.to );
		const int eb = std::max( existing.from, existing.to );
		if ( ea == a && eb == b ) {
			return;
		}
	}

	RoomLink link;
	link.from = from;
	link.to = to;
	link.xFirst = xFirst;
	link.traversal = traversal;
	link.chokePoint = chokePoint;
	link.widthScale = GenAI_clampf( widthScale, 0.45f, 1.4f );
	plan.links.push_back( link );
}

void GenAI_rebuildCorridorsFromLinks( BlockoutPlan& plan, float corridorWidth, float corridorHeight, float grid ){
	plan.corridors.clear();
	for ( std::size_t i = 0; i < plan.links.size(); ++i ) {
		const RoomLink& link = plan.links[i];
		if ( !GenAI_linkUsesPhysicalCorridor( link ) ) {
			continue;
		}
		const float width = GenAI_clampf( corridorWidth * link.widthScale, 32.0f, 1024.0f );
		GenAI_addCorridorBetweenRooms(
			plan.corridors,
			plan.rooms[link.from].bounds,
			plan.rooms[link.to].bounds,
			width,
			corridorHeight,
			grid,
			link.xFirst,
			static_cast<int>( i ),
			link.chokePoint
		);
	}
}

void GenAI_addSequentialConnections( BlockoutPlan& plan, float corridorWidth, float corridorHeight, float grid ){
	if ( plan.rooms.size() < 2 ) {
		return;
	}
	for ( std::size_t i = 1; i < plan.rooms.size(); ++i ) {
		GenAI_addLink( plan, static_cast<int>( i - 1 ), static_cast<int>( i ), i % 2 == 0 );
	}
	GenAI_rebuildCorridorsFromLinks( plan, corridorWidth, corridorHeight, grid );
}

void GenAI_generateHeuristicBlockoutPlan( const PromptToBlockoutOptions& options, const Vector3& anchor, float floorZ, float grid, BlockoutPlan& plan ){
	plan.rooms.clear();
	plan.links.clear();
	plan.corridors.clear();
	plan.usedOpenAI = false;

	const int requestedRoomCount = GenAI_extractPromptRoomCount( options.prompt, options.roomCount );
	const int roomCount = GenAI_clampi( requestedRoomCount, 2, 24 );
	const int minSize = std::min( options.roomMinSize, options.roomMaxSize );
	const int maxSize = std::max( options.roomMinSize, options.roomMaxSize );
	const float roomHeight = static_cast<float>( options.roomHeight );
	const float spacing = static_cast<float>( options.roomSpacing + maxSize );
	const float corridorWidth = static_cast<float>( options.corridorWidth );
	const float corridorHeight = static_cast<float>( options.corridorHeight );
	const BlockoutLayout layout = GenAI_pickLayout( options.prompt );

	const int seed = options.seed > 0 ? options.seed : GenAI_hashPromptSeed( options.prompt );
	std::mt19937 rng( static_cast<uint32_t>( seed ) );
	std::uniform_int_distribution<int> sizeDist( minSize, maxSize );
	std::uniform_real_distribution<float> jitterDist( -spacing * 0.2f, spacing * 0.2f );

	auto randomSize = [&](){
		return static_cast<float>( sizeDist( rng ) );
	};

	if ( layout == BlockoutLayout::Hub ) {
		GenAI_addRoom(
			plan,
			QStringLiteral( "hub" ),
			Vector3( anchor.x(), anchor.y(), floorZ + roomHeight * 0.5f ),
			static_cast<float>( maxSize ) * 1.4f,
			static_cast<float>( maxSize ) * 1.4f,
			roomHeight,
			grid
		);

		const float radius = spacing * 1.35f;
		const float twoPi = 6.28318530717958647692f;
		for ( int i = 1; i < roomCount; ++i ) {
			const float t = roomCount > 1 ? static_cast<float>( i - 1 ) / static_cast<float>( roomCount - 1 ) : 0.0f;
			const float angle = t * twoPi;
			const float x = anchor.x() + std::cos( angle ) * radius;
			const float y = anchor.y() + std::sin( angle ) * radius;
			GenAI_addRoom(
				plan,
				QStringLiteral( "room_%1" ).arg( i ),
				Vector3( x, y, floorZ + roomHeight * 0.5f ),
				randomSize(),
				randomSize(),
				roomHeight,
				grid
			);
		}

			for ( std::size_t i = 1; i < plan.rooms.size(); ++i ) {
				GenAI_addLink( plan, 0, static_cast<int>( i ), i % 2 == 0 );
			}
		}
		else if ( layout == BlockoutLayout::Branching ) {
		const int spineCount = std::max( 2, roomCount * 2 / 3 );
		for ( int i = 0; i < spineCount; ++i ) {
			const float x = anchor.x() + static_cast<float>( i ) * spacing;
			const float y = anchor.y() + jitterDist( rng ) * 0.35f;
			GenAI_addRoom(
				plan,
				QStringLiteral( "spine_%1" ).arg( i ),
				Vector3( x, y, floorZ + roomHeight * 0.5f ),
				randomSize(),
				randomSize(),
				roomHeight,
				grid
				);
				if ( i > 0 ) {
					GenAI_addLink( plan, i - 1, i, true );
				}
			}

		for ( int i = spineCount; i < roomCount; ++i ) {
			const int branchIndex = i - spineCount;
			const int parent = 1 + ( branchIndex % std::max( 1, spineCount - 1 ) );
			const float direction = ( branchIndex % 2 == 0 ) ? 1.0f : -1.0f;
			const Vector3 parentCenter = plan.rooms[parent].bounds.origin;
			const float x = parentCenter.x() + spacing * 0.35f;
			const float y = parentCenter.y() + direction * spacing;
			GenAI_addRoom(
				plan,
				QStringLiteral( "branch_%1" ).arg( branchIndex ),
				Vector3( x, y, floorZ + roomHeight * 0.5f ),
				randomSize(),
				randomSize(),
				roomHeight,
				grid
				);
				const std::size_t childIndex = plan.rooms.size() - 1;
				GenAI_addLink( plan, parent, static_cast<int>( childIndex ), branchIndex % 2 == 0 );
			}
		}
		else{
		for ( int i = 0; i < roomCount; ++i ) {
			const float x = anchor.x() + static_cast<float>( i ) * spacing;
			const float y = anchor.y() + jitterDist( rng );
			GenAI_addRoom(
				plan,
				QStringLiteral( "room_%1" ).arg( i ),
				Vector3( x, y, floorZ + roomHeight * 0.5f ),
				randomSize(),
				randomSize(),
				roomHeight,
				grid
				);
				if ( i > 0 ) {
					GenAI_addLink( plan, i - 1, i, i % 2 == 0 );
				}
			}
		}

	GenAI_rebuildCorridorsFromLinks( plan, corridorWidth, corridorHeight, grid );
}

bool GenAI_promptHasAnyTerm( const QString& prompt, std::initializer_list<const char*> terms ){
	const QString lowered = prompt.toLower();
	for ( const char* term : terms ) {
		if ( term != nullptr && lowered.contains( QString::fromLatin1( term ) ) ) {
			return true;
		}
	}
	return false;
}

BlockoutPlayStyle GenAI_detectPlayStyle( const QString& prompt ){
	const bool dmTerms = GenAI_promptHasAnyTerm(
		prompt,
		{ "deathmatch", "dm", "duel", "ffa", "arena", "tourney", "multiplayer", "frag" }
	);
	const bool campaignTerms = GenAI_promptHasAnyTerm(
		prompt,
		{ "campaign", "single player", "single-player", "episode", "mission", "objective", "boss", "co-op", "coop" }
	);
	if ( dmTerms && campaignTerms ) {
		return BlockoutPlayStyle::Hybrid;
	}
	if ( campaignTerms ) {
		return BlockoutPlayStyle::Campaign;
	}
	return BlockoutPlayStyle::Deathmatch;
}

bool GenAI_isCampaignStyle( BlockoutPlayStyle style ){
	return style == BlockoutPlayStyle::Campaign || style == BlockoutPlayStyle::Hybrid;
}

bool GenAI_isDeathmatchStyle( BlockoutPlayStyle style ){
	return style == BlockoutPlayStyle::Deathmatch || style == BlockoutPlayStyle::Hybrid;
}

const char* GenAI_playStyleName( BlockoutPlayStyle style ){
	switch ( style )
	{
	case BlockoutPlayStyle::Campaign:
		return "Campaign";
	case BlockoutPlayStyle::Hybrid:
		return "Hybrid";
	case BlockoutPlayStyle::Deathmatch:
	default:
		return "Deathmatch";
	}
}

float GenAI_roomDistanceXY( const AABB& a, const AABB& b ){
	const Vector3 delta = b.origin - a.origin;
	return std::sqrt( delta.x() * delta.x() + delta.y() * delta.y() );
}

std::vector<int> GenAI_computeRoomDegree( const BlockoutPlan& plan ){
	std::vector<int> degree( plan.rooms.size(), 0 );
	for ( const RoomLink& link : plan.links ) {
		if ( link.from >= 0 && link.from < static_cast<int>( degree.size() ) ) {
			++degree[link.from];
		}
		if ( link.to >= 0 && link.to < static_cast<int>( degree.size() ) ) {
			++degree[link.to];
		}
	}
	return degree;
}

bool GenAI_linkExists( const BlockoutPlan& plan, int a, int b ){
	if ( a < 0 || b < 0 || a >= static_cast<int>( plan.rooms.size() ) || b >= static_cast<int>( plan.rooms.size() ) || a == b ) {
		return true;
	}
	const int minIndex = std::min( a, b );
	const int maxIndex = std::max( a, b );
	for ( const RoomLink& link : plan.links ) {
		const int linkMin = std::min( link.from, link.to );
		const int linkMax = std::max( link.from, link.to );
		if ( linkMin == minIndex && linkMax == maxIndex ) {
			return true;
		}
	}
	return false;
}

bool GenAI_tryAddUniqueLink( BlockoutPlan& plan, int from, int to, bool xFirst, TraversalKind traversal, bool chokePoint, float widthScale ){
	const std::size_t before = plan.links.size();
	GenAI_addLink( plan, from, to, xFirst, traversal, chokePoint, widthScale );
	return plan.links.size() > before;
}

std::pair<int, int> GenAI_farthestRoomPair( const BlockoutPlan& plan ){
	std::pair<int, int> pair( -1, -1 );
	float bestDistance = -1.0f;
	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		for ( std::size_t j = i + 1; j < plan.rooms.size(); ++j ) {
			const float distance = GenAI_roomDistanceXY( plan.rooms[i].bounds, plan.rooms[j].bounds );
			if ( distance > bestDistance ) {
				bestDistance = distance;
				pair = std::make_pair( static_cast<int>( i ), static_cast<int>( j ) );
			}
		}
	}
	return pair;
}

std::vector<int> GenAI_roomProgressionOrder( const BlockoutPlan& plan ){
	std::vector<int> order;
	order.reserve( plan.rooms.size() );
	if ( plan.rooms.empty() ) {
		return order;
	}

	float minX = std::numeric_limits<float>::max();
	float maxX = std::numeric_limits<float>::lowest();
	float minY = std::numeric_limits<float>::max();
	float maxY = std::numeric_limits<float>::lowest();
	for ( const NamedRoom& room : plan.rooms ) {
		minX = std::min( minX, room.bounds.origin.x() );
		maxX = std::max( maxX, room.bounds.origin.x() );
		minY = std::min( minY, room.bounds.origin.y() );
		maxY = std::max( maxY, room.bounds.origin.y() );
	}
	const bool axisX = ( maxX - minX ) >= ( maxY - minY );

	std::vector<std::pair<float, int>> projection;
	projection.reserve( plan.rooms.size() );
	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		const float value = axisX ? plan.rooms[i].bounds.origin.x() : plan.rooms[i].bounds.origin.y();
		projection.push_back( std::make_pair( value, static_cast<int>( i ) ) );
	}
	std::sort( projection.begin(), projection.end(), []( const auto& a, const auto& b ){ return a.first < b.first; } );
	for ( const auto& [ value, index ] : projection ) {
		( void )value;
		order.push_back( index );
	}
	return order;
}

std::vector<std::vector<int>> GenAI_buildRoomAdjacency( const BlockoutPlan& plan ){
	std::vector<std::vector<int>> adjacency( plan.rooms.size() );
	for ( const RoomLink& link : plan.links ) {
		if ( link.from < 0 || link.to < 0
		  || link.from >= static_cast<int>( adjacency.size() )
		  || link.to >= static_cast<int>( adjacency.size() ) ) {
			continue;
		}
		adjacency[link.from].push_back( link.to );
		adjacency[link.to].push_back( link.from );
	}
	return adjacency;
}

int GenAI_farthestReachableRoom( const std::vector<std::vector<int>>& adjacency, int start, std::vector<int>* parent, std::vector<int>* distance ){
	if ( start < 0 || start >= static_cast<int>( adjacency.size() ) ) {
		return -1;
	}

	std::vector<int> dist( adjacency.size(), -1 );
	std::vector<int> bfsParent( adjacency.size(), -1 );
	std::queue<int> queue;
	queue.push( start );
	dist[start] = 0;

	while ( !queue.empty() ) {
		const int room = queue.front();
		queue.pop();
		for ( const int neighbor : adjacency[room] ) {
			if ( neighbor < 0 || neighbor >= static_cast<int>( adjacency.size() ) ) {
				continue;
			}
			if ( dist[neighbor] >= 0 ) {
				continue;
			}
			dist[neighbor] = dist[room] + 1;
			bfsParent[neighbor] = room;
			queue.push( neighbor );
		}
	}

	int farthest = start;
	for ( std::size_t i = 0; i < dist.size(); ++i ) {
		if ( dist[i] > dist[farthest] ) {
			farthest = static_cast<int>( i );
		}
	}

	if ( parent != nullptr ) {
		*parent = std::move( bfsParent );
	}
	if ( distance != nullptr ) {
		*distance = std::move( dist );
	}
	return farthest;
}

std::vector<int> GenAI_mainProgressionPath( const BlockoutPlan& plan ){
	if ( plan.rooms.empty() ) {
		return {};
	}

	const std::vector<std::vector<int>> adjacency = GenAI_buildRoomAdjacency( plan );
	if ( adjacency.empty() ) {
		return GenAI_roomProgressionOrder( plan );
	}
	const int farA = GenAI_farthestReachableRoom( adjacency, 0, nullptr, nullptr );
	if ( farA < 0 ) {
		return GenAI_roomProgressionOrder( plan );
	}

	std::vector<int> parent;
	std::vector<int> distance;
	const int farB = GenAI_farthestReachableRoom( adjacency, farA, &parent, &distance );
	if ( farB < 0 || farB >= static_cast<int>( parent.size() ) ) {
		return GenAI_roomProgressionOrder( plan );
	}

	std::vector<int> path;
	for ( int current = farB; current >= 0; current = parent[current] ) {
		path.push_back( current );
		if ( current == farA ) {
			break;
		}
	}
	std::reverse( path.begin(), path.end() );
	if ( path.size() < 2 ) {
		return GenAI_roomProgressionOrder( plan );
	}
	return path;
}

using RoomPairSet = std::set<std::pair<int, int>>;

RoomPairSet GenAI_buildProgressionEdgeSet( const std::vector<int>& order ){
	RoomPairSet edges;
	for ( std::size_t i = 1; i < order.size(); ++i ) {
		const int a = std::min( order[i - 1], order[i] );
		const int b = std::max( order[i - 1], order[i] );
		edges.insert( std::make_pair( a, b ) );
	}
	return edges;
}

bool GenAI_isProgressionEdge( const RoomPairSet& edges, int a, int b ){
	const int minIndex = std::min( a, b );
	const int maxIndex = std::max( a, b );
	return edges.find( std::make_pair( minIndex, maxIndex ) ) != edges.end();
}

void GenAI_addDeathmatchLoopLinks( BlockoutPlan& plan, int desiredExtraLinks ){
	if ( plan.rooms.size() < 3 || desiredExtraLinks <= 0 ) {
		return;
	}

	int addedLinks = 0;
	std::vector<int> degree = GenAI_computeRoomDegree( plan );

	for ( std::size_t i = 0; i < plan.rooms.size() && addedLinks < desiredExtraLinks; ++i ) {
		if ( degree[i] > 1 ) {
			continue;
		}
		int bestTarget = -1;
		float bestDistance = std::numeric_limits<float>::max();
		for ( std::size_t j = 0; j < plan.rooms.size(); ++j ) {
			if ( i == j || GenAI_linkExists( plan, static_cast<int>( i ), static_cast<int>( j ) ) ) {
				continue;
			}
			const float distance = GenAI_roomDistanceXY( plan.rooms[i].bounds, plan.rooms[j].bounds );
			if ( distance < bestDistance ) {
				bestDistance = distance;
				bestTarget = static_cast<int>( j );
			}
		}
		if ( bestTarget >= 0 && GenAI_tryAddUniqueLink( plan, static_cast<int>( i ), bestTarget, static_cast<int>( i + bestTarget ) % 2 == 0, TraversalKind::Corridor, false, 1.0f ) ) {
			++addedLinks;
			++degree[i];
			++degree[bestTarget];
		}
	}

	while ( addedLinks < desiredExtraLinks ) {
		std::pair<int, int> bestPair( -1, -1 );
		float bestDistance = -1.0f;
		for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
			for ( std::size_t j = i + 1; j < plan.rooms.size(); ++j ) {
				if ( GenAI_linkExists( plan, static_cast<int>( i ), static_cast<int>( j ) ) ) {
					continue;
				}
				const float distance = GenAI_roomDistanceXY( plan.rooms[i].bounds, plan.rooms[j].bounds );
				if ( distance > bestDistance ) {
					bestDistance = distance;
					bestPair = std::make_pair( static_cast<int>( i ), static_cast<int>( j ) );
				}
			}
		}
		if ( bestPair.first < 0 || bestPair.second < 0 ) {
			break;
		}
		if ( GenAI_tryAddUniqueLink(
			plan,
			bestPair.first,
			bestPair.second,
			addedLinks % 2 == 0,
			addedLinks % 3 == 0 ? TraversalKind::Door : TraversalKind::Corridor,
			false,
			0.95f ) ) {
			++addedLinks;
		}
		else{
			break;
		}
	}
}

float GenAI_linkDistanceXY( const BlockoutPlan& plan, const RoomLink& link ){
	if ( link.from < 0 || link.to < 0
	  || link.from >= static_cast<int>( plan.rooms.size() )
	  || link.to >= static_cast<int>( plan.rooms.size() ) ) {
		return 0.0f;
	}
	const Vector3 delta = plan.rooms[link.to].bounds.origin - plan.rooms[link.from].bounds.origin;
	return std::sqrt( delta.x() * delta.x() + delta.y() * delta.y() );
}

void GenAI_applyGameplaySemantics( BlockoutPlan& plan, const PromptToBlockoutOptions& options, float grid ){
	if ( plan.rooms.empty() ) {
		return;
	}

	const BlockoutPlayStyle playStyle = GenAI_detectPlayStyle( options.prompt );
	const bool campaignStyle = GenAI_isCampaignStyle( playStyle );
	const bool deathmatchStyle = GenAI_isDeathmatchStyle( playStyle );

	for ( NamedRoom& room : plan.rooms ) {
		room.defendable = false;
		room.isStart = false;
		room.isGoal = false;
		room.isSideRoute = false;
		room.isHub = false;
		room.isCombatBeat = false;
		room.isTransition = false;
		room.hasPlatform = false;
		room.movingPlatform = false;
		room.hasLiquid = false;
		room.useStairs = false;
		room.useRamp = false;
		room.progressionRank = -1;
		room.connectivity = 0;
		GenAI_applyPlannerRoleHints( room );
	}

	const bool explicitDoors = GenAI_promptHasAnyTerm( options.prompt, { "door", "choke", "defend", "lockdown" } );
	const bool explicitStairs = GenAI_promptHasAnyTerm( options.prompt, { "stairs", "stair" } );
	const bool explicitRamps = GenAI_promptHasAnyTerm( options.prompt, { "ramp", "incline" } );
	const bool explicitPlatforms = GenAI_promptHasAnyTerm( options.prompt, { "platform", "catwalk", "high ground", "elevation" } );
	const bool explicitFuncPlat = GenAI_promptHasAnyTerm( options.prompt, { "func_plat", "moving platform", "lift", "elevator" } );
	const bool explicitTeleporter = GenAI_promptHasAnyTerm( options.prompt, { "teleporter", "teleport", "portal" } );
	const bool explicitJumpPad = GenAI_promptHasAnyTerm( options.prompt, { "jumppad", "jump pad", "trigger_push", "bounce" } );
	const bool wantsLiquid = GenAI_promptHasAnyTerm( options.prompt, { "liquid", "water", "slime", "lava", "pool" } );
	const bool wantsDoors = explicitDoors || campaignStyle;
	const bool wantsStairs = explicitStairs || campaignStyle;
	const bool wantsRamps = explicitRamps || campaignStyle;
	const bool wantsPlatforms = explicitPlatforms || deathmatchStyle;
	const bool wantsFuncPlat = explicitFuncPlat || ( campaignStyle && plan.rooms.size() >= 8 );
	const bool wantsTeleporter = explicitTeleporter;
	const bool wantsJumpPad = explicitJumpPad || ( deathmatchStyle && plan.rooms.size() >= 6 );
	const std::vector<int> layoutProgressionOrder = GenAI_roomProgressionOrder( plan );

	if ( deathmatchStyle ) {
		// Competitive DM flow needs loops and very few dead ends.
		GenAI_addDeathmatchLoopLinks( plan, GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 3, 1, 4 ) );
	}
	if ( campaignStyle ) {
		// Campaign layouts should have a readable forward path with a few optional detours.
		for ( std::size_t i = 1; i < layoutProgressionOrder.size(); ++i ) {
			GenAI_tryAddUniqueLink( plan, layoutProgressionOrder[i - 1], layoutProgressionOrder[i], i % 2 == 0, TraversalKind::Corridor, false, 1.0f );
		}
		for ( std::size_t i = 1; i + 2 < layoutProgressionOrder.size(); i += 2 ) {
			if ( GenAI_tryAddUniqueLink( plan, layoutProgressionOrder[i], layoutProgressionOrder[i + 2], i % 2 != 0, TraversalKind::Door, true, 0.84f ) ) {
				break;
			}
		}
	}

	const std::vector<int> progressionOrder = campaignStyle ? GenAI_mainProgressionPath( plan ) : std::vector<int>();
	const RoomPairSet progressionEdges = GenAI_buildProgressionEdgeSet( progressionOrder );

	std::vector<int> degree = GenAI_computeRoomDegree( plan );
	for ( std::size_t i = 0; i < plan.rooms.size() && i < degree.size(); ++i ) {
		plan.rooms[i].connectivity = degree[i];
	}
	if ( campaignStyle && !progressionOrder.empty() ) {
		for ( std::size_t i = 0; i < progressionOrder.size(); ++i ) {
			const int roomIndex = progressionOrder[i];
			if ( roomIndex < 0 || roomIndex >= static_cast<int>( plan.rooms.size() ) ) {
				continue;
			}
			NamedRoom& room = plan.rooms[roomIndex];
			room.progressionRank = static_cast<int>( i );
			if ( i % 2 == 0 ) {
				room.isCombatBeat = true;
			}
			else{
				room.isTransition = true;
			}
			if ( i == 0 ) {
				room.isStart = true;
				room.isTransition = true;
				room.isCombatBeat = false;
			}
			if ( i + 1 == progressionOrder.size() ) {
				room.isGoal = true;
				room.isCombatBeat = true;
			}
		}
	}

	std::vector<std::pair<float, int>> roomScore;
	roomScore.reserve( plan.rooms.size() );
	std::vector<float> campaignWeight( plan.rooms.size(), 0.0f );
	if ( campaignStyle && !progressionOrder.empty() ) {
		for ( std::size_t i = 0; i < progressionOrder.size(); ++i ) {
			const int roomIndex = progressionOrder[i];
			const float t = static_cast<float>( i ) / static_cast<float>( std::max<std::size_t>( 1, progressionOrder.size() - 1 ) );
			campaignWeight[roomIndex] = t * 220000.0f;
		}
	}
	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		const AABB& bounds = plan.rooms[i].bounds;
		const float area = bounds.extents.x() * bounds.extents.y() * 4.0f;
		const float score = area + static_cast<float>( degree[i] ) * 120000.0f + campaignWeight[i];
		roomScore.push_back( std::make_pair( score, static_cast<int>( i ) ) );
	}
	std::sort( roomScore.begin(), roomScore.end(), []( const auto& a, const auto& b ){ return a.first > b.first; } );

	const int defendableCount = deathmatchStyle
		? GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 3, 2, 4 )
		: GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 4, 1, 2 );
	for ( int i = 0; i < defendableCount && i < static_cast<int>( roomScore.size() ); ++i ) {
		plan.rooms[roomScore[i].second].defendable = true;
	}
	if ( deathmatchStyle ) {
		const auto [ roomA, roomB ] = GenAI_farthestRoomPair( plan );
		if ( roomA >= 0 ) {
			plan.rooms[roomA].defendable = true;
		}
		if ( roomB >= 0 ) {
			plan.rooms[roomB].defendable = true;
		}
	}
	if ( deathmatchStyle && !roomScore.empty() ) {
		// Quake DM maps benefit from a readable contested hub with several approach routes.
		NamedRoom& hub = plan.rooms[roomScore.front().second];
		hub.isHub = true;
		hub.defendable = true;
		hub.hasPlatform = true;
		hub.useStairs = true;
		hub.useRamp = true;

		std::vector<int> controlLoop;
		const int loopCount = GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 3, 3, 5 );
		for ( int i = 0; i < loopCount && i < static_cast<int>( roomScore.size() ); ++i ) {
			controlLoop.push_back( roomScore[i].second );
		}
		for ( std::size_t i = 1; i < controlLoop.size(); ++i ) {
			GenAI_tryAddUniqueLink( plan, controlLoop[i - 1], controlLoop[i], i % 2 == 0, TraversalKind::Corridor, false, 1.02f );
		}
		if ( controlLoop.size() >= 3 ) {
			GenAI_tryAddUniqueLink( plan, controlLoop.back(), controlLoop.front(), true, TraversalKind::Corridor, false, 1.0f );
		}
	}

	std::vector<std::pair<float, int>> linkDistance;
	linkDistance.reserve( plan.links.size() );
	for ( std::size_t i = 0; i < plan.links.size(); ++i ) {
		linkDistance.push_back( std::make_pair( GenAI_linkDistanceXY( plan, plan.links[i] ), static_cast<int>( i ) ) );
	}
	std::sort( linkDistance.begin(), linkDistance.end(), []( const auto& a, const auto& b ){ return a.first > b.first; } );

	if ( plan.links.size() >= 2 ) {
		const int chokeCount = campaignStyle
			? GenAI_clampi( static_cast<int>( plan.links.size() ) / 3, 1, 3 )
			: GenAI_clampi( static_cast<int>( plan.links.size() ) / 4, 1, 2 );
		for ( int i = 0; i < chokeCount && i < static_cast<int>( linkDistance.size() ); ++i ) {
			RoomLink& link = plan.links[linkDistance[i].second];
			link.chokePoint = true;
			link.widthScale = std::min( link.widthScale, campaignStyle ? 0.72f : 0.78f );
			if ( link.traversal == TraversalKind::Corridor && wantsDoors ) {
				link.traversal = TraversalKind::Door;
			}
		}
	}

	bool stairToggle = true;
	for ( RoomLink& link : plan.links ) {
		const float dz = std::fabs( plan.rooms[link.from].bounds.origin.z() - plan.rooms[link.to].bounds.origin.z() );
		if ( dz > std::max( options.roomHeight * 0.33f, 64.0f ) ) {
			if ( link.traversal == TraversalKind::Corridor || link.traversal == TraversalKind::Door ) {
				link.traversal = stairToggle ? TraversalKind::Stairs : TraversalKind::Ramp;
				stairToggle = !stairToggle;
			}
		}
		if ( campaignStyle && ( link.traversal == TraversalKind::Corridor ) ) {
			if ( ( &link - plan.links.data() ) % 3 == 0 ) {
				link.traversal = TraversalKind::Door;
			}
		}
	}

	if ( wantsTeleporter && !plan.links.empty() ) {
		bool alreadyHasTeleporter = false;
		for ( const RoomLink& link : plan.links ) {
			if ( link.traversal == TraversalKind::Teleporter ) {
				alreadyHasTeleporter = true;
				break;
			}
		}
		if ( !alreadyHasTeleporter ) {
			plan.links[linkDistance.empty() ? 0 : linkDistance.front().second].traversal = TraversalKind::Teleporter;
		}
	}

	if ( wantsJumpPad && plan.links.size() >= 2 ) {
		for ( RoomLink& link : plan.links ) {
			if ( link.traversal == TraversalKind::Corridor ) {
				link.traversal = TraversalKind::JumpPad;
				break;
			}
		}
	}
	else if ( deathmatchStyle && !plan.links.empty() ) {
		// Add one skill-route traversal in larger DM layouts.
		bool hasSkillTraversal = false;
		for ( const RoomLink& link : plan.links ) {
			if ( link.traversal == TraversalKind::JumpPad || link.traversal == TraversalKind::Teleporter ) {
				hasSkillTraversal = true;
				break;
			}
		}
		if ( !hasSkillTraversal && plan.links.size() >= 5 && !linkDistance.empty() ) {
			RoomLink& link = plan.links[linkDistance.front().second];
			if ( link.traversal == TraversalKind::Corridor || link.traversal == TraversalKind::Door ) {
				link.traversal = TraversalKind::JumpPad;
			}
		}
	}

	int platformBudget = campaignStyle
		? std::max( 1, static_cast<int>( plan.rooms.size() ) / 4 )
		: std::max( 1, static_cast<int>( plan.rooms.size() ) / 3 );
	if ( wantsPlatforms ) {
		platformBudget = std::max( platformBudget, 2 );
	}
	for ( const auto& [ score, index ] : roomScore ) {
		if ( platformBudget <= 0 ) {
			break;
		}
		NamedRoom& room = plan.rooms[index];
		if ( room.defendable || wantsPlatforms ) {
			room.hasPlatform = true;
			room.useStairs = wantsStairs || platformBudget % 2 == 0;
			room.useRamp = wantsRamps || !room.useStairs;
			if ( wantsFuncPlat ) {
				room.movingPlatform = ( platformBudget % 2 == 1 );
			}
			--platformBudget;
		}
	}

	if ( wantsLiquid || plan.rooms.size() >= 6 ) {
		int liquidBudget = wantsLiquid
			? std::max( 1, static_cast<int>( plan.rooms.size() ) / 4 )
			: ( campaignStyle ? 0 : 1 );
		for ( int i = static_cast<int>( roomScore.size() ) - 1; i >= 0 && liquidBudget > 0; --i ) {
			NamedRoom& room = plan.rooms[roomScore[i].second];
			if ( room.defendable ) {
				continue;
			}
			room.hasLiquid = true;
			--liquidBudget;
		}
	}

	for ( RoomLink& link : plan.links ) {
		const std::ptrdiff_t linkIndex = &link - plan.links.data();
		const bool progressionEdge = GenAI_isProgressionEdge( progressionEdges, link.from, link.to );
		if ( campaignStyle ) {
			if ( progressionEdge ) {
				link.widthScale = std::max( link.widthScale, 0.95f );
			}
			else{
				link.widthScale = std::min( link.widthScale, 0.85f );
				link.chokePoint = true;
				if ( link.traversal == TraversalKind::Corridor ) {
					link.traversal = TraversalKind::Door;
				}
			}
		}
		else{
			if ( !link.chokePoint ) {
				link.widthScale = std::max( link.widthScale, 0.9f );
			}
		}

		if ( link.traversal == TraversalKind::Corridor && link.chokePoint && wantsDoors ) {
			link.traversal = TraversalKind::Door;
		}
		if ( link.traversal == TraversalKind::Corridor && ( wantsStairs || wantsRamps ) && linkDistance.size() > 2 ) {
			if ( linkIndex % 4 == 0 ) {
				link.traversal = wantsStairs ? TraversalKind::Stairs : TraversalKind::Ramp;
			}
		}
		if ( link.traversal == TraversalKind::Corridor && wantsFuncPlat ) {
			if ( linkIndex % 5 == 0 ) {
				link.traversal = TraversalKind::FuncPlat;
			}
		}
		if ( campaignStyle && !explicitTeleporter && link.traversal == TraversalKind::Teleporter ) {
			link.traversal = TraversalKind::Door;
		}
		if ( campaignStyle && !explicitJumpPad && link.traversal == TraversalKind::JumpPad ) {
			link.traversal = wantsStairs ? TraversalKind::Stairs : TraversalKind::Ramp;
		}
		if ( link.widthScale < 0.7f ) {
			link.chokePoint = true;
		}
	}

	degree = GenAI_computeRoomDegree( plan );
	for ( std::size_t i = 0; i < plan.rooms.size() && i < degree.size(); ++i ) {
		plan.rooms[i].connectivity = degree[i];
	}
	if ( deathmatchStyle ) {
		for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
			if ( !plan.rooms[i].defendable || plan.rooms[i].connectivity >= 3 ) {
				continue;
			}
			int bestTarget = -1;
			float bestDistance = std::numeric_limits<float>::max();
			for ( std::size_t j = 0; j < plan.rooms.size(); ++j ) {
				if ( i == j || GenAI_linkExists( plan, static_cast<int>( i ), static_cast<int>( j ) ) ) {
					continue;
				}
				const float distance = GenAI_roomDistanceXY( plan.rooms[i].bounds, plan.rooms[j].bounds );
				if ( distance < bestDistance ) {
					bestDistance = distance;
					bestTarget = static_cast<int>( j );
				}
			}
			if ( bestTarget >= 0 ) {
				GenAI_tryAddUniqueLink( plan, static_cast<int>( i ), bestTarget, i % 2 == 0, TraversalKind::Corridor, false, 1.0f );
			}
		}

		degree = GenAI_computeRoomDegree( plan );
		for ( std::size_t i = 0; i < plan.rooms.size() && i < degree.size(); ++i ) {
			plan.rooms[i].connectivity = degree[i];
			if ( plan.rooms[i].defendable && degree[i] >= 3 ) {
				plan.rooms[i].isCombatBeat = true;
			}
		}
	}

	if ( campaignStyle ) {
		std::vector<int> sideRouteLinks( plan.rooms.size(), 0 );
		for ( const RoomLink& link : plan.links ) {
			if ( !GenAI_isProgressionEdge( progressionEdges, link.from, link.to ) ) {
				if ( link.from >= 0 && link.from < static_cast<int>( sideRouteLinks.size() ) ) {
					++sideRouteLinks[link.from];
				}
				if ( link.to >= 0 && link.to < static_cast<int>( sideRouteLinks.size() ) ) {
					++sideRouteLinks[link.to];
				}
			}
		}

		for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
			NamedRoom& room = plan.rooms[i];
			if ( room.isGoal ) {
				room.defendable = true;
				room.hasPlatform = true;
				room.useStairs = true;
				room.useRamp = false;
				room.isCombatBeat = true;
				room.isTransition = false;
			}
			if ( room.isStart ) {
				room.hasLiquid = false;
				continue;
			}
			if ( !room.isGoal && ( sideRouteLinks[i] > 0 || room.connectivity <= 1 ) ) {
				room.isSideRoute = true;
				if ( !room.isCombatBeat ) {
					room.isTransition = true;
				}
			}
			if ( room.isSideRoute ) {
				room.hasPlatform = room.hasPlatform || ( room.progressionRank % 2 == 0 );
				room.hasLiquid = room.hasLiquid && room.progressionRank > 2;
			}
		}
	}

	GenAI_rebuildCorridorsFromLinks(
		plan,
		static_cast<float>( options.corridorWidth ),
		static_cast<float>( options.corridorHeight ),
		grid
	);
}

Vector3 GenAI_boundsMins( const AABB& bounds ){
	return bounds.origin - bounds.extents;
}

Vector3 GenAI_boundsMaxs( const AABB& bounds ){
	return bounds.origin + bounds.extents;
}

AABB GenAI_boundsFromMinMax( const Vector3& mins, const Vector3& maxs, float grid ){
	const Vector3 center = ( mins + maxs ) * 0.5f;
	const Vector3 size = maxs - mins;
	return GenAI_makeBlockoutBounds( center, size.x(), size.y(), size.z(), grid );
}

EntityClass* GenAI_findKnownEntityClass( const char* classname, bool hasBrushes ){
	if ( classname == nullptr || classname[0] == '\0' ) {
		return nullptr;
	}
	EntityClass* entityClass = GlobalEntityClassManager().findOrInsert( classname, hasBrushes );
	if ( entityClass == nullptr || entityClass->unknown ) {
		return nullptr;
	}
	return entityClass;
}

const char* GenAI_pickKnownClass( std::initializer_list<const char*> classnames, bool hasBrushes, bool fixedsize ){
	for ( const char* classname : classnames ) {
		EntityClass* entityClass = GenAI_findKnownEntityClass( classname, hasBrushes );
		if ( entityClass == nullptr ) {
			continue;
		}
		if ( entityClass->fixedsize == fixedsize ) {
			return classname;
		}
	}
	return nullptr;
}

bool GenAI_emitBrush( const AABB& bounds, const QByteArray& shaderUtf8, std::size_t brushLimit, std::vector<scene::Node*>& createdBrushes ){
	if ( createdBrushes.size() >= brushLimit ) {
		return false;
	}
	if ( scene::Node* node = Scene_BrushCreate_Cuboid( bounds, shaderUtf8.constData() ) ) {
		createdBrushes.push_back( node );
		return true;
	}
	return false;
}

bool GenAI_emitBrushFromMinMax( const Vector3& mins, const Vector3& maxs, float grid, const QByteArray& shaderUtf8, std::size_t brushLimit, std::vector<scene::Node*>& createdBrushes ){
	const Vector3 size = maxs - mins;
	if ( size.x() < grid * 0.75f || size.y() < grid * 0.75f || size.z() < grid * 0.75f ) {
		return false;
	}
	return GenAI_emitBrush( GenAI_boundsFromMinMax( mins, maxs, grid ), shaderUtf8, brushLimit, createdBrushes );
}

using GenAIKeyValues = std::vector<std::pair<const char*, QString>>;

scene::Node* GenAI_createPointEntity( const char* classname, const Vector3& origin, const GenAIKeyValues& keyValues, std::vector<scene::Node*>& createdEntities ){
	EntityClass* entityClass = GenAI_findKnownEntityClass( classname, false );
	if ( entityClass == nullptr || !entityClass->fixedsize ) {
		return nullptr;
	}

	scene::Traversable* root = Node_getTraversable( GlobalSceneGraph().root() );
	if ( root == nullptr ) {
		return nullptr;
	}

	NodeSmartReference node( GlobalEntityCreator().createEntity( entityClass ) );
	root->insert( node );

	Entity* entity = Node_getEntity( node.get() );
	if ( entity == nullptr ) {
		root->erase( node.get() );
		return nullptr;
	}

	const QString originString = QStringLiteral( "%1 %2 %3" )
		.arg( origin.x(), 0, 'f', 2 )
		.arg( origin.y(), 0, 'f', 2 )
		.arg( origin.z(), 0, 'f', 2 );
	const QByteArray originUtf8 = originString.toUtf8();
	entity->setKeyValue( "origin", originUtf8.constData() );
	for ( const auto& [ key, value ] : keyValues ) {
		if ( key == nullptr || key[0] == '\0' ) {
			continue;
		}
		const QByteArray utf8 = value.toUtf8();
		entity->setKeyValue( key, utf8.constData() );
	}

	createdEntities.push_back( node.get_pointer() );
	return node.get_pointer();
}

scene::Node* GenAI_createSingleBrushEntity( const char* classname, const AABB& brushBounds, const QByteArray& shaderUtf8, const GenAIKeyValues& keyValues, std::size_t brushLimit, std::vector<scene::Node*>& createdEntities, std::vector<scene::Node*>& createdBrushes ){
	EntityClass* entityClass = GenAI_findKnownEntityClass( classname, true );
	if ( entityClass == nullptr || entityClass->fixedsize ) {
		return nullptr;
	}
	if ( createdBrushes.size() >= brushLimit ) {
		return nullptr;
	}

	scene::Traversable* root = Node_getTraversable( GlobalSceneGraph().root() );
	scene::Traversable* worldTraversable = Node_getTraversable( Map_FindOrInsertWorldspawn( g_map ) );
	if ( root == nullptr || worldTraversable == nullptr ) {
		return nullptr;
	}

	NodeSmartReference entityNode( GlobalEntityCreator().createEntity( entityClass ) );
	root->insert( entityNode );

	Entity* entity = Node_getEntity( entityNode.get() );
	scene::Traversable* entityTraversable = Node_getTraversable( entityNode.get() );
	if ( entity == nullptr || entityTraversable == nullptr ) {
		root->erase( entityNode.get() );
		return nullptr;
	}

	for ( const auto& [ key, value ] : keyValues ) {
		if ( key == nullptr || key[0] == '\0' ) {
			continue;
		}
		const QByteArray utf8 = value.toUtf8();
		entity->setKeyValue( key, utf8.constData() );
	}

	scene::Node* brushNode = Scene_BrushCreate_Cuboid( brushBounds, shaderUtf8.constData() );
	if ( brushNode == nullptr ) {
		root->erase( entityNode.get() );
		return nullptr;
	}

	NodeSmartReference brushRef( *brushNode );
	worldTraversable->erase( *brushNode );
	entityTraversable->insert( brushRef );

	createdEntities.push_back( entityNode.get_pointer() );
	createdBrushes.push_back( brushNode );
	return entityNode.get_pointer();
}

enum class RoomSide
{
	West,
	East,
	South,
	North
};

struct RoomOpening
{
	RoomSide side = RoomSide::North;
	float along = 0.0f;
	float halfWidth = 64.0f;
	float bottomZ = 0.0f;
	float topZ = 0.0f;
	int linkIndex = -1;
	TraversalKind traversal = TraversalKind::Corridor;
	bool chokePoint = false;
};

struct BlockoutBuildStats
{
	int roomShellBrushes = 0;
	int corridorShellBrushes = 0;
	int detailBrushes = 0;
	int doorEntities = 0;
	int traversalEntities = 0;
	int spawnEntities = 0;
	int itemEntities = 0;
	int liquidBrushes = 0;
	int platformBrushes = 0;
	int stairsBrushes = 0;
};

struct BlockoutBuildResult
{
	std::vector<scene::Node*> brushNodes;
	std::vector<scene::Node*> entityNodes;
	BlockoutBuildStats stats;
};

RoomSide GenAI_toRoomSide( BoundarySide side ){
	switch ( side )
	{
	case BoundarySide::West:
		return RoomSide::West;
	case BoundarySide::East:
		return RoomSide::East;
	case BoundarySide::South:
		return RoomSide::South;
	case BoundarySide::North:
	default:
		return RoomSide::North;
	}
}

void GenAI_addOpeningForLinkEnd( const BlockoutPlan& plan, const PromptToBlockoutOptions& options, int linkIndex, int roomIndex, int otherRoomIndex, float grid, float wallThickness, float floorThickness, float ceilingThickness, std::vector<std::vector<RoomOpening>>& roomOpenings ){
	if ( roomIndex < 0 || roomIndex >= static_cast<int>( plan.rooms.size() ) ) {
		return;
	}
	if ( otherRoomIndex < 0 || otherRoomIndex >= static_cast<int>( plan.rooms.size() ) ) {
		return;
	}
	if ( linkIndex < 0 || linkIndex >= static_cast<int>( plan.links.size() ) ) {
		return;
	}

	const NamedRoom& room = plan.rooms[roomIndex];
	const RoomLink& link = plan.links[linkIndex];
	const CorridorRoute route = GenAI_makeCorridorRoute( plan.rooms[link.from].bounds, plan.rooms[link.to].bounds, link.xFirst, grid );
	if ( !route.valid ) {
		return;
	}

	const CorridorRouteEndpoint* endpoint = nullptr;
	if ( roomIndex == link.from ) {
		endpoint = &route.from;
	}
	else if ( roomIndex == link.to ) {
		endpoint = &route.to;
	}
	else if ( otherRoomIndex == link.from ) {
		endpoint = &route.to;
	}
	else if ( otherRoomIndex == link.to ) {
		endpoint = &route.from;
	}
	if ( endpoint == nullptr ) {
		return;
	}

	const Vector3 roomMins = GenAI_boundsMins( room.bounds );
	const Vector3 roomMaxs = GenAI_boundsMaxs( room.bounds );

	RoomOpening opening;
	opening.linkIndex = linkIndex;
	opening.traversal = link.traversal;
	opening.chokePoint = link.chokePoint;

	const float corridorHalfWidth = std::max( grid * 1.5f, options.corridorWidth * link.widthScale * 0.5f - wallThickness * 0.35f );
	const float roomWidth = roomMaxs.x() - roomMins.x();
	const float roomDepth = roomMaxs.y() - roomMins.y();
	opening.side = GenAI_toRoomSide( endpoint->side );
	if ( opening.side == RoomSide::West || opening.side == RoomSide::East ) {
		opening.halfWidth = GenAI_clampf( corridorHalfWidth, grid * 2.0f, roomDepth * 0.35f );
		opening.along = GenAI_clampf( endpoint->along, roomMins.y() + opening.halfWidth + grid, roomMaxs.y() - opening.halfWidth - grid );
	}
	else{
		opening.halfWidth = GenAI_clampf( corridorHalfWidth, grid * 2.0f, roomWidth * 0.35f );
		opening.along = GenAI_clampf( endpoint->along, roomMins.x() + opening.halfWidth + grid, roomMaxs.x() - opening.halfWidth - grid );
	}

	const float wallBottom = roomMins.z() + floorThickness;
	const float wallTop = roomMaxs.z() - ceilingThickness;
	if ( wallTop - wallBottom < grid * 2.0f ) {
		return;
	}
	const float openingHeight = GenAI_clampf(
		static_cast<float>( options.corridorHeight ) + grid * 2.0f,
		grid * 6.0f,
		wallTop - wallBottom
	);
	opening.bottomZ = wallBottom;
	opening.topZ = std::min( wallBottom + openingHeight, wallTop );

	if ( opening.topZ <= opening.bottomZ + grid * 0.5f ) {
		return;
	}
	roomOpenings[roomIndex].push_back( opening );
}

void GenAI_emitRoomWallWithOpenings( const NamedRoom& room, RoomSide side, const std::vector<RoomOpening>& openings, float wallThickness, float floorThickness, float ceilingThickness, float grid, const QByteArray& wallShaderUtf8, std::size_t brushLimit, BlockoutBuildResult& result ){
	const Vector3 mins = GenAI_boundsMins( room.bounds );
	const Vector3 maxs = GenAI_boundsMaxs( room.bounds );
	const float wallBottom = mins.z() + floorThickness;
	const float wallTop = maxs.z() - ceilingThickness;
	if ( wallTop - wallBottom < grid ) {
		return;
	}

	struct Interval
	{
		float start = 0.0f;
		float end = 0.0f;
		float top = 0.0f;
	};

	std::vector<Interval> intervals;
	intervals.reserve( openings.size() );
	for ( const RoomOpening& opening : openings ) {
		if ( opening.side != side ) {
			continue;
		}
		Interval interval;
		interval.start = opening.along - opening.halfWidth;
		interval.end = opening.along + opening.halfWidth;
		interval.top = opening.topZ;
		if ( side == RoomSide::West || side == RoomSide::East ) {
			interval.start = GenAI_clampf( interval.start, mins.y() + grid, maxs.y() - grid );
			interval.end = GenAI_clampf( interval.end, mins.y() + grid, maxs.y() - grid );
		}
		else{
			interval.start = GenAI_clampf( interval.start, mins.x() + grid, maxs.x() - grid );
			interval.end = GenAI_clampf( interval.end, mins.x() + grid, maxs.x() - grid );
		}
		if ( interval.end <= interval.start + grid * 0.5f ) {
			continue;
		}
		intervals.push_back( interval );
	}
	std::sort( intervals.begin(), intervals.end(), []( const Interval& a, const Interval& b ){ return a.start < b.start; } );

	const float axisMin = ( side == RoomSide::West || side == RoomSide::East ) ? mins.y() : mins.x();
	const float axisMax = ( side == RoomSide::West || side == RoomSide::East ) ? maxs.y() : maxs.x();
	float cursor = axisMin;
	for ( const Interval& interval : intervals ) {
		const float segmentStart = cursor;
		const float segmentEnd = std::min( interval.start, axisMax );
		if ( segmentEnd > segmentStart + grid * 0.5f ) {
			Vector3 segmentMins = mins;
			Vector3 segmentMaxs = maxs;
			if ( side == RoomSide::West || side == RoomSide::East ) {
				segmentMins.y() = segmentStart;
				segmentMaxs.y() = segmentEnd;
				if ( side == RoomSide::West ) {
					segmentMaxs.x() = mins.x() + wallThickness;
				}
				else{
					segmentMins.x() = maxs.x() - wallThickness;
				}
			}
			else{
				segmentMins.x() = segmentStart;
				segmentMaxs.x() = segmentEnd;
				if ( side == RoomSide::South ) {
					segmentMaxs.y() = mins.y() + wallThickness;
				}
				else{
					segmentMins.y() = maxs.y() - wallThickness;
				}
			}
			segmentMins.z() = wallBottom;
			segmentMaxs.z() = wallTop;
			if ( GenAI_emitBrushFromMinMax( segmentMins, segmentMaxs, grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.roomShellBrushes;
			}
		}
		cursor = std::max( cursor, interval.end );
	}

	if ( axisMax > cursor + grid * 0.5f ) {
		Vector3 segmentMins = mins;
		Vector3 segmentMaxs = maxs;
		if ( side == RoomSide::West || side == RoomSide::East ) {
			segmentMins.y() = cursor;
			segmentMaxs.y() = axisMax;
			if ( side == RoomSide::West ) {
				segmentMaxs.x() = mins.x() + wallThickness;
			}
			else{
				segmentMins.x() = maxs.x() - wallThickness;
			}
		}
		else{
			segmentMins.x() = cursor;
			segmentMaxs.x() = axisMax;
			if ( side == RoomSide::South ) {
				segmentMaxs.y() = mins.y() + wallThickness;
			}
			else{
				segmentMins.y() = maxs.y() - wallThickness;
			}
		}
		segmentMins.z() = wallBottom;
		segmentMaxs.z() = wallTop;
		if ( GenAI_emitBrushFromMinMax( segmentMins, segmentMaxs, grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.roomShellBrushes;
		}
	}

	for ( const Interval& interval : intervals ) {
		if ( wallTop - interval.top < grid * 0.5f ) {
			continue;
		}
		Vector3 lintelMins = mins;
		Vector3 lintelMaxs = maxs;
		if ( side == RoomSide::West || side == RoomSide::East ) {
			lintelMins.y() = interval.start;
			lintelMaxs.y() = interval.end;
			if ( side == RoomSide::West ) {
				lintelMaxs.x() = mins.x() + wallThickness;
			}
			else{
				lintelMins.x() = maxs.x() - wallThickness;
			}
		}
		else{
			lintelMins.x() = interval.start;
			lintelMaxs.x() = interval.end;
			if ( side == RoomSide::South ) {
				lintelMaxs.y() = mins.y() + wallThickness;
			}
			else{
				lintelMins.y() = maxs.y() - wallThickness;
			}
		}
		lintelMins.z() = interval.top;
		lintelMaxs.z() = wallTop;
		if ( GenAI_emitBrushFromMinMax( lintelMins, lintelMaxs, grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.roomShellBrushes;
		}
	}
}

void GenAI_generateRoomShell( const NamedRoom& room, const std::vector<RoomOpening>& openings, float wallThickness, float floorThickness, float ceilingThickness, float grid, const QByteArray& floorShaderUtf8, const QByteArray& wallShaderUtf8, const QByteArray& ceilingShaderUtf8, std::size_t brushLimit, BlockoutBuildResult& result ){
	const Vector3 mins = GenAI_boundsMins( room.bounds );
	const Vector3 maxs = GenAI_boundsMaxs( room.bounds );
	const float floorTop = mins.z() + floorThickness;
	const float ceilingBottom = maxs.z() - ceilingThickness;

	if ( GenAI_emitBrushFromMinMax( mins, Vector3( maxs.x(), maxs.y(), floorTop ), grid, floorShaderUtf8, brushLimit, result.brushNodes ) ) {
		++result.stats.roomShellBrushes;
	}
	if ( GenAI_emitBrushFromMinMax( Vector3( mins.x(), mins.y(), ceilingBottom ), maxs, grid, ceilingShaderUtf8, brushLimit, result.brushNodes ) ) {
		++result.stats.roomShellBrushes;
	}

	GenAI_emitRoomWallWithOpenings( room, RoomSide::West, openings, wallThickness, floorThickness, ceilingThickness, grid, wallShaderUtf8, brushLimit, result );
	GenAI_emitRoomWallWithOpenings( room, RoomSide::East, openings, wallThickness, floorThickness, ceilingThickness, grid, wallShaderUtf8, brushLimit, result );
	GenAI_emitRoomWallWithOpenings( room, RoomSide::South, openings, wallThickness, floorThickness, ceilingThickness, grid, wallShaderUtf8, brushLimit, result );
	GenAI_emitRoomWallWithOpenings( room, RoomSide::North, openings, wallThickness, floorThickness, ceilingThickness, grid, wallShaderUtf8, brushLimit, result );
}

void GenAI_generateCorridorShell( const CorridorSegment& corridor, float wallThickness, float floorThickness, float ceilingThickness, float grid, const QByteArray& floorShaderUtf8, const QByteArray& wallShaderUtf8, const QByteArray& ceilingShaderUtf8, const QByteArray& detailShaderUtf8, std::size_t brushLimit, BlockoutBuildResult& result ){
	const Vector3 mins = GenAI_boundsMins( corridor.bounds );
	const Vector3 maxs = GenAI_boundsMaxs( corridor.bounds );
	const float floorTop = mins.z() + floorThickness;
	const float ceilingBottom = maxs.z() - ceilingThickness;

	if ( GenAI_emitBrushFromMinMax( mins, Vector3( maxs.x(), maxs.y(), floorTop ), grid, floorShaderUtf8, brushLimit, result.brushNodes ) ) {
		++result.stats.corridorShellBrushes;
	}
	if ( GenAI_emitBrushFromMinMax( Vector3( mins.x(), mins.y(), ceilingBottom ), maxs, grid, ceilingShaderUtf8, brushLimit, result.brushNodes ) ) {
		++result.stats.corridorShellBrushes;
	}

	const bool axisX = ( maxs.x() - mins.x() ) >= ( maxs.y() - mins.y() );
	const float majorLength = axisX ? ( maxs.x() - mins.x() ) : ( maxs.y() - mins.y() );
	const float crossSpan = axisX ? ( maxs.y() - mins.y() ) : ( maxs.x() - mins.x() );
	float mitreInset = std::min( majorLength * 0.33f, std::max( wallThickness * 1.5f, crossSpan * 0.35f ) );
	mitreInset = GenAI_clampf( mitreInset, 0.0f, std::max( 0.0f, majorLength * 0.45f - grid ) );
	const float trimmedMajorMin = axisX ? mins.x() + mitreInset : mins.y() + mitreInset;
	const float trimmedMajorMax = axisX ? maxs.x() - mitreInset : maxs.y() - mitreInset;

	if ( axisX ) {
		const float wallXMin = ( trimmedMajorMax > trimmedMajorMin + grid ) ? trimmedMajorMin : mins.x();
		const float wallXMax = ( trimmedMajorMax > trimmedMajorMin + grid ) ? trimmedMajorMax : maxs.x();
		if ( GenAI_emitBrushFromMinMax( Vector3( wallXMin, mins.y(), floorTop ), Vector3( wallXMax, mins.y() + wallThickness, ceilingBottom ), grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.corridorShellBrushes;
		}
		if ( GenAI_emitBrushFromMinMax( Vector3( wallXMin, maxs.y() - wallThickness, floorTop ), Vector3( wallXMax, maxs.y(), ceilingBottom ), grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.corridorShellBrushes;
		}
	}
	else{
		const float wallYMin = ( trimmedMajorMax > trimmedMajorMin + grid ) ? trimmedMajorMin : mins.y();
		const float wallYMax = ( trimmedMajorMax > trimmedMajorMin + grid ) ? trimmedMajorMax : maxs.y();
		if ( GenAI_emitBrushFromMinMax( Vector3( mins.x(), wallYMin, floorTop ), Vector3( mins.x() + wallThickness, wallYMax, ceilingBottom ), grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.corridorShellBrushes;
		}
		if ( GenAI_emitBrushFromMinMax( Vector3( maxs.x() - wallThickness, wallYMin, floorTop ), Vector3( maxs.x(), wallYMax, ceilingBottom ), grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.corridorShellBrushes;
		}
	}

	if ( corridor.chokePoint ) {
		const Vector3 center = corridor.bounds.origin;
		const float pinch = std::max( wallThickness * 1.2f, grid * 2.0f );
		const float blockerHeight = std::min( ceilingBottom - floorTop, grid * 8.0f );
		if ( axisX ) {
			const float halfX = std::max( grid * 2.0f, corridor.bounds.extents.x() * 0.15f );
			if ( GenAI_emitBrushFromMinMax(
				Vector3( center.x() - halfX, mins.y() + wallThickness, floorTop ),
				Vector3( center.x() + halfX, mins.y() + wallThickness + pinch, floorTop + blockerHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
			if ( GenAI_emitBrushFromMinMax(
				Vector3( center.x() - halfX, maxs.y() - wallThickness - pinch, floorTop ),
				Vector3( center.x() + halfX, maxs.y() - wallThickness, floorTop + blockerHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
		}
		else{
			const float halfY = std::max( grid * 2.0f, corridor.bounds.extents.y() * 0.15f );
			if ( GenAI_emitBrushFromMinMax(
				Vector3( mins.x() + wallThickness, center.y() - halfY, floorTop ),
				Vector3( mins.x() + wallThickness + pinch, center.y() + halfY, floorTop + blockerHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
			if ( GenAI_emitBrushFromMinMax(
				Vector3( maxs.x() - wallThickness - pinch, center.y() - halfY, floorTop ),
				Vector3( maxs.x() - wallThickness, center.y() + halfY, floorTop + blockerHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
		}
	}
	else if ( majorLength > std::max( grid * 18.0f, 320.0f ) ) {
		// Break very long sightlines with offset half-height cover while keeping clear traversal.
		const Vector3 center = corridor.bounds.origin;
		const float coverHeight = std::min( ceilingBottom - floorTop, std::max( grid * 6.0f, 96.0f ) );
		const float coverHalfMajor = std::max( grid * 2.0f, majorLength * 0.08f );
		const float offset = std::max( grid * 2.0f, crossSpan * 0.22f );
		if ( axisX ) {
			const float coverCenterY = center.y() + offset;
			if ( GenAI_emitBrushFromMinMax(
				Vector3( center.x() - coverHalfMajor, coverCenterY - wallThickness, floorTop ),
				Vector3( center.x() + coverHalfMajor, coverCenterY + wallThickness, floorTop + coverHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
		}
		else{
			const float coverCenterX = center.x() - offset;
			if ( GenAI_emitBrushFromMinMax(
				Vector3( coverCenterX - wallThickness, center.y() - coverHalfMajor, floorTop ),
				Vector3( coverCenterX + wallThickness, center.y() + coverHalfMajor, floorTop + coverHeight ),
				grid, detailShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
		}
	}
}

void GenAI_generateRoomFeatures( const NamedRoom& room, float wallThickness, float floorThickness, float grid, const QByteArray& floorShaderUtf8, const QByteArray& wallShaderUtf8, const QByteArray& liquidShaderUtf8, std::size_t brushLimit, BlockoutBuildResult& result ){
	const Vector3 mins = GenAI_boundsMins( room.bounds );
	const Vector3 maxs = GenAI_boundsMaxs( room.bounds );
	const float floorTop = mins.z() + floorThickness;
	const Vector3 size = maxs - mins;

	AABB platformBounds;
	if ( room.hasPlatform ) {
		const float platformHeight = GenAI_clampf( size.z() * 0.35f, grid * 4.0f, size.z() * 0.6f );
		const float platformWidth = GenAI_clampf( size.x() * 0.4f, grid * 6.0f, size.x() * 0.6f );
		const float platformDepth = GenAI_clampf( size.y() * 0.35f, grid * 6.0f, size.y() * 0.55f );
		const float centerX = room.bounds.origin.x() + size.x() * 0.17f;
		const float centerY = room.bounds.origin.y();
		const Vector3 platformMins(
			GenAI_clampf( centerX - platformWidth * 0.5f, mins.x() + wallThickness + grid * 2.0f, maxs.x() - wallThickness - grid * 2.0f ),
			GenAI_clampf( centerY - platformDepth * 0.5f, mins.y() + wallThickness + grid * 2.0f, maxs.y() - wallThickness - grid * 2.0f ),
			floorTop
		);
		const Vector3 platformMaxs(
			std::min( platformMins.x() + platformWidth, maxs.x() - wallThickness - grid ),
			std::min( platformMins.y() + platformDepth, maxs.y() - wallThickness - grid ),
			floorTop + platformHeight
		);
		platformBounds = GenAI_boundsFromMinMax( platformMins, platformMaxs, grid );

		if ( GenAI_emitBrush( platformBounds, floorShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.platformBrushes;
		}

		const Vector3 pMins = GenAI_boundsMins( platformBounds );
		const Vector3 pMaxs = GenAI_boundsMaxs( platformBounds );
		const float runStart = mins.x() + wallThickness + grid * 2.0f;
		const float runEnd = pMins.x();
		const float runLength = std::max( 0.0f, runEnd - runStart );
		const float rise = pMaxs.z() - floorTop;
		if ( runLength > grid * 4.0f && rise > grid * 2.0f ) {
			const int sliceCount = room.useRamp ? 3 : 6;
			const float sliceDepth = runLength / static_cast<float>( sliceCount );
			for ( int i = 0; i < sliceCount; ++i ) {
				const float startX = runStart + sliceDepth * static_cast<float>( i );
				const float endX = startX + sliceDepth;
				const float stepTop = floorTop + rise * static_cast<float>( i + 1 ) / static_cast<float>( sliceCount );
				Vector3 stepMins( startX, pMins.y(), floorTop );
				Vector3 stepMaxs( std::min( endX, pMins.x() ), pMaxs.y(), stepTop );
				if ( GenAI_emitBrushFromMinMax( stepMins, stepMaxs, grid, floorShaderUtf8, brushLimit, result.brushNodes ) ) {
					++result.stats.stairsBrushes;
				}
			}
		}
	}

	if ( room.hasLiquid ) {
		const Vector3 liquidMins(
			mins.x() + wallThickness + grid * 2.0f,
			mins.y() + wallThickness + grid * 2.0f,
			floorTop
		);
		const Vector3 liquidMaxs(
			maxs.x() - wallThickness - grid * 2.0f,
			maxs.y() - wallThickness - grid * 2.0f,
			floorTop + std::max( grid * 2.0f, size.z() * 0.08f )
		);
		if ( GenAI_emitBrushFromMinMax( liquidMins, liquidMaxs, grid, liquidShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.liquidBrushes;
		}
	}

	if ( room.isHub ) {
		// DM hubs benefit from cross-cover that preserves circulation while breaking dominance sightlines.
		const float crossHalf = std::max( grid * 2.0f, std::min( size.x(), size.y() ) * 0.12f );
		const float crossLengthX = std::max( grid * 4.0f, size.x() * 0.32f );
		const float crossLengthY = std::max( grid * 4.0f, size.y() * 0.32f );
		const float crossHeight = std::max( grid * 6.0f, size.z() * 0.42f );
		if ( GenAI_emitBrushFromMinMax(
			Vector3( room.bounds.origin.x() - crossLengthX * 0.5f, room.bounds.origin.y() - crossHalf, floorTop ),
			Vector3( room.bounds.origin.x() + crossLengthX * 0.5f, room.bounds.origin.y() + crossHalf, floorTop + crossHeight ),
			grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.detailBrushes;
		}
		if ( GenAI_emitBrushFromMinMax(
			Vector3( room.bounds.origin.x() - crossHalf, room.bounds.origin.y() - crossLengthY * 0.5f, floorTop ),
			Vector3( room.bounds.origin.x() + crossHalf, room.bounds.origin.y() + crossLengthY * 0.5f, floorTop + crossHeight ),
			grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.detailBrushes;
		}
	}

	if ( room.isCombatBeat && !room.defendable ) {
		const float coverHalf = std::max( grid * 2.0f, std::min( size.x(), size.y() ) * 0.08f );
		const float coverHeight = std::max( grid * 5.0f, size.z() * 0.36f );
		const float offsetX = size.x() * 0.2f;
		const float offsetY = size.y() * 0.16f;
		if ( GenAI_emitBrushFromMinMax(
			Vector3( room.bounds.origin.x() - offsetX - coverHalf, room.bounds.origin.y() + offsetY - coverHalf, floorTop ),
			Vector3( room.bounds.origin.x() - offsetX + coverHalf, room.bounds.origin.y() + offsetY + coverHalf, floorTop + coverHeight ),
			grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.detailBrushes;
		}
	}

	if ( room.isSideRoute ) {
		// Side-route reward marker: a compact raised cache pedestal.
		const float padW = std::max( grid * 4.0f, size.x() * 0.18f );
		const float padD = std::max( grid * 4.0f, size.y() * 0.18f );
		const float padH = std::max( grid * 2.0f, size.z() * 0.12f );
		const Vector3 padMins(
			room.bounds.origin.x() - padW * 0.5f,
			room.bounds.origin.y() - padD * 0.5f,
			floorTop
		);
		const Vector3 padMaxs(
			room.bounds.origin.x() + padW * 0.5f,
			room.bounds.origin.y() + padD * 0.5f,
			floorTop + padH
		);
		if ( GenAI_emitBrushFromMinMax( padMins, padMaxs, grid, floorShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.detailBrushes;
		}
	}

	if ( room.isGoal ) {
		// Goal spaces benefit from a strong central landmark/readable objective focus.
		const float landmarkHalf = std::max( grid * 2.5f, std::min( size.x(), size.y() ) * 0.1f );
		const float landmarkHeight = std::max( grid * 6.0f, size.z() * 0.45f );
		const Vector3 landmarkMins( room.bounds.origin.x() - landmarkHalf, room.bounds.origin.y() - landmarkHalf, floorTop );
		const Vector3 landmarkMaxs( room.bounds.origin.x() + landmarkHalf, room.bounds.origin.y() + landmarkHalf, floorTop + landmarkHeight );
		if ( GenAI_emitBrushFromMinMax( landmarkMins, landmarkMaxs, grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
			++result.stats.detailBrushes;
		}
	}

	if ( room.defendable ) {
		const float pillarHalf = std::max( grid * 1.5f, std::min( size.x(), size.y() ) * 0.07f );
		const float pillarHeight = std::max( grid * 8.0f, size.z() * 0.5f );
		for ( int i = -1; i <= 1; i += 2 ) {
			const Vector3 center(
				room.bounds.origin.x() + static_cast<float>( i ) * size.x() * 0.18f,
				room.bounds.origin.y() + static_cast<float>( -i ) * size.y() * 0.12f,
				floorTop + pillarHeight * 0.5f
			);
			const Vector3 pillarMins( center.x() - pillarHalf, center.y() - pillarHalf, floorTop );
			const Vector3 pillarMaxs( center.x() + pillarHalf, center.y() + pillarHalf, floorTop + pillarHeight );
			if ( GenAI_emitBrushFromMinMax( pillarMins, pillarMaxs, grid, wallShaderUtf8, brushLimit, result.brushNodes ) ) {
				++result.stats.detailBrushes;
			}
		}
	}
}

void GenAI_placeSpawnsAndItems( const BlockoutPlan& plan, const PromptToBlockoutOptions& options, float floorThickness, float grid, BlockoutBuildResult& result ){
	if ( plan.rooms.empty() ) {
		return;
	}

	const BlockoutPlayStyle playStyle = GenAI_detectPlayStyle( options.prompt );
	const bool campaignStyle = GenAI_isCampaignStyle( playStyle );

	const char* dmSpawnClass = GenAI_pickKnownClass(
		{ "info_player_deathmatch", "info_player_team1", "info_player_team2", "info_player_start" },
		false,
		true
	);
	const char* campaignStartClass = GenAI_pickKnownClass(
		{ "info_player_start", "info_player_coop", "info_player_deathmatch" },
		false,
		true
	);
	const char* majorItemClass = GenAI_pickKnownClass(
		{ "weapon_rocketlauncher", "weapon_railgun", "weapon_lightning", "weapon_bfg", "weapon_hyperblaster", "weapon_supershotgun" },
		false,
		true
	);
	const char* healthClass = GenAI_pickKnownClass(
		{ "item_health_mega", "item_health_large", "item_health", "item_health_small" },
		false,
		true
	);
	const char* armorClass = GenAI_pickKnownClass(
		{ "item_armor_body", "item_armor_combat", "item_armor_jacket", "item_armor_shard" },
		false,
		true
	);
	const char* ammoClass = GenAI_pickKnownClass(
		{ "ammo_rockets", "ammo_cells", "ammo_shells", "ammo_bullets", "ammo_slugs" },
		false,
		true
	);
	const char* bonusItemClass = GenAI_pickKnownClass(
		{ "item_quad", "item_regen", "item_haste", "item_enviro", "item_invisibility", "item_silencer", "item_adrenaline" },
		false,
		true
	);

	auto placeSpawnFacingCenter = [&]( const char* classname, const NamedRoom& room, const Vector3& position )->bool{
		if ( classname == nullptr ) {
			return false;
		}
		const float yaw = std::atan2( room.bounds.origin.y() - position.y(), room.bounds.origin.x() - position.x() ) * 57.2957795f;
		if ( GenAI_createPointEntity( classname, position, { { "angle", QString::number( yaw, 'f', 1 ) } }, result.entityNodes ) != nullptr ) {
			++result.stats.spawnEntities;
			return true;
		}
		return false;
	};

	const std::vector<int> progressionOrder = campaignStyle ? GenAI_mainProgressionPath( plan ) : GenAI_roomProgressionOrder( plan );
	std::set<int> progressionRoomSet( progressionOrder.begin(), progressionOrder.end() );
	std::set<int> controlRooms;
	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		if ( plan.rooms[i].defendable ) {
			controlRooms.insert( static_cast<int>( i ) );
		}
	}
	if ( !campaignStyle && controlRooms.empty() ) {
		const auto [ roomA, roomB ] = GenAI_farthestRoomPair( plan );
		if ( roomA >= 0 ) {
			controlRooms.insert( roomA );
		}
		if ( roomB >= 0 ) {
			controlRooms.insert( roomB );
		}
	}

	if ( campaignStyle ) {
		const int startRoom = progressionOrder.empty() ? 0 : progressionOrder.front();
		const NamedRoom& room = plan.rooms[startRoom];
		const Vector3 mins = GenAI_boundsMins( room.bounds );
		const float floorZ = mins.z() + floorThickness + grid * 1.5f;
		const Vector3 startPos( room.bounds.origin.x() - room.bounds.extents.x() * 0.2f, room.bounds.origin.y(), floorZ );
		placeSpawnFacingCenter( campaignStartClass, room, startPos );
	}
	else{
		struct SpawnCandidate
		{
			int roomIndex = -1;
			Vector3 position = Vector3( 0.0f, 0.0f, 0.0f );
		};
		std::vector<SpawnCandidate> preferredSpawns;
		std::vector<SpawnCandidate> fallbackSpawns;
		preferredSpawns.reserve( plan.rooms.size() * 2 );
		fallbackSpawns.reserve( plan.rooms.size() );
		std::vector<Vector3> placedSpawns;
		const float minSpawnSeparation = std::max( grid * 10.0f, 128.0f );
		const float minSpawnSeparationSq = minSpawnSeparation * minSpawnSeparation;

		for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
			const NamedRoom& room = plan.rooms[i];
			const Vector3 mins = GenAI_boundsMins( room.bounds );
			const float floorZ = mins.z() + floorThickness + grid * 1.5f;
			const Vector3 spawnA(
				room.bounds.origin.x() + ( ( i % 2 == 0 ) ? -1.0f : 1.0f ) * room.bounds.extents.x() * 0.24f,
				room.bounds.origin.y() + ( ( i % 3 == 0 ) ? -1.0f : 1.0f ) * room.bounds.extents.y() * 0.22f,
				floorZ
			);
			const Vector3 spawnB(
				room.bounds.origin.x() - ( ( i % 2 == 0 ) ? -1.0f : 1.0f ) * room.bounds.extents.x() * 0.22f,
				room.bounds.origin.y() - ( ( i % 3 == 0 ) ? -1.0f : 1.0f ) * room.bounds.extents.y() * 0.18f,
				floorZ
			);
			const bool controlRoom = controlRooms.find( static_cast<int>( i ) ) != controlRooms.end();
			const bool highControlRoom = controlRoom || room.isHub;
			if ( highControlRoom ) {
				fallbackSpawns.push_back( { static_cast<int>( i ), spawnA } );
			}
			else{
				preferredSpawns.push_back( { static_cast<int>( i ), spawnA } );
				const bool largeRoom = ( room.bounds.extents.x() + room.bounds.extents.y() ) > 420.0f;
				if ( largeRoom ) {
					preferredSpawns.push_back( { static_cast<int>( i ), spawnB } );
				}
			}
		}

		int spawnBudget = GenAI_clampi( static_cast<int>( plan.rooms.size() ) * 2, 8, 24 );
		auto isSpawnSeparated = [&]( const Vector3& position ){
			for ( const Vector3& placed : placedSpawns ) {
				const Vector3 delta = position - placed;
				const float distSq = delta.x() * delta.x() + delta.y() * delta.y() + delta.z() * delta.z();
				if ( distSq < minSpawnSeparationSq ) {
					return false;
				}
			}
			return true;
		};
		auto emitSpawnCandidates = [&]( const std::vector<SpawnCandidate>& candidates ){
			for ( const SpawnCandidate& candidate : candidates ) {
				if ( spawnBudget <= 0 ) {
					break;
				}
				if ( candidate.roomIndex < 0 || candidate.roomIndex >= static_cast<int>( plan.rooms.size() ) ) {
					continue;
				}
				if ( !isSpawnSeparated( candidate.position ) ) {
					continue;
				}
				if ( placeSpawnFacingCenter( dmSpawnClass, plan.rooms[candidate.roomIndex], candidate.position ) ) {
					placedSpawns.push_back( candidate.position );
					--spawnBudget;
				}
			}
		};
		if ( dmSpawnClass != nullptr ) {
			emitSpawnCandidates( preferredSpawns );
			emitSpawnCandidates( fallbackSpawns );
		}
	}

	const int maxMajorItems = campaignStyle ? 2 : 3;
	std::set<int> majorRooms;
	if ( majorItemClass != nullptr ) {
		std::vector<std::pair<float, int>> majorCandidates;
		majorCandidates.reserve( plan.rooms.size() );
		for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
			const NamedRoom& room = plan.rooms[i];
			const float area = room.bounds.extents.x() * room.bounds.extents.y() * 4.0f;
			float score = area;
			if ( room.isHub ) {
				score += 380000.0f;
			}
			if ( room.defendable ) {
				score += 300000.0f;
			}
			if ( room.isGoal ) {
				score += 260000.0f;
			}
			if ( room.isCombatBeat ) {
				score += 140000.0f;
			}
			if ( room.isSideRoute ) {
				score += 40000.0f;
			}
			if ( campaignStyle && room.isTransition && !room.isGoal ) {
				score -= 120000.0f;
			}
			majorCandidates.push_back( std::make_pair( score, static_cast<int>( i ) ) );
		}
		std::sort( majorCandidates.begin(), majorCandidates.end(), []( const auto& a, const auto& b ){ return a.first > b.first; } );

		const float majorSpacing = campaignStyle
			? std::max( 320.0f, static_cast<float>( options.roomSpacing ) * 0.55f )
			: std::max( 384.0f, static_cast<float>( options.roomSpacing ) * 0.65f );
		int placed = 0;
		for ( const auto& [ score, roomIndex ] : majorCandidates ) {
			( void )score;
			if ( placed >= maxMajorItems ) {
				break;
			}
			const NamedRoom& room = plan.rooms[roomIndex];
			bool separated = true;
			for ( const int existing : majorRooms ) {
				if ( GenAI_roomDistanceXY( room.bounds, plan.rooms[existing].bounds ) < majorSpacing ) {
					separated = false;
					break;
				}
			}
			if ( !separated && !majorRooms.empty() ) {
				continue;
			}
			const float floorZ = GenAI_boundsMins( room.bounds ).z() + floorThickness + grid * 1.5f;
			const Vector3 itemPos( room.bounds.origin.x(), room.bounds.origin.y(), floorZ + grid * 2.0f );
			if ( GenAI_createPointEntity( majorItemClass, itemPos, {}, result.entityNodes ) != nullptr ) {
				majorRooms.insert( roomIndex );
				++result.stats.itemEntities;
				++placed;
			}
		}
	}

	int healthBudget = campaignStyle
		? GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 2, 2, 10 )
		: GenAI_clampi( static_cast<int>( plan.rooms.size() ) * 2 / 3, 3, 14 );
	int ammoBudget = campaignStyle
		? GenAI_clampi( static_cast<int>( plan.rooms.size() ) * 2 / 3, 2, 12 )
		: GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 2, 3, 10 );
	int armorBudget = campaignStyle
		? GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 3, 1, 6 )
		: GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 3, 2, 8 );
	int bonusBudget = campaignStyle
		? GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 4, 1, 4 )
		: GenAI_clampi( static_cast<int>( plan.rooms.size() ) / 5, 1, 3 );

	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		if ( majorRooms.find( static_cast<int>( i ) ) != majorRooms.end() ) {
			continue;
		}

		const NamedRoom& room = plan.rooms[i];
		const Vector3 mins = GenAI_boundsMins( room.bounds );
		const Vector3 maxs = GenAI_boundsMaxs( room.bounds );
		const float floorZ = mins.z() + floorThickness + grid * 1.5f;
		const Vector3 healthPos( mins.x() + room.bounds.extents.x() * 0.35f, maxs.y() - room.bounds.extents.y() * 0.25f, floorZ );
		const Vector3 ammoPos( maxs.x() - room.bounds.extents.x() * 0.35f, mins.y() + room.bounds.extents.y() * 0.2f, floorZ );
		const Vector3 armorPos( room.bounds.origin.x(), room.bounds.origin.y(), floorZ );

		const bool onMainProgression = progressionRoomSet.find( static_cast<int>( i ) ) != progressionRoomSet.end();
		const bool placeHealth = campaignStyle
			? ( room.isCombatBeat || room.isGoal || room.isStart || room.isSideRoute )
			: ( !room.isHub && ( !room.defendable || i % 2 == 0 ) );
		const bool placeAmmo = campaignStyle
			? ( room.isTransition || room.isCombatBeat || room.isSideRoute )
			: ( !room.isHub || room.connectivity >= 3 );
		const bool placeArmor = campaignStyle
			? ( onMainProgression || room.isGoal )
			: ( room.defendable || room.isSideRoute || room.connectivity >= 3 );

		if ( placeHealth && healthClass != nullptr && healthBudget > 0 ) {
			if ( GenAI_createPointEntity( healthClass, healthPos, {}, result.entityNodes ) != nullptr ) {
				++result.stats.itemEntities;
				--healthBudget;
			}
		}
		if ( placeAmmo && ammoClass != nullptr && ammoBudget > 0 ) {
			if ( GenAI_createPointEntity( ammoClass, ammoPos, {}, result.entityNodes ) != nullptr ) {
				++result.stats.itemEntities;
				--ammoBudget;
			}
		}
		if ( placeArmor && armorClass != nullptr && armorBudget > 0 ) {
			if ( GenAI_createPointEntity( armorClass, armorPos, {}, result.entityNodes ) != nullptr ) {
				++result.stats.itemEntities;
				--armorBudget;
			}
		}
		if ( room.isSideRoute && bonusItemClass != nullptr && bonusBudget > 0 ) {
			const Vector3 bonusPos( room.bounds.origin.x(), room.bounds.origin.y(), floorZ + grid * 3.0f );
			if ( GenAI_createPointEntity( bonusItemClass, bonusPos, {}, result.entityNodes ) != nullptr ) {
				++result.stats.itemEntities;
				--bonusBudget;
			}
		}
	}
}

void GenAI_placeTraversalEntities( const BlockoutPlan& plan, float floorThickness, float grid, const QByteArray& solidShaderUtf8, const QByteArray& triggerShaderUtf8, std::size_t brushLimit, BlockoutBuildResult& result ){
	const char* doorClass = GenAI_pickKnownClass( { "func_door" }, true, false );
	const char* platClass = GenAI_pickKnownClass( { "func_plat" }, true, false );
	const char* teleTriggerClass = GenAI_pickKnownClass( { "trigger_teleport" }, true, false );
	const char* teleDestClass = GenAI_pickKnownClass( { "misc_teleporter_dest", "target_position", "info_teleport_destination" }, false, true );
	const char* jumpTriggerClass = GenAI_pickKnownClass( { "trigger_push" }, true, false );
	const char* jumpDestClass = GenAI_pickKnownClass( { "target_position" }, false, true );

	for ( std::size_t i = 0; i < plan.links.size(); ++i ) {
		const RoomLink& link = plan.links[i];
		const NamedRoom& from = plan.rooms[link.from];
		const NamedRoom& to = plan.rooms[link.to];
		const Vector3 fromMins = GenAI_boundsMins( from.bounds );
		const Vector3 toMins = GenAI_boundsMins( to.bounds );
		const float fromFloor = fromMins.z() + floorThickness + grid;
		const float toFloor = toMins.z() + floorThickness + grid;
		const Vector3 mid = ( from.bounds.origin + to.bounds.origin ) * 0.5f;

		if ( link.traversal == TraversalKind::Door && doorClass != nullptr ) {
			AABB targetCorridor = {};
			bool haveCorridor = false;
			for ( const CorridorSegment& corridor : plan.corridors ) {
				if ( corridor.linkIndex == static_cast<int>( i ) ) {
					targetCorridor = corridor.bounds;
					haveCorridor = true;
					break;
				}
			}
			if ( haveCorridor ) {
				const Vector3 cMins = GenAI_boundsMins( targetCorridor );
				const Vector3 cMaxs = GenAI_boundsMaxs( targetCorridor );
				const bool axisX = ( cMaxs.x() - cMins.x() ) >= ( cMaxs.y() - cMins.y() );
				const float doorHeight = std::max( grid * 6.0f, ( cMaxs.z() - cMins.z() ) * 0.7f );
				Vector3 dMins = cMins;
				Vector3 dMaxs = cMaxs;
				if ( axisX ) {
					dMins.x() = targetCorridor.origin.x() - grid * 2.0f;
					dMaxs.x() = targetCorridor.origin.x() + grid * 2.0f;
					dMins.y() += grid * 1.5f;
					dMaxs.y() -= grid * 1.5f;
				}
				else{
					dMins.y() = targetCorridor.origin.y() - grid * 2.0f;
					dMaxs.y() = targetCorridor.origin.y() + grid * 2.0f;
					dMins.x() += grid * 1.5f;
					dMaxs.x() -= grid * 1.5f;
				}
				dMins.z() = cMins.z() + floorThickness;
				dMaxs.z() = std::min( cMaxs.z() - floorThickness, dMins.z() + doorHeight );
				const AABB doorBrush = GenAI_boundsFromMinMax( dMins, dMaxs, grid );
				if ( GenAI_createSingleBrushEntity(
					doorClass,
					doorBrush,
					solidShaderUtf8,
					{
						{ "speed", "220" },
						{ "wait", "2" }
					},
					brushLimit,
					result.entityNodes,
					result.brushNodes
				) != nullptr ) {
					++result.stats.doorEntities;
				}
			}
		}
		else if ( link.traversal == TraversalKind::FuncPlat && platClass != nullptr ) {
			const float size = std::max( grid * 6.0f, 96.0f );
			const Vector3 mins( mid.x() - size * 0.5f, mid.y() - size * 0.5f, std::min( fromFloor, toFloor ) );
			const Vector3 maxs( mid.x() + size * 0.5f, mid.y() + size * 0.5f, std::min( fromFloor, toFloor ) + std::max( grid * 3.0f, 48.0f ) );
			if ( GenAI_createSingleBrushEntity(
				platClass,
				GenAI_boundsFromMinMax( mins, maxs, grid ),
				solidShaderUtf8,
				{
					{ "speed", "150" },
					{ "height", QString::number( std::max( 96.0f, std::fabs( toFloor - fromFloor ) + 64.0f ), 'f', 0 ) }
				},
				brushLimit,
				result.entityNodes,
				result.brushNodes
			) != nullptr ) {
				++result.stats.traversalEntities;
			}
		}
		else if ( link.traversal == TraversalKind::Teleporter && teleTriggerClass != nullptr && teleDestClass != nullptr ) {
			const QString target = QStringLiteral( "genai_tp_%1" ).arg( i );
			const Vector3 destPos( to.bounds.origin.x(), to.bounds.origin.y(), toFloor + grid * 2.0f );
			if ( GenAI_createPointEntity( teleDestClass, destPos, { { "targetname", target } }, result.entityNodes ) != nullptr ) {
				const float size = std::max( grid * 6.0f, 96.0f );
				const Vector3 triggerMins( from.bounds.origin.x() - size * 0.5f, from.bounds.origin.y() - size * 0.5f, fromFloor );
				const Vector3 triggerMaxs( from.bounds.origin.x() + size * 0.5f, from.bounds.origin.y() + size * 0.5f, fromFloor + std::max( grid * 4.0f, 64.0f ) );
				if ( GenAI_createSingleBrushEntity(
					teleTriggerClass,
					GenAI_boundsFromMinMax( triggerMins, triggerMaxs, grid ),
					triggerShaderUtf8,
					{ { "target", target } },
					brushLimit,
					result.entityNodes,
					result.brushNodes
				) != nullptr ) {
					++result.stats.traversalEntities;
				}
			}
		}
		else if ( link.traversal == TraversalKind::JumpPad && jumpTriggerClass != nullptr && jumpDestClass != nullptr ) {
			const QString target = QStringLiteral( "genai_jump_%1" ).arg( i );
			const Vector3 destPos( to.bounds.origin.x(), to.bounds.origin.y(), toFloor + std::max( grid * 10.0f, 160.0f ) );
			if ( GenAI_createPointEntity( jumpDestClass, destPos, { { "targetname", target } }, result.entityNodes ) != nullptr ) {
				const float size = std::max( grid * 4.0f, 80.0f );
				const Vector3 triggerMins( from.bounds.origin.x() - size * 0.5f, from.bounds.origin.y() - size * 0.5f, fromFloor );
				const Vector3 triggerMaxs( from.bounds.origin.x() + size * 0.5f, from.bounds.origin.y() + size * 0.5f, fromFloor + grid * 2.0f );
				if ( GenAI_createSingleBrushEntity(
					jumpTriggerClass,
					GenAI_boundsFromMinMax( triggerMins, triggerMaxs, grid ),
					triggerShaderUtf8,
					{
						{ "target", target },
						{ "speed", "950" }
					},
					brushLimit,
					result.entityNodes,
					result.brushNodes
				) != nullptr ) {
					++result.stats.traversalEntities;
				}
			}
		}
	}
}

BlockoutBuildResult GenAI_buildAdvancedBlockout( const PromptToBlockoutOptions& options, const BlockoutPlan& plan, const QByteArray& shaderUtf8, float grid ){
	BlockoutBuildResult result;
	const std::size_t brushLimit = 2048;
	const float wallThickness = GenAI_clampf( grid * 2.0f, 16.0f, 64.0f );
	const float floorThickness = std::max( grid * 2.0f, 16.0f );
	const float ceilingThickness = std::max( grid * 2.0f, 16.0f );
	const QString fallbackShader = shaderUtf8.isEmpty() ? GenAI_defaultBlockoutShader() : QString::fromUtf8( shaderUtf8.constData() ).trimmed();
	const QString floorShader = options.floorShader.trimmed().isEmpty() ? fallbackShader : options.floorShader.trimmed();
	const QString wallShader = options.wallShader.trimmed().isEmpty() ? fallbackShader : options.wallShader.trimmed();
	const QString ceilingShader = options.ceilingShader.trimmed().isEmpty() ? fallbackShader : options.ceilingShader.trimmed();
	const QString skyShader = options.skyShader.trimmed().isEmpty() ? GenAI_defaultSkyShader() : options.skyShader.trimmed();
	const bool useSkyCeiling = options.useSkyCeiling && !skyShader.isEmpty();
	const bool useIdTech3Caulk = options.idTech3Caulk && GenAI_isIdTech3Game();
	const QString caulkShader = useIdTech3Caulk ? GenAI_defaultCaulkShader() : QString();
	const QString detailShader = ( useIdTech3Caulk && !caulkShader.isEmpty() ) ? caulkShader : wallShader;
	const QString triggerShader = [](){
		const QString commonTrigger = GenAI_commonShader( "trigger" );
		if ( !commonTrigger.isEmpty() ) {
			return commonTrigger;
		}
		return QString();
	}();
	const QByteArray floorShaderUtf8 = floorShader.toUtf8();
	const QByteArray wallShaderUtf8 = wallShader.toUtf8();
	const QByteArray ceilingShaderUtf8 = ceilingShader.toUtf8();
	const QByteArray skyShaderUtf8 = skyShader.toUtf8();
	const QByteArray ceilingSurfaceShaderUtf8 = useSkyCeiling ? skyShaderUtf8 : ceilingShaderUtf8;
	const QByteArray detailShaderUtf8 = detailShader.toUtf8();
	const QByteArray triggerShaderUtf8 = triggerShader.isEmpty() ? detailShaderUtf8 : triggerShader.toUtf8();
	const QString liquidShader = [](){
		const QString water = QString::fromUtf8( GetCommonShader( "water" ).c_str() ).trimmed();
		if ( !water.isEmpty() ) {
			return water;
		}
		const QString slime = QString::fromUtf8( GetCommonShader( "slime" ).c_str() ).trimmed();
		if ( !slime.isEmpty() ) {
			return slime;
		}
		return QString::fromUtf8( GetCommonShader( "notex" ).c_str() ).trimmed();
	}();
	const QByteArray liquidShaderUtf8 = liquidShader.isEmpty() ? floorShaderUtf8 : liquidShader.toUtf8();

	std::vector<std::vector<RoomOpening>> roomOpenings( plan.rooms.size() );
	for ( std::size_t i = 0; i < plan.links.size(); ++i ) {
		const RoomLink& link = plan.links[i];
		if ( !GenAI_linkUsesPhysicalCorridor( link ) ) {
			continue;
		}
		GenAI_addOpeningForLinkEnd( plan, options, static_cast<int>( i ), link.from, link.to, grid, wallThickness, floorThickness, ceilingThickness, roomOpenings );
		GenAI_addOpeningForLinkEnd( plan, options, static_cast<int>( i ), link.to, link.from, grid, wallThickness, floorThickness, ceilingThickness, roomOpenings );
	}

	for ( std::size_t i = 0; i < plan.rooms.size(); ++i ) {
		const NamedRoom& room = plan.rooms[i];
		GenAI_generateRoomShell( room, roomOpenings[i], wallThickness, floorThickness, ceilingThickness, grid, floorShaderUtf8, wallShaderUtf8, ceilingSurfaceShaderUtf8, brushLimit, result );
		GenAI_generateRoomFeatures( room, wallThickness, floorThickness, grid, floorShaderUtf8, wallShaderUtf8, liquidShaderUtf8, brushLimit, result );
	}

	for ( const CorridorSegment& corridor : plan.corridors ) {
		GenAI_generateCorridorShell( corridor, wallThickness, floorThickness, ceilingThickness, grid, floorShaderUtf8, wallShaderUtf8, ceilingSurfaceShaderUtf8, detailShaderUtf8, brushLimit, result );
	}

	GenAI_placeTraversalEntities( plan, floorThickness, grid, wallShaderUtf8, triggerShaderUtf8, brushLimit, result );
	GenAI_placeSpawnsAndItems( plan, options, floorThickness, grid, result );
	return result;
}

QString GenAI_effectiveApiKey(){
	const QString stored = QString::fromUtf8( g_genAIApiKey.c_str() ).trimmed();
	if ( !stored.isEmpty() ) {
		return stored;
	}
	return QString::fromUtf8( qgetenv( "OPENAI_API_KEY" ) ).trimmed();
}

QString GenAI_normaliseBaseUrl(){
	QString baseUrl = QString::fromUtf8( g_genAIBaseUrl.c_str() ).trimmed();
	while ( baseUrl.endsWith( '/' ) ) {
		baseUrl.chop( 1 );
	}
	return baseUrl;
}

QString GenAI_normaliseResponsesPath(){
	QString path = QString::fromUtf8( g_genAIResponsesPath.c_str() ).trimmed();
	if ( path.isEmpty() ) {
		path = "/responses";
	}
	if ( !path.startsWith( '/' ) ) {
		path.prepend( '/' );
	}
	return path;
}

QUrl GenAI_endpointUrl( QString* error ){
	if ( error != nullptr ) {
		error->clear();
	}

	const QString baseUrl = GenAI_normaliseBaseUrl();
	if ( baseUrl.isEmpty() ) {
		if ( error != nullptr ) {
			*error = "OpenAI base URL is empty.";
		}
		return {};
	}

	const QUrl parsedBase( baseUrl );
	if ( !parsedBase.isValid() || parsedBase.scheme().isEmpty() || parsedBase.host().isEmpty() ) {
		if ( error != nullptr ) {
			*error = QString( "OpenAI base URL is invalid: %1" ).arg( baseUrl );
		}
		return {};
	}

	const QString rawPath = QString::fromUtf8( g_genAIResponsesPath.c_str() ).trimmed();
	if ( rawPath.startsWith( "http://", Qt::CaseInsensitive ) || rawPath.startsWith( "https://", Qt::CaseInsensitive ) ) {
		const QUrl absoluteEndpoint( rawPath );
		if ( !absoluteEndpoint.isValid() || absoluteEndpoint.scheme().isEmpty() || absoluteEndpoint.host().isEmpty() ) {
			if ( error != nullptr ) {
				*error = QString( "OpenAI endpoint URL is invalid: %1" ).arg( rawPath );
			}
			return {};
		}
		return absoluteEndpoint;
	}

	QString joinedBase = baseUrl;
	while ( joinedBase.endsWith( '/' ) ) {
		joinedBase.chop( 1 );
	}

	QString joinedPath = GenAI_normaliseResponsesPath();
	if ( !joinedPath.startsWith( '/' ) ) {
		joinedPath.prepend( '/' );
	}

	const QUrl endpoint( joinedBase + joinedPath );
	if ( !endpoint.isValid() ) {
		if ( error != nullptr ) {
			*error = QString( "OpenAI endpoint URL is invalid: %1" ).arg( endpoint.toString() );
		}
		return {};
	}
	return endpoint;
}

bool GenAI_checkConfigured( QString* reason ){
	if ( reason != nullptr ) {
		reason->clear();
	}

	if ( !g_genAIEnabled ) {
		if ( reason != nullptr ) {
			*reason = "GenAI is disabled.";
		}
		return false;
	}

	const QString model = QString::fromUtf8( g_genAIModel.c_str() ).trimmed();
	if ( model.isEmpty() ) {
		if ( reason != nullptr ) {
			*reason = "OpenAI model is empty.";
		}
		return false;
	}

	QString endpointError;
	if ( !GenAI_endpointUrl( &endpointError ).isValid() ) {
		if ( reason != nullptr ) {
			*reason = endpointError;
		}
		return false;
	}

	if ( GenAI_effectiveApiKey().isEmpty() ) {
		if ( reason != nullptr ) {
			*reason = "No API key is configured (set one in Preferences or OPENAI_API_KEY).";
		}
		return false;
	}

	return true;
}

QString GenAI_openAIPlannerPrompt( const PromptToBlockoutOptions& options ){
	const int maxOffset = std::max( options.roomSpacing * options.roomCount, options.roomMaxSize * options.roomCount );
	const BlockoutPlayStyle playStyle = GenAI_detectPlayStyle( options.prompt );
	QString styleGuidance;
	if ( GenAI_isCampaignStyle( playStyle ) ) {
		styleGuidance =
			"- Campaign guidance: build a clear start-to-goal progression with readable landmarks.\n"
			"- Include encounter beats (combat spaces) separated by safer transition spaces.\n"
			"- Use alternating wide combat arenas and tighter connectors to pace intensity.\n"
			"- Favor stairs/ramps/doors over teleport-heavy traversal unless explicitly requested.\n"
			"- Add at least one optional side-route that rejoins progression (secret/reward style).\n"
			"- Keep objective-critical routes readable from major landmarks.\n"
			"- Reserve largest/most vertical combat room near the end of progression.\n";
	}
	else{
		styleGuidance =
			"- Deathmatch guidance: prioritize looped circulation with multiple route choices.\n"
			"- Avoid dead-end chains; keep at least one loop through major control spaces.\n"
			"- Separate major-control spaces so movement decisions and item timing matter.\n"
			"- Use choke routes sparingly and keep enough open alternatives for flow.\n"
			"- Keep spawn locations out of strongest control sightlines when possible.\n"
			"- Include one central contested hub with at least three approach connections.\n";
	}
	return QString(
		"Design a practical, production-oriented FPS blockout layout.\n"
		"Return ONLY JSON (no markdown) with this exact schema:\n"
		"{\"rooms\":[{\"name\":\"string\",\"role\":\"hub|arena|connector|secret|start|goal(optional)\",\"x\":number,\"y\":number,\"z\":number,\"width\":number,\"depth\":number,\"height\":number}],"
		"\"connections\":[{\"from\":number,\"to\":number,\"type\":\"corridor|door|stairs|ramp|func_plat|jumppad|teleporter\",\"choke\":boolean,\"widthScale\":number}]}\n"
		"Rules:\n"
		"- Create exactly %1 rooms.\n"
		"- Coordinates are offsets from anchor (0,0,0) in map units.\n"
		"- Keep x/y offsets roughly within +/- %2.\n"
		"- Width/depth between %3 and %4.\n"
		"- Typical height %5.\n"
		"- Keep room/corridor geometry axis-aligned.\n"
		"- connections are indices into rooms.\n"
		"- Ensure the graph is connected.\n"
		"- Include choke-point links near defendable item routes.\n"
		"- Prefer traversal variety (doors, stairs/ramps, occasional teleporter/jumppad) when suitable.\n"
		"- Tag room roles when obvious so downstream generation can place spawns/items appropriately.\n"
		"%6"
		"User request:\n%7"
	)
	.arg( options.roomCount )
	.arg( maxOffset )
	.arg( options.roomMinSize )
	.arg( options.roomMaxSize )
	.arg( options.roomHeight )
	.arg( styleGuidance )
	.arg( options.prompt );
}

QString GenAI_parseOpenAIErrorMessage( const QByteArray& body ){
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson( body, &parseError );
	if ( parseError.error != QJsonParseError::NoError || !doc.isObject() ) {
		return QString::fromUtf8( body ).trimmed();
	}
	const QJsonObject root = doc.object();
	const QJsonObject error = root.value( "error" ).toObject();
	const QString message = error.value( "message" ).toString().trimmed();
	if ( !message.isEmpty() ) {
		return message;
	}
	return QString::fromUtf8( body ).trimmed();
}

QString GenAI_extractResponseText( const QJsonObject& root ){
	const QString topLevel = root.value( "output_text" ).toString().trimmed();
	if ( !topLevel.isEmpty() ) {
		return topLevel;
	}

	QStringList parts;
	const QJsonArray output = root.value( "output" ).toArray();
	for ( const QJsonValue& entryValue : output ) {
		const QJsonObject entry = entryValue.toObject();
		const QString entryType = entry.value( "type" ).toString();
		if ( entryType == "message" ) {
			const QJsonArray content = entry.value( "content" ).toArray();
			for ( const QJsonValue& contentValue : content ) {
				const QJsonObject contentObject = contentValue.toObject();
				const QString text = contentObject.value( "text" ).toString().trimmed();
				if ( !text.isEmpty() ) {
					parts.push_back( text );
					continue;
				}
				const QString outputText = contentObject.value( "output_text" ).toString().trimmed();
				if ( !outputText.isEmpty() ) {
					parts.push_back( outputText );
				}
			}
		}
		else if ( entryType == "output_text" ) {
			const QString text = entry.value( "text" ).toString().trimmed();
			if ( !text.isEmpty() ) {
				parts.push_back( text );
			}
		}
	}

	return parts.join( "\n" ).trimmed();
}

QByteArray GenAI_extractJsonObject( const QString& text ){
	const QString trimmed = text.trimmed();
	const int firstBrace = trimmed.indexOf( '{' );
	const int lastBrace = trimmed.lastIndexOf( '}' );
	if ( firstBrace >= 0 && lastBrace > firstBrace ) {
		return trimmed.mid( firstBrace, lastBrace - firstBrace + 1 ).toUtf8();
	}
	return trimmed.toUtf8();
}

double GenAI_jsonNumber( const QJsonObject& object, const char* key, double fallback ){
	const QJsonValue value = object.value( key );
	if ( value.isDouble() ) {
		return value.toDouble();
	}
	return fallback;
}

int GenAI_connectionIndex( const QJsonValue& value, const std::map<QString, int>& nameToIndex ){
	if ( value.isDouble() ) {
		return static_cast<int>( value.toInt() );
	}
	if ( value.isString() ) {
		const auto found = nameToIndex.find( value.toString() );
		if ( found != nameToIndex.end() ) {
			return found->second;
		}
	}
	return -1;
}

bool GenAI_buildPlanFromJsonObject( const QJsonObject& object, const PromptToBlockoutOptions& options, const Vector3& anchor, float floorZ, float grid, BlockoutPlan& plan, QString& error ){
	plan.rooms.clear();
	plan.links.clear();
	plan.corridors.clear();

	const QJsonArray rooms = object.value( "rooms" ).toArray();
	if ( rooms.isEmpty() ) {
		error = "Planner output does not contain rooms.";
		return false;
	}

	const int minSize = std::min( options.roomMinSize, options.roomMaxSize );
	const int maxSize = std::max( options.roomMinSize, options.roomMaxSize );
	const int roomLimit = GenAI_clampi( options.roomCount, 2, 24 );
	std::map<QString, int> nameToIndex;

	for ( int i = 0; i < rooms.size() && static_cast<int>( plan.rooms.size() ) < roomLimit; ++i ) {
		const QJsonObject room = rooms.at( i ).toObject();
		const QString name = room.value( "name" ).toString().trimmed().isEmpty()
			? QStringLiteral( "room_%1" ).arg( i )
			: room.value( "name" ).toString().trimmed();

		const float offsetX = static_cast<float>( GenAI_jsonNumber( room, "x", static_cast<double>( i * options.roomSpacing ) ) );
		const float offsetY = static_cast<float>( GenAI_jsonNumber( room, "y", 0.0 ) );
		const float offsetZ = static_cast<float>( GenAI_jsonNumber( room, "z", options.roomHeight * 0.5 ) );
		const float width = static_cast<float>( GenAI_jsonNumber( room, "width", static_cast<double>( minSize ) ) );
		const float depth = static_cast<float>( GenAI_jsonNumber( room, "depth", static_cast<double>( minSize ) ) );
		const float height = static_cast<float>( GenAI_jsonNumber( room, "height", static_cast<double>( options.roomHeight ) ) );

		const Vector3 center( anchor.x() + offsetX, anchor.y() + offsetY, floorZ + offsetZ );
		GenAI_addRoom(
			plan,
			name,
			center,
			GenAI_clampf( width, static_cast<float>( minSize ), static_cast<float>( maxSize ) ),
			GenAI_clampf( depth, static_cast<float>( minSize ), static_cast<float>( maxSize ) ),
			GenAI_clampf( height, 64.0f, 2048.0f ),
			grid
		);
		const QString role = room.value( "role" ).toString().trimmed().toLower();
		if ( !role.isEmpty() ) {
			plan.rooms.back().plannerRole = role;
		}
		nameToIndex[plan.rooms.back().name] = static_cast<int>( plan.rooms.size() - 1 );
	}

	if ( plan.rooms.size() < 2 ) {
		error = "Planner output produced fewer than two valid rooms.";
		return false;
	}

	const float corridorWidth = GenAI_clampf(
		static_cast<float>( GenAI_jsonNumber( object, "corridorWidth", options.corridorWidth ) ),
		32.0f,
		1024.0f
	);
	const float corridorHeight = GenAI_clampf(
		static_cast<float>( GenAI_jsonNumber( object, "corridorHeight", options.corridorHeight ) ),
		32.0f,
		1024.0f
	);

	const QJsonArray edges = object.value( "connections" ).toArray();
	for ( const QJsonValue& edgeValue : edges ) {
		if ( plan.links.size() >= 128 ) {
			break;
		}
		const QJsonObject edge = edgeValue.toObject();
		const int from = GenAI_connectionIndex( edge.value( "from" ), nameToIndex );
		const int to = GenAI_connectionIndex( edge.value( "to" ), nameToIndex );
		if ( from < 0 || to < 0 || from == to ) {
			continue;
		}
		if ( from >= static_cast<int>( plan.rooms.size() ) || to >= static_cast<int>( plan.rooms.size() ) ) {
			continue;
		}
		const bool xFirst = edge.value( "xFirst" ).toBool( static_cast<int>( plan.links.size() ) % 2 == 0 );
		const bool chokePoint = edge.value( "choke" ).toBool( false );
		const float widthScale = GenAI_clampf(
			static_cast<float>( GenAI_jsonNumber( edge, "widthScale", chokePoint ? 0.75 : 1.0 ) ),
			0.45f,
			1.4f
		);
		const TraversalKind traversal = GenAI_parseTraversalKind( edge.value( "type" ).toString() );
		GenAI_addLink( plan, from, to, xFirst, traversal, chokePoint, widthScale );
	}

	if ( plan.links.empty() ) {
		GenAI_addSequentialConnections( plan, corridorWidth, corridorHeight, grid );
	}
	else{
		GenAI_rebuildCorridorsFromLinks( plan, corridorWidth, corridorHeight, grid );
	}

	return true;
}

bool GenAI_generateOpenAIBlockoutPlan( const PromptToBlockoutOptions& options, const Vector3& anchor, float floorZ, float grid, BlockoutPlan& plan, QString& error ){
	error.clear();

	QNetworkRequest request;
	QByteArray requestError;
	if ( !GenAI_PrepareOpenAIRequest( request, requestError ) ) {
		error = QString::fromUtf8( requestError ).trimmed();
		return false;
	}

	const QString plannerPrompt = GenAI_openAIPlannerPrompt( options );
	const QByteArray plannerPromptUtf8 = plannerPrompt.toUtf8();
	const QByteArray payload = GenAI_BuildSimpleResponsesPayload( plannerPromptUtf8.constData() );

	QNetworkAccessManager network;
	QNetworkReply* reply = network.post( request, payload );
	if ( reply == nullptr ) {
		error = "Failed to create OpenAI network request.";
		return false;
	}

	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot( true );
	QObject::connect( &timeout, &QTimer::timeout, &loop, &QEventLoop::quit );
	QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
	timeout.start( std::max( g_genAITimeoutMs + 1000, 2000 ) );
	loop.exec();

	if ( !reply->isFinished() ) {
		reply->abort();
		reply->deleteLater();
		error = QString( "OpenAI planner request timed out after %1 ms." ).arg( g_genAITimeoutMs );
		return false;
	}

	timeout.stop();
	const QByteArray body = reply->readAll();
	if ( reply->error() != QNetworkReply::NoError ) {
		const QString message = GenAI_parseOpenAIErrorMessage( body );
		error = message.isEmpty()
		      ? reply->errorString()
		      : QString( "%1 (%2)" ).arg( reply->errorString(), message );
		reply->deleteLater();
		return false;
	}
	reply->deleteLater();

	QJsonParseError parseError;
	const QJsonDocument responseDoc = QJsonDocument::fromJson( body, &parseError );
	if ( parseError.error != QJsonParseError::NoError || !responseDoc.isObject() ) {
		error = QString( "OpenAI response parse error: %1" ).arg( parseError.errorString() );
		return false;
	}

	const QString responseText = GenAI_extractResponseText( responseDoc.object() );
	if ( responseText.isEmpty() ) {
		error = "OpenAI response did not contain text output.";
		return false;
	}

	const QByteArray planJson = GenAI_extractJsonObject( responseText );
	QJsonParseError planParseError;
	const QJsonDocument planDoc = QJsonDocument::fromJson( planJson, &planParseError );
	if ( planParseError.error != QJsonParseError::NoError || !planDoc.isObject() ) {
		error = QString( "OpenAI planner output is not valid JSON: %1" ).arg( planParseError.errorString() );
		return false;
	}

	QString planError;
	if ( !GenAI_buildPlanFromJsonObject( planDoc.object(), options, anchor, floorZ, grid, plan, planError ) ) {
		error = QString( "OpenAI planner JSON was rejected: %1" ).arg( planError );
		return false;
	}

	plan.usedOpenAI = true;
	return true;
}

void GenAI_toggleEnabled(){
	g_genAIEnabled = !g_genAIEnabled;
}

void GenAI_openPreferences(){
	PreferencesDialog_showDialogForQuery( "GenAI" );
}

void GenAI_openDocs(){
	OpenURL( "https://platform.openai.com/docs/api-reference/responses" );
}

bool GenAI_showPromptToBlockoutDialog( PromptToBlockoutOptions& options ){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( i18n::tr( "Prompt-to-Blockout Generator" ) );
	dialog.resize( 760, 620 );

	auto *layout = new QVBoxLayout( &dialog );
	{
		auto *description = new QLabel(
			i18n::tr( "Describe the layout you want. The generator creates blockout brushes near the camera and keeps the result in one undo step." )
		);
		description->setWordWrap( true );
		layout->addWidget( description );
	}

	auto *prompt = new QPlainTextEdit( options.prompt );
	prompt->setPlaceholderText( i18n::tr( "Example: 8-room industrial CTF map with a central atrium and two flanking routes." ) );
	prompt->setMinimumHeight( 140 );
	layout->addWidget( prompt );

	auto *form = new QFormLayout;
	layout->addLayout( form );

	auto *shader = new QLineEdit( options.shader );
	shader->setPlaceholderText( i18n::tr( "Global fallback if specific surface shaders are empty" ) );

	auto *floorShader = new QLineEdit( options.floorShader );
	floorShader->setPlaceholderText( i18n::tr( "Default: common/notex for this game" ) );

	auto *wallShader = new QLineEdit( options.wallShader );
	wallShader->setPlaceholderText( i18n::tr( "Default: common/notex for this game" ) );

	auto *ceilingShader = new QLineEdit( options.ceilingShader );
	ceilingShader->setPlaceholderText( i18n::tr( "Default: common/notex for this game" ) );

	auto *skyShader = new QLineEdit( options.skyShader );
	skyShader->setPlaceholderText( i18n::tr( "Default: common/sky (if available)" ) );

	auto *roomCount = new QSpinBox;
	roomCount->setRange( 2, 24 );
	roomCount->setValue( options.roomCount );

	auto *roomMin = new QSpinBox;
	roomMin->setRange( 64, 4096 );
	roomMin->setSingleStep( 16 );
	roomMin->setValue( options.roomMinSize );

	auto *roomMax = new QSpinBox;
	roomMax->setRange( 64, 8192 );
	roomMax->setSingleStep( 16 );
	roomMax->setValue( options.roomMaxSize );

	auto *roomHeight = new QSpinBox;
	roomHeight->setRange( 64, 2048 );
	roomHeight->setSingleStep( 16 );
	roomHeight->setValue( options.roomHeight );

	auto *corridorWidth = new QSpinBox;
	corridorWidth->setRange( 32, 1024 );
	corridorWidth->setSingleStep( 16 );
	corridorWidth->setValue( options.corridorWidth );

	auto *corridorHeight = new QSpinBox;
	corridorHeight->setRange( 32, 1024 );
	corridorHeight->setSingleStep( 16 );
	corridorHeight->setValue( options.corridorHeight );

	auto *spacing = new QSpinBox;
	spacing->setRange( 64, 4096 );
	spacing->setSingleStep( 16 );
	spacing->setValue( options.roomSpacing );

	auto *seed = new QSpinBox;
	seed->setRange( 0, 2147483647 );
	seed->setValue( options.seed );
	seed->setToolTip( i18n::tr( "0 derives a deterministic seed from the prompt." ) );

	auto *useOpenAIPlanner = new QCheckBox( i18n::tr( "Use OpenAI planner when configured" ) );
	useOpenAIPlanner->setChecked( options.useOpenAIPlanner );

	auto *useSkyCeiling = new QCheckBox( i18n::tr( "Use sky shader on ceilings" ) );
	useSkyCeiling->setChecked( options.useSkyCeiling );

	const bool idTech3Game = GenAI_isIdTech3Game();
	auto *idTech3Caulk = new QCheckBox( i18n::tr( "Use caulk on utility/detail blockout brushes (idTech3)" ) );
	idTech3Caulk->setChecked( options.idTech3Caulk && idTech3Game );
	if ( !idTech3Game ) {
		idTech3Caulk->setChecked( false );
		idTech3Caulk->setEnabled( false );
		idTech3Caulk->setToolTip( i18n::tr( "Only available for idTech3/quake3-format games." ) );
	}

	const bool configured = GenAI_IsConfigured();
	auto *plannerStatus = new QLabel(
		configured
		? i18n::tr( "OpenAI planner is configured and can be used." )
		: i18n::tr( "OpenAI planner is not configured. Deterministic fallback planning will be used." )
	);
	plannerStatus->setWordWrap( true );

	form->addRow( i18n::tr( "Blockout Shader" ), shader );
	form->addRow( i18n::tr( "Floor Shader" ), floorShader );
	form->addRow( i18n::tr( "Wall Shader" ), wallShader );
	form->addRow( i18n::tr( "Ceiling Shader" ), ceilingShader );
	form->addRow( i18n::tr( "Sky Shader" ), skyShader );
	form->addRow( i18n::tr( "Room Count" ), roomCount );
	form->addRow( i18n::tr( "Room Min Size" ), roomMin );
	form->addRow( i18n::tr( "Room Max Size" ), roomMax );
	form->addRow( i18n::tr( "Room Height" ), roomHeight );
	form->addRow( i18n::tr( "Corridor Width" ), corridorWidth );
	form->addRow( i18n::tr( "Corridor Height" ), corridorHeight );
	form->addRow( i18n::tr( "Room Spacing" ), spacing );
	form->addRow( i18n::tr( "Seed" ), seed );
	form->addRow( "", useSkyCeiling );
	form->addRow( "", idTech3Caulk );
	form->addRow( "", useOpenAIPlanner );
	form->addRow( "", plannerStatus );

	QObject::connect( roomMin, static_cast<void(QSpinBox::*)(int)>( &QSpinBox::valueChanged ), [roomMax]( int value ){
		if ( roomMax->value() < value ) {
			roomMax->setValue( value );
		}
	} );

	auto *buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
	layout->addWidget( buttons );
	QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	if ( dialog.exec() != QDialog::DialogCode::Accepted ) {
		return false;
	}

	options.prompt = prompt->toPlainText().trimmed();
	options.shader = shader->text().trimmed();
	options.floorShader = floorShader->text().trimmed();
	options.wallShader = wallShader->text().trimmed();
	options.ceilingShader = ceilingShader->text().trimmed();
	options.skyShader = skyShader->text().trimmed();
	options.roomCount = roomCount->value();
	options.roomMinSize = roomMin->value();
	options.roomMaxSize = roomMax->value();
	options.roomHeight = roomHeight->value();
	options.corridorWidth = corridorWidth->value();
	options.corridorHeight = corridorHeight->value();
	options.roomSpacing = spacing->value();
	options.seed = seed->value();
	options.useSkyCeiling = useSkyCeiling->isChecked();
	options.idTech3Caulk = idTech3Caulk->isChecked() && idTech3Game;
	options.useOpenAIPlanner = useOpenAIPlanner->isChecked();
	return true;
}

void GenAI_promptToBlockout(){
	if ( !g_genAIEnabled ) {
		qt_MessageBox( MainFrame_getWindow(), "GenAI is disabled. Enable GenAI features first.", "GenAI", EMessageBoxType::Warning );
		return;
	}

	PromptToBlockoutOptions options = GenAI_defaultPromptToBlockoutOptions();
	if ( options.shader.trimmed().isEmpty() ) {
		options.shader = GenAI_defaultBlockoutShader();
	}
	if ( options.floorShader.trimmed().isEmpty() ) {
		options.floorShader = GenAI_defaultFloorShader();
	}
	if ( options.wallShader.trimmed().isEmpty() ) {
		options.wallShader = GenAI_defaultWallShader();
	}
	if ( options.ceilingShader.trimmed().isEmpty() ) {
		options.ceilingShader = GenAI_defaultCeilingShader();
	}
	if ( options.skyShader.trimmed().isEmpty() ) {
		options.skyShader = GenAI_defaultSkyShader();
	}
	options.idTech3Caulk = options.idTech3Caulk && GenAI_isIdTech3Game();
	if ( !GenAI_showPromptToBlockoutDialog( options ) ) {
		return;
	}

	options = GenAI_sanitisePromptToBlockoutOptions( options );
	GenAI_storePromptToBlockoutOptions( options );

	const float grid = GenAI_safeGridSize();
	float floorZ = 0.0f;
	const Vector3 anchor = GenAI_blockoutAnchor( grid, floorZ );

	BlockoutPlan plan;
	QString plannerWarning;
	if ( options.useOpenAIPlanner && GenAI_IsConfigured() ) {
		QString error;
		if ( !GenAI_generateOpenAIBlockoutPlan( options, anchor, floorZ, grid, plan, error ) ) {
			plannerWarning = error;
		}
	}
	else if ( options.useOpenAIPlanner && !GenAI_IsConfigured() ) {
		plannerWarning = "OpenAI planner is not configured. Using deterministic fallback planner.";
	}

	if ( plan.rooms.empty() ) {
		GenAI_generateHeuristicBlockoutPlan( options, anchor, floorZ, grid, plan );
	}

	if ( plan.rooms.empty() ) {
		qt_MessageBox( MainFrame_getWindow(), "Prompt-to-Blockout could not generate a valid room plan.", "GenAI", EMessageBoxType::Error );
		return;
	}

	GenAI_applyGameplaySemantics( plan, options, grid );

	const QString shader = options.shader.trimmed().isEmpty() ? GenAI_defaultBlockoutShader() : options.shader.trimmed();
	const QByteArray shaderUtf8 = shader.toUtf8();
	BlockoutBuildResult generated;

	{
		UndoableCommand undo( "genaiPromptToBlockout" );
		generated = GenAI_buildAdvancedBlockout( options, plan, shaderUtf8, grid );
	}

	if ( generated.brushNodes.empty() && generated.entityNodes.empty() ) {
		qt_MessageBox( MainFrame_getWindow(), "Prompt-to-Blockout did not create any geometry or entities.", "GenAI", EMessageBoxType::Warning );
		return;
	}

	const std::set<scene::Node*> createdBrushSet( generated.brushNodes.begin(), generated.brushNodes.end() );
	scene::Node& worldspawn = Map_FindOrInsertWorldspawn( g_map );
	GlobalSelectionSystem().setSelectedAll( false );
	scene::Path rootPath( makeReference( GlobalSceneGraph().root() ) );
	scene::Path worldspawnPath( rootPath );
	worldspawnPath.push( makeReference( worldspawn ) );

	struct SelectCreatedWorldBrushes : public scene::Traversable::Walker
	{
		const std::set<scene::Node*>& created;
		const scene::Path& worldPath;
		SelectCreatedWorldBrushes( const std::set<scene::Node*>& created_, const scene::Path& worldPath_ )
			: created( created_ ), worldPath( worldPath_ ){
		}
		bool pre( scene::Node& node ) const override {
			if ( created.find( &node ) != created.end() ) {
				scene::Path brushPath( worldPath );
				brushPath.push( makeReference( node ) );
				selectPath( brushPath, true );
			}
			return false;
		}
	};
	if ( scene::Traversable* worldTraversable = Node_getTraversable( worldspawn ) ) {
		worldTraversable->traverse( SelectCreatedWorldBrushes( createdBrushSet, worldspawnPath ) );
	}
	for ( scene::Node* entityNode : generated.entityNodes ) {
		if ( entityNode == nullptr ) {
			continue;
		}
		scene::Path entityPath( rootPath );
		entityPath.push( makeReference( *entityNode ) );
		selectPath( entityPath, true );
	}

	QString summary = QString(
		"Generated %1 room-shell brushes, %2 corridor-shell brushes, and %3 detail brushes.\n"
		"Traversal/entities: doors %4, traversal actors %5, spawns %6, items %7.\n"
		"Liquids/platforms/stairs: %8 / %9 / %10.\n"
		"Planner: %11.\n"
		"Playstyle profile: %12."
	).arg( generated.stats.roomShellBrushes )
	 .arg( generated.stats.corridorShellBrushes )
	 .arg( generated.stats.detailBrushes )
	 .arg( generated.stats.doorEntities )
	 .arg( generated.stats.traversalEntities )
	 .arg( generated.stats.spawnEntities )
	 .arg( generated.stats.itemEntities )
	 .arg( generated.stats.liquidBrushes )
	 .arg( generated.stats.platformBrushes )
	 .arg( generated.stats.stairsBrushes )
	 .arg( plan.usedOpenAI ? "OpenAI" : "Deterministic fallback" )
	 .arg( GenAI_playStyleName( GenAI_detectPlayStyle( options.prompt ) ) );

	if ( !plannerWarning.trimmed().isEmpty() ) {
		summary += QString( "\n\nNote: %1" ).arg( plannerWarning.trimmed() );
	}

	const QByteArray summaryUtf8 = summary.toUtf8();
	qt_MessageBox( MainFrame_getWindow(), summaryUtf8.constData(), "Prompt-to-Blockout", EMessageBoxType::Info );
}

void GenAI_showStatus(){
	QString reason;
	const bool configured = GenAI_checkConfigured( &reason );
	const QString model = QString::fromUtf8( g_genAIModel.c_str() ).trimmed();
	const QString baseUrl = GenAI_normaliseBaseUrl();
	const QString endpoint = GenAI_endpointUrl( nullptr ).toString();
	const bool usingStoredKey = !QString::fromUtf8( g_genAIApiKey.c_str() ).trimmed().isEmpty();
	const bool usingEnvKey = !QString::fromUtf8( qgetenv( "OPENAI_API_KEY" ) ).trimmed().isEmpty();

	QString keySource = "none";
	if ( usingStoredKey ) {
		keySource = "stored in preferences";
	}
	else if ( usingEnvKey ) {
		keySource = "OPENAI_API_KEY environment variable";
	}

	const QString message = QString(
		"Enabled: %1\n"
		"Configured: %2\n"
		"Model: %3\n"
		"Base URL: %4\n"
		"Endpoint: %5\n"
		"API key source: %6\n"
		"Timeout: %7 ms\n"
		"Max output tokens: %8\n"
		"Response storage: %9\n"
		"%10"
	).arg( g_genAIEnabled ? "Yes" : "No" )
	 .arg( configured ? "Yes" : "No" )
	 .arg( model.isEmpty() ? "(empty)" : model )
	 .arg( baseUrl.isEmpty() ? "(empty)" : baseUrl )
	 .arg( endpoint.isEmpty() ? "(invalid)" : endpoint )
	 .arg( keySource )
	 .arg( g_genAITimeoutMs )
	 .arg( g_genAIMaxOutputTokens )
	 .arg( g_genAIAllowResponseStore ? "Allowed" : "Disabled (store=false)" )
	 .arg( configured ? QString() : QString( "\nIssue: %1" ).arg( reason ) );

	const QByteArray messageUtf8 = message.toUtf8();
	qt_MessageBox(
		MainFrame_getWindow(),
		messageUtf8.constData(),
		"GenAI Status",
		configured ? EMessageBoxType::Info : EMessageBoxType::Warning
	);
}

void GenAI_clearStoredApiKey(){
	if ( g_genAIApiKey.empty() ) {
		qt_MessageBox( MainFrame_getWindow(), "No stored GenAI API key to clear.", "GenAI" );
		return;
	}
	g_genAIApiKey = "";
	qt_MessageBox( MainFrame_getWindow(), "Stored GenAI API key cleared from preferences state.", "GenAI" );
}

void GenAI_constructPreferences( PreferencesPage& page ){
	QCheckBox* enabled = page.appendCheckBox(
		"OpenAI API",
		i18n::tr( "Enable GenAI features" ).toUtf8().constData(),
		g_genAIEnabled
	);

	QWidget* baseUrl = page.appendEntry( "Base URL", g_genAIBaseUrl );
	QWidget* responsesPath = page.appendEntry( "Responses API Path", g_genAIResponsesPath );
	QWidget* apiKey = page.appendEntry( "API Key", g_genAIApiKey );
	QWidget* model = page.appendEntry( "Model", g_genAIModel );
	QWidget* organization = page.appendEntry( "Organization (optional)", g_genAIOrganization );
	QWidget* project = page.appendEntry( "Project (optional)", g_genAIProject );
	QWidget* timeout = page.appendSpinner( "Request Timeout (ms)", g_genAITimeoutMs, 1000, 300000 );
	QWidget* maxTokens = page.appendSpinner( "Max Output Tokens", g_genAIMaxOutputTokens, 1, 32768 );
	QCheckBox* allowStorage = page.appendCheckBox(
		"Responses API",
		i18n::tr( "Allow API-side response storage" ).toUtf8().constData(),
		g_genAIAllowResponseStore
	);

	QWidget* blockoutPrompt = page.appendEntry( "Default Blockout Prompt", g_genAIBlockoutDefaultPrompt );
	QWidget* blockoutShader = page.appendEntry( "Blockout Shader Override", g_genAIBlockoutShader );
	QWidget* blockoutFloorShader = page.appendEntry( "Blockout Floor Shader", g_genAIBlockoutFloorShader );
	QWidget* blockoutWallShader = page.appendEntry( "Blockout Wall Shader", g_genAIBlockoutWallShader );
	QWidget* blockoutCeilingShader = page.appendEntry( "Blockout Ceiling Shader", g_genAIBlockoutCeilingShader );
	QWidget* blockoutSkyShader = page.appendEntry( "Blockout Sky Shader", g_genAIBlockoutSkyShader );
	QWidget* blockoutRooms = page.appendSpinner( "Blockout Room Count", g_genAIBlockoutRoomCount, 2, 24 );
	QWidget* blockoutRoomMin = page.appendSpinner( "Blockout Room Min Size", g_genAIBlockoutRoomMinSize, 64, 4096 );
	QWidget* blockoutRoomMax = page.appendSpinner( "Blockout Room Max Size", g_genAIBlockoutRoomMaxSize, 64, 8192 );
	QWidget* blockoutRoomHeight = page.appendSpinner( "Blockout Room Height", g_genAIBlockoutRoomHeight, 64, 2048 );
	QWidget* blockoutCorridorWidth = page.appendSpinner( "Blockout Corridor Width", g_genAIBlockoutCorridorWidth, 32, 1024 );
	QWidget* blockoutCorridorHeight = page.appendSpinner( "Blockout Corridor Height", g_genAIBlockoutCorridorHeight, 32, 1024 );
	QWidget* blockoutSpacing = page.appendSpinner( "Blockout Room Spacing", g_genAIBlockoutRoomSpacing, 64, 4096 );
	QWidget* blockoutSeed = page.appendSpinner( "Blockout Seed", g_genAIBlockoutSeed, 0, 2147483647 );
	const bool idTech3Game = GenAI_isIdTech3Game();
	QCheckBox* blockoutUseSkyCeiling = page.appendCheckBox(
		"Prompt-to-Blockout",
		i18n::tr( "Use sky shader on generated ceilings" ).toUtf8().constData(),
		g_genAIBlockoutUseSkyCeiling
	);
	QCheckBox* blockoutIdTech3Caulk = page.appendCheckBox(
		"Prompt-to-Blockout",
		i18n::tr( "Use caulk on utility/detail blockout brushes (idTech3)" ).toUtf8().constData(),
		g_genAIBlockoutIdTech3Caulk
	);
	QCheckBox* blockoutUseOpenAI = page.appendCheckBox(
		"Prompt-to-Blockout",
		i18n::tr( "Use OpenAI planner by default" ).toUtf8().constData(),
		g_genAIBlockoutUseOpenAIPlanner
	);
	if ( !idTech3Game ) {
		blockoutIdTech3Caulk->setChecked( false );
		blockoutIdTech3Caulk->setEnabled( false );
		blockoutIdTech3Caulk->setToolTip( i18n::tr( "Only available for idTech3/quake3-format games." ) );
	}

	if ( QLineEdit* lineEdit = qobject_cast<QLineEdit*>( apiKey ) ) {
		lineEdit->setEchoMode( QLineEdit::PasswordEchoOnEdit );
		lineEdit->setPlaceholderText( i18n::tr( "Optional. Leave blank to use OPENAI_API_KEY" ) );
	}

	Widget_connectToggleDependency( baseUrl, enabled );
	Widget_connectToggleDependency( responsesPath, enabled );
	Widget_connectToggleDependency( apiKey, enabled );
	Widget_connectToggleDependency( model, enabled );
	Widget_connectToggleDependency( organization, enabled );
	Widget_connectToggleDependency( project, enabled );
	Widget_connectToggleDependency( timeout, enabled );
	Widget_connectToggleDependency( maxTokens, enabled );
	Widget_connectToggleDependency( allowStorage, enabled );
	Widget_connectToggleDependency( blockoutPrompt, enabled );
	Widget_connectToggleDependency( blockoutShader, enabled );
	Widget_connectToggleDependency( blockoutFloorShader, enabled );
	Widget_connectToggleDependency( blockoutWallShader, enabled );
	Widget_connectToggleDependency( blockoutCeilingShader, enabled );
	Widget_connectToggleDependency( blockoutSkyShader, enabled );
	Widget_connectToggleDependency( blockoutRooms, enabled );
	Widget_connectToggleDependency( blockoutRoomMin, enabled );
	Widget_connectToggleDependency( blockoutRoomMax, enabled );
	Widget_connectToggleDependency( blockoutRoomHeight, enabled );
	Widget_connectToggleDependency( blockoutCorridorWidth, enabled );
	Widget_connectToggleDependency( blockoutCorridorHeight, enabled );
	Widget_connectToggleDependency( blockoutSpacing, enabled );
	Widget_connectToggleDependency( blockoutSeed, enabled );
	Widget_connectToggleDependency( blockoutUseSkyCeiling, enabled );
	Widget_connectToggleDependency( blockoutIdTech3Caulk, enabled );
	Widget_connectToggleDependency( blockoutUseOpenAI, enabled );
	if ( !idTech3Game ) {
		blockoutIdTech3Caulk->setEnabled( false );
	}

	QPushButton* runPromptToBlockout = page.appendButton( "", "Run Prompt-to-Blockout..." );
	QPushButton* status = page.appendButton( "", "Show GenAI Status" );
	QPushButton* docs = page.appendButton( "", "Open OpenAI API Docs" );
	QPushButton* clearKey = page.appendButton( "", "Clear Stored API Key" );

	Widget_connectToggleDependency( runPromptToBlockout, enabled );
	Widget_connectToggleDependency( status, enabled );
	Widget_connectToggleDependency( docs, enabled );
	Widget_connectToggleDependency( clearKey, enabled );

	QObject::connect( runPromptToBlockout, &QPushButton::clicked, [](){ GenAI_promptToBlockout(); } );
	QObject::connect( status, &QPushButton::clicked, [](){ GenAI_showStatus(); } );
	QObject::connect( docs, &QPushButton::clicked, [](){ GenAI_openDocs(); } );
	QObject::connect( clearKey, &QPushButton::clicked, [](){ GenAI_clearStoredApiKey(); } );
}
} // namespace

void GenAI_Construct(){
	PreferencesDialog_addGenAIPreferences( makeCallbackF( GenAI_constructPreferences ) );

	GlobalPreferenceSystem().registerPreference( "GenAIEnabled", BoolImportStringCaller( g_genAIEnabled ), BoolExportStringCaller( g_genAIEnabled ) );
	GlobalPreferenceSystem().registerPreference( "GenAIAllowResponseStore", BoolImportStringCaller( g_genAIAllowResponseStore ), BoolExportStringCaller( g_genAIAllowResponseStore ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBaseUrl", CopiedStringImportStringCaller( g_genAIBaseUrl ), CopiedStringExportStringCaller( g_genAIBaseUrl ) );
	GlobalPreferenceSystem().registerPreference( "GenAIResponsesPath", CopiedStringImportStringCaller( g_genAIResponsesPath ), CopiedStringExportStringCaller( g_genAIResponsesPath ) );
	GlobalPreferenceSystem().registerPreference( "GenAIApiKey", CopiedStringImportStringCaller( g_genAIApiKey ), CopiedStringExportStringCaller( g_genAIApiKey ) );
	GlobalPreferenceSystem().registerPreference( "GenAIModel", CopiedStringImportStringCaller( g_genAIModel ), CopiedStringExportStringCaller( g_genAIModel ) );
	GlobalPreferenceSystem().registerPreference( "GenAIOrganization", CopiedStringImportStringCaller( g_genAIOrganization ), CopiedStringExportStringCaller( g_genAIOrganization ) );
	GlobalPreferenceSystem().registerPreference( "GenAIProject", CopiedStringImportStringCaller( g_genAIProject ), CopiedStringExportStringCaller( g_genAIProject ) );
	GlobalPreferenceSystem().registerPreference( "GenAITimeoutMs", IntImportStringCaller( g_genAITimeoutMs ), IntExportStringCaller( g_genAITimeoutMs ) );
	GlobalPreferenceSystem().registerPreference( "GenAIMaxOutputTokens", IntImportStringCaller( g_genAIMaxOutputTokens ), IntExportStringCaller( g_genAIMaxOutputTokens ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutDefaultPrompt", CopiedStringImportStringCaller( g_genAIBlockoutDefaultPrompt ), CopiedStringExportStringCaller( g_genAIBlockoutDefaultPrompt ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutShader", CopiedStringImportStringCaller( g_genAIBlockoutShader ), CopiedStringExportStringCaller( g_genAIBlockoutShader ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutFloorShader", CopiedStringImportStringCaller( g_genAIBlockoutFloorShader ), CopiedStringExportStringCaller( g_genAIBlockoutFloorShader ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutWallShader", CopiedStringImportStringCaller( g_genAIBlockoutWallShader ), CopiedStringExportStringCaller( g_genAIBlockoutWallShader ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutCeilingShader", CopiedStringImportStringCaller( g_genAIBlockoutCeilingShader ), CopiedStringExportStringCaller( g_genAIBlockoutCeilingShader ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutSkyShader", CopiedStringImportStringCaller( g_genAIBlockoutSkyShader ), CopiedStringExportStringCaller( g_genAIBlockoutSkyShader ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutRoomCount", IntImportStringCaller( g_genAIBlockoutRoomCount ), IntExportStringCaller( g_genAIBlockoutRoomCount ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutRoomMinSize", IntImportStringCaller( g_genAIBlockoutRoomMinSize ), IntExportStringCaller( g_genAIBlockoutRoomMinSize ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutRoomMaxSize", IntImportStringCaller( g_genAIBlockoutRoomMaxSize ), IntExportStringCaller( g_genAIBlockoutRoomMaxSize ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutRoomHeight", IntImportStringCaller( g_genAIBlockoutRoomHeight ), IntExportStringCaller( g_genAIBlockoutRoomHeight ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutCorridorWidth", IntImportStringCaller( g_genAIBlockoutCorridorWidth ), IntExportStringCaller( g_genAIBlockoutCorridorWidth ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutCorridorHeight", IntImportStringCaller( g_genAIBlockoutCorridorHeight ), IntExportStringCaller( g_genAIBlockoutCorridorHeight ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutRoomSpacing", IntImportStringCaller( g_genAIBlockoutRoomSpacing ), IntExportStringCaller( g_genAIBlockoutRoomSpacing ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutSeed", IntImportStringCaller( g_genAIBlockoutSeed ), IntExportStringCaller( g_genAIBlockoutSeed ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutUseSkyCeiling", BoolImportStringCaller( g_genAIBlockoutUseSkyCeiling ), BoolExportStringCaller( g_genAIBlockoutUseSkyCeiling ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutIdTech3Caulk", BoolImportStringCaller( g_genAIBlockoutIdTech3Caulk ), BoolExportStringCaller( g_genAIBlockoutIdTech3Caulk ) );
	GlobalPreferenceSystem().registerPreference( "GenAIBlockoutUseOpenAIPlanner", BoolImportStringCaller( g_genAIBlockoutUseOpenAIPlanner ), BoolExportStringCaller( g_genAIBlockoutUseOpenAIPlanner ) );

	GlobalToggles_insert( "GenAIEnable", makeCallbackF( GenAI_toggleEnabled ), ToggleItem::AddCallbackCaller( g_genAIEnabledItem ) );
	GlobalCommands_insert( "GenAIPromptToBlockout", makeCallbackF( GenAI_promptToBlockout ) );
	GlobalCommands_insert( "GenAIPreferences", makeCallbackF( GenAI_openPreferences ) );
	GlobalCommands_insert( "GenAIStatus", makeCallbackF( GenAI_showStatus ) );
	GlobalCommands_insert( "GenAIOpenAPIDocs", makeCallbackF( GenAI_openDocs ) );
	GlobalCommands_insert( "GenAIClearAPIKey", makeCallbackF( GenAI_clearStoredApiKey ) );
}

void GenAI_Destroy(){
}

bool GenAI_IsEnabled(){
	return g_genAIEnabled;
}

bool GenAI_IsConfigured(){
	return GenAI_checkConfigured( nullptr );
}

bool GenAI_PrepareOpenAIRequest( QNetworkRequest& request, QByteArray& error ){
	error.clear();

	QString reason;
	if ( !GenAI_checkConfigured( &reason ) ) {
		error = reason.toUtf8();
		return false;
	}

	QString endpointError;
	const QUrl endpoint = GenAI_endpointUrl( &endpointError );
	if ( !endpoint.isValid() ) {
		error = endpointError.toUtf8();
		return false;
	}

	request.setUrl( endpoint );
	request.setHeader( QNetworkRequest::ContentTypeHeader, QStringLiteral( "application/json" ) );
	request.setRawHeader( "Authorization", QByteArray( "Bearer " ) + GenAI_effectiveApiKey().toUtf8() );

	const QString org = QString::fromUtf8( g_genAIOrganization.c_str() ).trimmed();
	if ( !org.isEmpty() ) {
		request.setRawHeader( "OpenAI-Organization", org.toUtf8() );
	}

	const QString project = QString::fromUtf8( g_genAIProject.c_str() ).trimmed();
	if ( !project.isEmpty() ) {
		request.setRawHeader( "OpenAI-Project", project.toUtf8() );
	}

#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
	request.setTransferTimeout( g_genAITimeoutMs );
#endif

	return true;
}

QByteArray GenAI_BuildSimpleResponsesPayload( const char* prompt ){
	QJsonObject payload;
	payload.insert( "model", QString::fromUtf8( g_genAIModel.c_str() ) );
	payload.insert( "input", QString::fromUtf8( prompt == nullptr ? "" : prompt ) );
	payload.insert( "max_output_tokens", g_genAIMaxOutputTokens );
	if ( !g_genAIAllowResponseStore ) {
		payload.insert( "store", false );
	}
	return QJsonDocument( payload ).toJson( QJsonDocument::Compact );
}
