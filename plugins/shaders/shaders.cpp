/*
   Copyright (c) 2001, Loki software, inc.
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this list
   of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of Loki software nor the names of its contributors may be used
   to endorse or promote products derived from this software without specific prior
   written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//
// Shaders Manager Plugin
//
// Leonardo Zide (leo@lokigames.com)
//

#include "shaders.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <map>
#include <list>

#include "ifilesystem.h"
#include "ishaders.h"
#include "iscriplib.h"
#include "itextures.h"
#include "qerplugin.h"
#include "irender.h"

#include "debugging/debugging.h"
#include "string/pooledstring.h"
#include "math/vector.h"
#include "math/matrix.h"
#include "math/pi.h"
#include "generic/callback.h"
#include "generic/referencecounted.h"
#include "stream/memstream.h"
#include "stream/stringstream.h"
#include "stream/textfilestream.h"
#include "os/path.h"
#include "os/dir.h"
#include "os/file.h"
#include "stringio.h"
#include "shaderlib.h"
#include "texturelib.h"
#include "commandlib.h"
#include "moduleobservers.h"
#include "archivelib.h"
#include "imagelib.h"

const char* g_shadersExtension = "";
const char* g_shadersDirectory = "";
bool g_enableDefaultShaders = true;
ShaderLanguage g_shaderLanguage = SHADERLANGUAGE_QUAKE3;
bool g_enableQ3ShaderStages = false;
bool g_useShaderList = true;
_QERPlugImageTable* g_bitmapModule = 0;
const char* g_texturePrefix = "textures/";

void ActiveShaders_IteratorBegin();
bool ActiveShaders_IteratorAtEnd();
IShader *ActiveShaders_IteratorCurrent();
void ActiveShaders_IteratorIncrement();
Callback<void()> g_ActiveShadersChangedNotify;

void FreeShaders();
void LoadShaderFile( const char *filename );

/*!
   NOTE TTimo: there is an important distinction between SHADER_NOT_FOUND and SHADER_NOTEX:
   SHADER_NOT_FOUND means we didn't find the raw texture or the shader for this
   SHADER_NOTEX means we recognize this as a shader script, but we are missing the texture to represent it
   this was in the initial design of the shader code since early GtkRadiant alpha, and got sort of foxed in 1.2 and put back in
 */

Image* loadBitmap( void* environment, const char* name ){
	DirectoryArchiveFile file( name, name );
	if ( !file.failed() ) {
		return g_bitmapModule->loadImage( file );
	}
	return 0;
}

inline byte* getPixel( byte* pixels, int width, int height, int x, int y ){
	return pixels + ( ( ( ( ( y + height ) % height ) * width ) + ( ( x + width ) % width ) ) * 4 );
}

class KernelElement
{
public:
	int x, y;
	float w;
};

Image& convertHeightmapToNormalmap( Image& heightmap, float scale ){
	int w = heightmap.getWidth();
	int h = heightmap.getHeight();

	Image& normalmap = *( new RGBAImage( heightmap.getWidth(), heightmap.getHeight() ) );

	byte* in = heightmap.getRGBAPixels();
	byte* out = normalmap.getRGBAPixels();

#if 1
	// no filtering
	const int kernelSize = 2;
	KernelElement kernel_du[kernelSize] = {
		{-1, 0,-0.5f },
		{ 1, 0, 0.5f }
	};
	KernelElement kernel_dv[kernelSize] = {
		{ 0, 1, 0.5f },
		{ 0,-1,-0.5f }
	};
#else
	// 3x3 Prewitt
	const int kernelSize = 6;
	KernelElement kernel_du[kernelSize] = {
		{-1, 1,-1 },
		{-1, 0,-1 },
		{-1,-1,-1 },
		{ 1, 1, 1 },
		{ 1, 0, 1 },
		{ 1,-1, 1 }
	};
	KernelElement kernel_dv[kernelSize] = {
		{-1, 1, 1 },
		{ 0, 1, 1 },
		{ 1, 1, 1 },
		{-1,-1,-1 },
		{ 0,-1,-1 },
		{ 1,-1,-1 }
	};
#endif

	int x, y = 0;
	while ( y < h )
	{
		x = 0;
		while ( x < w )
		{
			float du = 0;
			for ( KernelElement* i = kernel_du; i != kernel_du + kernelSize; ++i )
			{
				du += ( getPixel( in, w, h, x + ( *i ).x, y + ( *i ).y )[0] / 255.0 ) * ( *i ).w;
			}
			float dv = 0;
			for ( KernelElement* i = kernel_dv; i != kernel_dv + kernelSize; ++i )
			{
				dv += ( getPixel( in, w, h, x + ( *i ).x, y + ( *i ).y )[0] / 255.0 ) * ( *i ).w;
			}

			float nx = -du * scale;
			float ny = -dv * scale;
			float nz = 1;

			// Normalize
			float norm = 1.0 / sqrt( nx * nx + ny * ny + nz * nz );
			out[0] = float_to_integer( ( ( nx * norm ) + 1 ) * 127.5 );
			out[1] = float_to_integer( ( ( ny * norm ) + 1 ) * 127.5 );
			out[2] = float_to_integer( ( ( nz * norm ) + 1 ) * 127.5 );
			out[3] = 255;

			x++;
			out += 4;
		}

		y++;
	}

	return normalmap;
}

Image* loadHeightmap( void* environment, const char* name ){
	Image* heightmap = GlobalTexturesCache().loadImage( name );
	if ( heightmap != 0 ) {
		Image& normalmap = convertHeightmapToNormalmap( *heightmap, *reinterpret_cast<float*>( environment ) );
		heightmap->release();
		return &normalmap;
	}
	return 0;
}


Image* createSolidImage( byte r, byte g, byte b, byte a ){
	RGBAImage* image = new RGBAImage( 1, 1 );
	image->pixels[0].red = r;
	image->pixels[0].green = g;
	image->pixels[0].blue = b;
	image->pixels[0].alpha = a;
	return image;
}

Image* loadSpecial( void* environment, const char* name ){
	if ( string_equal_nocase( name, "$whiteimage" ) || string_equal_nocase( name, "$lightmap" ) ) {
		return createSolidImage( 255, 255, 255, 255 );
	}
	if ( string_equal_nocase( name, "$blackimage" ) ) {
		return createSolidImage( 0, 0, 0, 255 );
	}
	if ( *name == '_' ) { // special image
		Image* image = loadBitmap( environment, StringStream( GlobalRadiant().getAppPath(), "bitmaps/", name + 1, ".png" ) );
		if ( image != 0 ) {
			return image;
		}
	}
	return GlobalTexturesCache().loadImage( name );
}

class ShaderPoolContext
{
};
typedef Static<StringPool, ShaderPoolContext> ShaderPool;
typedef PooledString<ShaderPool> ShaderString;
typedef ShaderString ShaderVariable;
typedef ShaderString ShaderValue;
typedef CopiedString TextureExpression;

// clean a texture name to the qtexture_t name format we use internally
// NOTE: case sensitivity: the engine is case sensitive. we store the shader name with case information and save with case
// information as well. but we assume there won't be any case conflict and so when doing lookups based on shader name,
// we compare as case insensitive. That is Radiant is case insensitive, but knows that the engine is case sensitive.
//++timo FIXME: we need to put code somewhere to detect when two shaders that are case insensitive equal are present
template<typename StringType>
void parseTextureName( StringType& name, const char* token ){
	name = StringStream<64>( PathCleaned( PathExtensionless( token ) ) ).c_str(); // remove extension
}

bool Tokeniser_parseTextureName( Tokeniser& tokeniser, TextureExpression& name ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#texture-name" );
		return false;
	}
	parseTextureName( name, token );
	return true;
}

bool Tokeniser_parseShaderName( Tokeniser& tokeniser, CopiedString& name ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#shader-name" );
		return false;
	}
	parseTextureName( name, token );
	return true;
}

bool Tokeniser_parseString( Tokeniser& tokeniser, ShaderString& string ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#string" );
		return false;
	}
	string = token;
	return true;
}



typedef std::list<ShaderVariable> ShaderParameters;
typedef std::list<ShaderVariable> ShaderArguments;

typedef std::pair<ShaderVariable, ShaderVariable> BlendFuncExpression;

enum Q3WaveFunc
{
	Q3_WAVE_SIN,
	Q3_WAVE_TRIANGLE,
	Q3_WAVE_SQUARE,
	Q3_WAVE_SAWTOOTH,
	Q3_WAVE_INVERSESAWTOOTH,
	Q3_WAVE_NOISE,
};

struct Q3WaveForm
{
	Q3WaveFunc func;
	float base;
	float amplitude;
	float phase;
	float frequency;

	Q3WaveForm() :
		func( Q3_WAVE_SIN ),
		base( 0.0f ),
		amplitude( 1.0f ),
		phase( 0.0f ),
		frequency( 1.0f ){
	}
};

enum Q3RgbGenType
{
	Q3_RGB_IDENTITY,
	Q3_RGB_IDENTITY_LIGHTING,
	Q3_RGB_CONST,
	Q3_RGB_WAVE,
	Q3_RGB_VERTEX,
	Q3_RGB_EXACTVERTEX,
	Q3_RGB_ENTITY,
	Q3_RGB_ONE_MINUS_ENTITY,
	Q3_RGB_LIGHTING_DIFFUSE,
	Q3_RGB_ONE_MINUS_VERTEX,
};

enum Q3AlphaGenType
{
	Q3_ALPHA_IDENTITY,
	Q3_ALPHA_CONST,
	Q3_ALPHA_WAVE,
	Q3_ALPHA_VERTEX,
	Q3_ALPHA_ONE_MINUS_VERTEX,
	Q3_ALPHA_ENTITY,
	Q3_ALPHA_ONE_MINUS_ENTITY,
	Q3_ALPHA_PORTAL,
	Q3_ALPHA_LIGHTING_SPECULAR,
};

enum Q3TcModType
{
	Q3_TCMOD_SCROLL,
	Q3_TCMOD_SCALE,
	Q3_TCMOD_ROTATE,
	Q3_TCMOD_STRETCH,
	Q3_TCMOD_TRANSFORM,
	Q3_TCMOD_TURB,
};

struct Q3TcMod
{
	Q3TcModType type;
	float params[6];
	Q3WaveForm wave;

	Q3TcMod() :
		type( Q3_TCMOD_SCROLL ),
		params{ 0, 0, 0, 0, 0, 0 },
		wave(){
	}
};

struct Q3StageTemplate
{
	enum MapType
	{
		MAP_NONE,
		MAP_TEXTURE,
		MAP_CLAMP,
		MAP_ANIM,
		MAP_ANIM_CLAMP,
	};

	MapType mapType;
	TextureExpression map;
	std::vector<TextureExpression> animMaps;
	float animFps;

	bool hasBlendFunc;
	BlendFunc blendFunc;

	Q3RgbGenType rgbGenType;
	Vector3 rgbConst;
	Q3WaveForm rgbWave;

	Q3AlphaGenType alphaGenType;
	float alphaConst;
	Q3WaveForm alphaWave;
	float alphaPortalRange;

	ShaderStageAlphaFunc alphaFunc;
	ShaderStageDepthFunc depthFunc;
	bool depthWrite;
	bool detail;

	ShaderStageTcGen tcGen;
	Vector3 tcGenVec0;
	Vector3 tcGenVec1;
	std::vector<Q3TcMod> tcMods;

	Q3StageTemplate() :
		mapType( MAP_NONE ),
		animFps( 0.0f ),
		hasBlendFunc( false ),
		blendFunc( BLEND_ONE, BLEND_ZERO ),
		rgbGenType( Q3_RGB_IDENTITY ),
		rgbConst( 1, 1, 1 ),
		rgbWave(),
		alphaGenType( Q3_ALPHA_IDENTITY ),
		alphaConst( 1.0f ),
		alphaWave(),
		alphaPortalRange( 0.0f ),
		alphaFunc( eStageAlphaNone ),
		depthFunc( eStageDepthNone ),
		depthWrite( false ),
		detail( false ),
		tcGen( eTcGenBase ),
		tcGenVec0( 1, 0, 0 ),
		tcGenVec1( 0, 1, 0 ),
		tcMods(){
	}
};

struct Q3Stage
{
	std::vector<qtexture_t*> textures;
	float animFps;
	bool clampToEdge;
	bool hasBlendFunc;
	BlendFunc blendFunc;
	Q3RgbGenType rgbGenType;
	Vector3 rgbConst;
	Q3WaveForm rgbWave;
	Q3AlphaGenType alphaGenType;
	float alphaConst;
	Q3WaveForm alphaWave;
	float alphaPortalRange;
	ShaderStageAlphaFunc alphaFunc;
	ShaderStageDepthFunc depthFunc;
	bool depthWrite;
	bool detail;
	ShaderStageTcGen tcGen;
	Vector3 tcGenVec0;
	Vector3 tcGenVec1;
	std::vector<Q3TcMod> tcMods;
	bool usesVertexColour;
	bool animated;

	Q3Stage() :
		animFps( 0.0f ),
		clampToEdge( false ),
		hasBlendFunc( false ),
		blendFunc( BLEND_ONE, BLEND_ZERO ),
		rgbGenType( Q3_RGB_IDENTITY ),
		rgbConst( 1, 1, 1 ),
		rgbWave(),
		alphaGenType( Q3_ALPHA_IDENTITY ),
		alphaConst( 1.0f ),
		alphaWave(),
		alphaPortalRange( 0.0f ),
		alphaFunc( eStageAlphaNone ),
		depthFunc( eStageDepthNone ),
		depthWrite( false ),
		detail( false ),
		tcGen( eTcGenBase ),
		tcGenVec0( 1, 0, 0 ),
		tcGenVec1( 0, 1, 0 ),
		tcMods(),
		usesVertexColour( false ),
		animated( false ){
	}
};

float Q3Shader_waveValue( const Q3WaveForm& wave, float time );
float Q3Shader_clamp01( float value );
Matrix4 Q3Shader_buildTexMatrix( const Q3Stage& stage, float time );

class ShaderTemplate
{
	std::size_t m_refcount;
	CopiedString m_Name;
public:

	ShaderParameters m_params;

	TextureExpression m_textureName;
	TextureExpression m_skyBox;
	TextureExpression m_diffuse;
	TextureExpression m_bump;
	ShaderValue m_heightmapScale;
	TextureExpression m_specular;
	TextureExpression m_lightFalloffImage;

	int m_nFlags;
	float m_fTrans;

// alphafunc stuff
	IShader::EAlphaFunc m_AlphaFunc;
	float m_AlphaRef;
// cull stuff
	IShader::ECull m_Cull;

	ShaderTemplate() :
		m_refcount( 0 ){
		m_nFlags = 0;
		m_fTrans = 1;
	}

	void IncRef(){
		++m_refcount;
	}
	void DecRef(){
		ASSERT_MESSAGE( m_refcount != 0, "shader reference-count going below zero" );
		if ( --m_refcount == 0 ) {
			delete this;
		}
	}

	std::size_t refcount(){
		return m_refcount;
	}

	const char* getName() const {
		return m_Name.c_str();
	}
	void setName( const char* name ){
		m_Name = name;
	}

// -----------------------------------------

	bool parseDoom3( Tokeniser& tokeniser );
	bool parseQuake3( Tokeniser& tokeniser );
	bool parseTemplate( Tokeniser& tokeniser );


	void CreateDefault( const char *name ){
		if ( g_enableDefaultShaders ) {
			m_textureName = name;
		}
		else
		{
			m_textureName = "";
		}
		setName( name );
	}


	class MapLayerTemplate
	{
		TextureExpression m_texture;
		BlendFuncExpression m_blendFunc;
		bool m_clampToBorder;
		ShaderValue m_alphaTest;
	public:
		MapLayerTemplate( const TextureExpression& texture, const BlendFuncExpression& blendFunc, bool clampToBorder, const ShaderValue& alphaTest ) :
			m_texture( texture ),
			m_blendFunc( blendFunc ),
			m_clampToBorder( false ),
			m_alphaTest( alphaTest ){
		}
		const TextureExpression& texture() const {
			return m_texture;
		}
		const BlendFuncExpression& blendFunc() const {
			return m_blendFunc;
		}
		bool clampToBorder() const {
			return m_clampToBorder;
		}
		const ShaderValue& alphaTest() const {
			return m_alphaTest;
		}
	};
	typedef std::vector<MapLayerTemplate> MapLayers;
	MapLayers m_layers;

	typedef std::vector<Q3StageTemplate> Q3StageTemplates;
	Q3StageTemplates m_q3Stages;
};


bool Doom3Shader_parseHeightmap( Tokeniser& tokeniser, TextureExpression& bump, ShaderValue& heightmapScale ){
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, bump ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "," ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseString( tokeniser, heightmapScale ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
	return true;
}

bool Doom3Shader_parseAddnormals( Tokeniser& tokeniser, TextureExpression& bump ){
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, bump ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "," ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "heightmap" ) );
	TextureExpression heightmapName;
	ShaderValue heightmapScale;
	RETURN_FALSE_IF_FAIL( Doom3Shader_parseHeightmap( tokeniser, heightmapName, heightmapScale ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
	return true;
}

bool Doom3Shader_parseBumpmap( Tokeniser& tokeniser, TextureExpression& bump, ShaderValue& heightmapScale ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#bumpmap" );
		return false;
	}
	if ( string_equal( token, "heightmap" ) ) {
		RETURN_FALSE_IF_FAIL( Doom3Shader_parseHeightmap( tokeniser, bump, heightmapScale ) );
	}
	else if ( string_equal( token, "addnormals" ) ) {
		RETURN_FALSE_IF_FAIL( Doom3Shader_parseAddnormals( tokeniser, bump ) );
	}
	else
	{
		parseTextureName( bump, token );
	}
	return true;
}

enum LayerTypeId
{
	LAYER_NONE,
	LAYER_BLEND,
	LAYER_DIFFUSEMAP,
	LAYER_BUMPMAP,
	LAYER_SPECULARMAP
};

class LayerTemplate
{
public:
	LayerTypeId m_type;
	TextureExpression m_texture;
	BlendFuncExpression m_blendFunc;
	bool m_clampToBorder;
	ShaderValue m_alphaTest;
	ShaderValue m_heightmapScale;

	LayerTemplate() : m_type( LAYER_NONE ), m_blendFunc( "GL_ONE", "GL_ZERO" ), m_clampToBorder( false ), m_alphaTest( "-1" ), m_heightmapScale( "0" ){
	}
};

bool parseShaderParameters( Tokeniser& tokeniser, ShaderParameters& params ){
	Tokeniser_parseToken( tokeniser, "(" );
	for (;; )
	{
		const char* param = tokeniser.getToken();
		if ( string_equal( param, ")" ) ) {
			break;
		}
		params.push_back( param );
		const char* comma = tokeniser.getToken();
		if ( string_equal( comma, ")" ) ) {
			break;
		}
		if ( !string_equal( comma, "," ) ) {
			Tokeniser_unexpectedError( tokeniser, comma, "," );
			return false;
		}
	}
	return true;
}

bool Q3Shader_parseVec3( Tokeniser& tokeniser, Vector3& value ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#vector3" );
		return false;
	}
	if ( string_equal( token, "(" ) ) {
		RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value.x() ) );
		RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value.y() ) );
		RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value.z() ) );
		RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
		return true;
	}
	if ( !string_parse_float( token, value.x() ) ) {
		Tokeniser_unexpectedError( tokeniser, token, "#number" );
		return false;
	}
	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value.y() ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value.z() ) );
	return true;
}

bool Q3Shader_parseConstColor( Tokeniser& tokeniser, Vector3& value ){
	return Q3Shader_parseVec3( tokeniser, value );
}

bool Q3Shader_parseConstAlpha( Tokeniser& tokeniser, float& value ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		Tokeniser_unexpectedError( tokeniser, token, "#number" );
		return false;
	}
	if ( string_equal( token, "(" ) ) {
		RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, value ) );
		RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
		return true;
	}
	if ( !string_parse_float( token, value ) ) {
		Tokeniser_unexpectedError( tokeniser, token, "#number" );
		return false;
	}
	return true;
}

bool Q3Shader_parseWaveForm( Tokeniser& tokeniser, Q3WaveForm& wave ){
	const char* func = tokeniser.getToken();
	if ( func == 0 ) {
		Tokeniser_unexpectedError( tokeniser, func, "#wavefunc" );
		return false;
	}
	if ( string_equal_nocase( func, "sin" ) ) {
		wave.func = Q3_WAVE_SIN;
	}
	else if ( string_equal_nocase( func, "triangle" ) ) {
		wave.func = Q3_WAVE_TRIANGLE;
	}
	else if ( string_equal_nocase( func, "square" ) ) {
		wave.func = Q3_WAVE_SQUARE;
	}
	else if ( string_equal_nocase( func, "sawtooth" ) ) {
		wave.func = Q3_WAVE_SAWTOOTH;
	}
	else if ( string_equal_nocase( func, "inversesawtooth" ) || string_equal_nocase( func, "inverseSawtooth" ) ) {
		wave.func = Q3_WAVE_INVERSESAWTOOTH;
	}
	else if ( string_equal_nocase( func, "noise" ) ) {
		wave.func = Q3_WAVE_NOISE;
	}
	else{
		Tokeniser_unexpectedError( tokeniser, func, "#wavefunc" );
		return false;
	}

	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, wave.base ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, wave.amplitude ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, wave.phase ) );
	RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, wave.frequency ) );
	return true;
}

BlendFactor Q3Shader_parseBlendFactor( const char* token ){
	if ( string_equal_nocase( token, "gl_zero" ) ) {
		return BLEND_ZERO;
	}
	if ( string_equal_nocase( token, "gl_one" ) ) {
		return BLEND_ONE;
	}
	if ( string_equal_nocase( token, "gl_src_color" ) ) {
		return BLEND_SRC_COLOUR;
	}
	if ( string_equal_nocase( token, "gl_one_minus_src_color" ) ) {
		return BLEND_ONE_MINUS_SRC_COLOUR;
	}
	if ( string_equal_nocase( token, "gl_src_alpha" ) ) {
		return BLEND_SRC_ALPHA;
	}
	if ( string_equal_nocase( token, "gl_one_minus_src_alpha" ) ) {
		return BLEND_ONE_MINUS_SRC_ALPHA;
	}
	if ( string_equal_nocase( token, "gl_dst_color" ) ) {
		return BLEND_DST_COLOUR;
	}
	if ( string_equal_nocase( token, "gl_one_minus_dst_color" ) ) {
		return BLEND_ONE_MINUS_DST_COLOUR;
	}
	if ( string_equal_nocase( token, "gl_dst_alpha" ) ) {
		return BLEND_DST_ALPHA;
	}
	if ( string_equal_nocase( token, "gl_one_minus_dst_alpha" ) ) {
		return BLEND_ONE_MINUS_DST_ALPHA;
	}
	if ( string_equal_nocase( token, "gl_src_alpha_saturate" ) ) {
		return BLEND_SRC_ALPHA_SATURATE;
	}
	return BLEND_ZERO;
}

bool Q3Shader_isStageDirective( const char* token ){
	static const char* directives[] = {
		"map",
		"clampmap",
		"animmap",
		"clampanimmap",
		"videomap",
		"blendfunc",
		"rgbgen",
		"alphagen",
		"tcgen",
		"tcmod",
		"alphafunc",
		"depthfunc",
		"depthwrite",
		"detail",
	};
	for ( const char* directive : directives )
	{
		if ( string_equal_nocase( token, directive ) ) {
			return true;
		}
	}
	return false;
}

bool ShaderTemplate::parseTemplate( Tokeniser& tokeniser ){
	m_Name = tokeniser.getToken();
	if ( !parseShaderParameters( tokeniser, m_params ) ) {
		globalErrorStream() << "shader template: " << Quoted( m_Name ) << ": parameter parse failed\n";
		return false;
	}

	return parseDoom3( tokeniser );
}

bool ShaderTemplate::parseDoom3( Tokeniser& tokeniser ){
	LayerTemplate currentLayer;
	bool isFog = false;

	// we need to read until we hit a balanced }
	int depth = 0;
	for (;; )
	{
		tokeniser.nextLine();
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			return false;
		}

		if ( string_equal( token, "{" ) ) {
			++depth;
			continue;
		}
		else if ( string_equal( token, "}" ) ) {
			--depth;
			if ( depth < 0 ) { // error
				return false;
			}
			if ( depth == 0 ) { // end of shader
				break;
			}
			if ( depth == 1 ) { // end of layer
				if ( currentLayer.m_type == LAYER_DIFFUSEMAP ) {
					m_diffuse = currentLayer.m_texture;
				}
				else if ( currentLayer.m_type == LAYER_BUMPMAP ) {
					m_bump = currentLayer.m_texture;
				}
				else if ( currentLayer.m_type == LAYER_SPECULARMAP ) {
					m_specular = currentLayer.m_texture;
				}
				else if ( !currentLayer.m_texture.empty() ) {
					m_layers.push_back( MapLayerTemplate(
					                        currentLayer.m_texture.c_str(),
					                        currentLayer.m_blendFunc,
					                        currentLayer.m_clampToBorder,
					                        currentLayer.m_alphaTest
					                    ) );
				}
				currentLayer.m_type = LAYER_NONE;
				currentLayer.m_texture = "";
			}
			continue;
		}

		if ( depth == 2 ) { // in layer
			if ( string_equal_nocase( token, "blend" ) ) {
				const char* blend = tokeniser.getToken();

				if ( blend == 0 ) {
					Tokeniser_unexpectedError( tokeniser, blend, "#blend" );
					return false;
				}

				if ( string_equal_nocase( blend, "diffusemap" ) ) {
					currentLayer.m_type = LAYER_DIFFUSEMAP;
				}
				else if ( string_equal_nocase( blend, "bumpmap" ) ) {
					currentLayer.m_type = LAYER_BUMPMAP;
				}
				else if ( string_equal_nocase( blend, "specularmap" ) ) {
					currentLayer.m_type = LAYER_SPECULARMAP;
				}
				else
				{
					currentLayer.m_blendFunc.first = blend;

					const char* comma = tokeniser.getToken();

					if ( comma == 0 ) {
						Tokeniser_unexpectedError( tokeniser, comma, "#comma" );
						return false;
					}

					if ( string_equal( comma, "," ) ) {
						RETURN_FALSE_IF_FAIL( Tokeniser_parseString( tokeniser, currentLayer.m_blendFunc.second ) );
					}
					else
					{
						currentLayer.m_blendFunc.second = "";
						tokeniser.ungetToken();
					}
				}
			}
			else if ( string_equal_nocase( token, "map" ) ) {
				if ( currentLayer.m_type == LAYER_BUMPMAP ) {
					RETURN_FALSE_IF_FAIL( Doom3Shader_parseBumpmap( tokeniser, currentLayer.m_texture, currentLayer.m_heightmapScale ) );
				}
				else
				{
					const char* map = tokeniser.getToken();

					if ( map == 0 ) {
						Tokeniser_unexpectedError( tokeniser, map, "#map" );
						return false;
					}

					if ( string_equal( map, "makealpha" ) ) {
						RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
						const char* texture = tokeniser.getToken();
						if ( texture == 0 ) {
							Tokeniser_unexpectedError( tokeniser, texture, "#texture" );
							return false;
						}
						currentLayer.m_texture = texture;
						RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
					}
					else
					{
						parseTextureName( currentLayer.m_texture, map );
					}
				}
			}
			else if ( string_equal_nocase( token, "zeroclamp" ) ) {
				currentLayer.m_clampToBorder = true;
			}
#if 0
			else if ( string_equal_nocase( token, "alphaTest" ) ) {
				Tokeniser_getFloat( tokeniser, currentLayer.m_alphaTest );
			}
#endif
		}
		else if ( depth == 1 ) {
			if ( string_equal_nocase( token, "qer_editorimage" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_textureName ) );
			}
			else if ( string_equal_nocase( token, "qer_trans" ) ) {
				m_fTrans = string_read_float( tokeniser.getToken() );
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "translucent" ) ) {
				m_fTrans = 1;
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal( token, "DECAL_MACRO" ) ) {
				m_fTrans = 1;
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "bumpmap" ) ) {
				RETURN_FALSE_IF_FAIL( Doom3Shader_parseBumpmap( tokeniser, m_bump, m_heightmapScale ) );
			}
			else if ( string_equal_nocase( token, "diffusemap" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_diffuse ) );
			}
			else if ( string_equal_nocase( token, "specularmap" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_specular ) );
			}
			else if ( string_equal_nocase( token, "twosided" ) ) {
				m_Cull = IShader::eCullNone;
				m_nFlags |= QER_CULL;
			}
			else if ( string_equal_nocase( token, "nodraw" ) ) {
				m_nFlags |= QER_NODRAW;
			}
			else if ( string_equal_nocase( token, "nonsolid" ) ) {
				m_nFlags |= QER_NONSOLID;
			}
			else if ( string_equal_nocase( token, "liquid" ) ) {
				m_nFlags |= QER_LIQUID;
			}
			else if ( string_equal_nocase( token, "areaportal" ) ) {
				m_nFlags |= QER_AREAPORTAL;
			}
			else if ( string_equal_nocase( token, "playerclip" )
			       || string_equal_nocase( token, "monsterclip" )
			       || string_equal_nocase( token, "ikclip" )
			       || string_equal_nocase( token, "moveableclip" ) ) {
				m_nFlags |= QER_CLIP;
			}
			if ( string_equal_nocase( token, "fogLight" ) ) {
				isFog = true;
			}
			else if ( !isFog && string_equal_nocase( token, "lightFalloffImage" ) ) {
				const char* lightFalloffImage = tokeniser.getToken();
				if ( lightFalloffImage == 0 ) {
					Tokeniser_unexpectedError( tokeniser, lightFalloffImage, "#lightFalloffImage" );
					return false;
				}
				if ( string_equal_nocase( lightFalloffImage, "makeintensity" ) ) {
					RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, "(" ) );
					TextureExpression name;
					RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, name ) );
					m_lightFalloffImage = name;
					RETURN_FALSE_IF_FAIL( Tokeniser_parseToken( tokeniser, ")" ) );
				}
				else
				{
					m_lightFalloffImage = lightFalloffImage;
				}
			}
		}
	}

	if ( m_textureName.empty() ) {
		m_textureName = m_diffuse;
	}

	return true;
}

typedef SmartPointer<ShaderTemplate> ShaderTemplatePointer;
typedef std::map<CopiedString, ShaderTemplatePointer> ShaderTemplateMap;

ShaderTemplateMap g_shaders;
ShaderTemplateMap g_shaderTemplates;

ShaderTemplate* findTemplate( const char* name ){
	ShaderTemplateMap::iterator i = g_shaderTemplates.find( name );
	if ( i != g_shaderTemplates.end() ) {
		return ( *i ).second.get();
	}
	return 0;
}

class ShaderDefinition
{
public:
	ShaderDefinition( ShaderTemplate* shaderTemplate, const ShaderArguments& args, const char* filename )
		: shaderTemplate( shaderTemplate ), args( args ), filename( filename ){
	}
	ShaderTemplate* shaderTemplate;
	ShaderArguments args;
	const char* filename;
};

typedef std::map<CopiedString, ShaderDefinition, shader_less_t> ShaderDefinitionMap;

ShaderDefinitionMap g_shaderDefinitions;

bool parseTemplateInstance( Tokeniser& tokeniser, const char* filename ){
	CopiedString name;
	RETURN_FALSE_IF_FAIL( Tokeniser_parseShaderName( tokeniser, name ) );
	const char* templateName = tokeniser.getToken();
	ShaderTemplate* shaderTemplate = findTemplate( templateName );
	if ( shaderTemplate == 0 ) {
		globalErrorStream() << "shader instance: " << Quoted( name ) << ": shader template not found: " << Quoted( templateName ) << '\n';
	}

	ShaderArguments args;
	if ( !parseShaderParameters( tokeniser, args ) ) {
		globalErrorStream() << "shader instance: " << Quoted( name ) << ": argument parse failed\n";
		return false;
	}

	if ( shaderTemplate != 0 ) {
		if ( !g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( name, ShaderDefinition( shaderTemplate, args, filename ) ) ).second ) {
			globalErrorStream() << "shader instance: " << Quoted( name ) << ": already exists, second definition ignored\n";
		}
	}
	return true;
}


const char* evaluateShaderValue( const char* value, const ShaderParameters& params, const ShaderArguments& args ){
	ShaderArguments::const_iterator j = args.begin();
	for ( ShaderParameters::const_iterator i = params.begin(); i != params.end(); ++i, ++j )
	{
		const char* other = ( *i ).c_str();
		if ( string_equal( value, other ) ) {
			return ( *j ).c_str();
		}
	}
	return value;
}

///\todo BlendFunc parsing
BlendFunc evaluateBlendFunc( const BlendFuncExpression& blendFunc, const ShaderParameters& params, const ShaderArguments& args ){
	return BlendFunc( BLEND_ONE, BLEND_ZERO );
}

qtexture_t* evaluateTexture( const TextureExpression& texture, const ShaderParameters& params, const ShaderArguments& args, const LoadImageCallback& loader = GlobalTexturesCache().defaultLoader() ){
	StringOutputStream result( 64 );
	const char* expression = texture.c_str();
	const char* end = expression + string_length( expression );
	if ( !string_empty( expression ) ) {
		for (;; )
		{
			const char* best = end;
			const char* bestParam = 0;
			const char* bestArg = 0;
			ShaderArguments::const_iterator j = args.begin();
			for ( ShaderParameters::const_iterator i = params.begin(); i != params.end(); ++i, ++j )
			{
				const char* found = strstr( expression, ( *i ).c_str() );
				if ( found != 0 && found < best ) {
					best = found;
					bestParam = ( *i ).c_str();
					bestArg = ( *j ).c_str();
				}
			}
			if ( best != end ) {
				result << StringRange( expression, best );
				result << PathCleaned( bestArg );
				expression = best + string_length( bestParam );
			}
			else
			{
				break;
			}
		}
		result << expression;
	}
	return GlobalTexturesCache().capture( loader, result );
}

float evaluateFloat( const ShaderValue& value, const ShaderParameters& params, const ShaderArguments& args ){
	const char* result = evaluateShaderValue( value.c_str(), params, args );
	float f;
	if ( !string_parse_float( result, f ) ) {
		globalErrorStream() << "parsing float value failed: " << Quoted( result ) << '\n';
		return 1;
	}
	return f;
}

BlendFactor evaluateBlendFactor( const ShaderValue& value, const ShaderParameters& params, const ShaderArguments& args ){
	const char* result = evaluateShaderValue( value.c_str(), params, args );

	if ( string_equal_nocase( result, "gl_zero" ) ) {
		return BLEND_ZERO;
	}
	if ( string_equal_nocase( result, "gl_one" ) ) {
		return BLEND_ONE;
	}
	if ( string_equal_nocase( result, "gl_src_color" ) ) {
		return BLEND_SRC_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_one_minus_src_color" ) ) {
		return BLEND_ONE_MINUS_SRC_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_src_alpha" ) ) {
		return BLEND_SRC_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_one_minus_src_alpha" ) ) {
		return BLEND_ONE_MINUS_SRC_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_dst_color" ) ) {
		return BLEND_DST_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_one_minus_dst_color" ) ) {
		return BLEND_ONE_MINUS_DST_COLOUR;
	}
	if ( string_equal_nocase( result, "gl_dst_alpha" ) ) {
		return BLEND_DST_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_one_minus_dst_alpha" ) ) {
		return BLEND_ONE_MINUS_DST_ALPHA;
	}
	if ( string_equal_nocase( result, "gl_src_alpha_saturate" ) ) {
		return BLEND_SRC_ALPHA_SATURATE;
	}

	globalErrorStream() << "parsing blend-factor value failed: " << Quoted( result ) << '\n';
	return BLEND_ZERO;
}

class CShader final : public IShader
{
	std::size_t m_refcount;

	ShaderTemplatePointer m_template;
	ShaderArguments m_args;
	CopiedString m_filename;
// name is shader-name, otherwise texture-name (if not a real shader)
	CopiedString m_Name;

	qtexture_t* m_pTexture;
	qtexture_t* m_pSkyBox;
	qtexture_t* m_notfound;
	qtexture_t* m_pDiffuse;
	float m_heightmapScale;
	qtexture_t* m_pBump;
	qtexture_t* m_pSpecular;
	qtexture_t* m_pLightFalloffImage;
	BlendFunc m_blendFunc;

	bool m_bInUse;
	std::vector<Q3Stage> m_q3Stages;
	bool m_q3Animated;


public:
	static bool m_lightingEnabled;

	CShader( const ShaderDefinition& definition ) :
		m_refcount( 0 ),
		m_template( definition.shaderTemplate ),
		m_args( definition.args ),
		m_filename( definition.filename ),
		m_blendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA ),
		m_bInUse( false ),
		m_q3Animated( false ){
		m_pTexture = 0;
		m_pSkyBox = 0;
		m_pDiffuse = 0;
		m_pBump = 0;
		m_pSpecular = 0;

		m_notfound = 0;

		realise();
	}
	CShader( const ShaderTemplatePointer& shaderTemplate, const ShaderArguments& args, const char* filename ) :
		m_refcount( 0 ),
		m_template( shaderTemplate ),
		m_args( args ),
		m_filename( filename ),
		m_blendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA ),
		m_bInUse( false ),
		m_q3Animated( false ){
		m_pTexture = 0;
		m_pSkyBox = 0;
		m_pDiffuse = 0;
		m_pBump = 0;
		m_pSpecular = 0;

		m_notfound = 0;

		realise();
	}
	~CShader(){
		unrealise();

		ASSERT_MESSAGE( m_refcount == 0, "deleting active shader" );
	}

// IShaders implementation -----------------
	void IncRef() override {
		++m_refcount;
	}
	void DecRef() override {
		ASSERT_MESSAGE( m_refcount != 0, "shader reference-count going below zero" );
		if ( --m_refcount == 0 ) {
			delete this;
		}
	}

	std::size_t refcount(){
		return m_refcount;
	}

// get/set the qtexture_t* Radiant uses to represent this shader object
	qtexture_t* getTexture() const override {
		return m_pTexture;
	}
	qtexture_t* getSkyBox() override {
		/* load skybox if only used */
		if( m_pSkyBox == nullptr && !m_template->m_skyBox.empty() )
			m_pSkyBox = GlobalTexturesCache().capture( LoadImageCallback( 0, GlobalTexturesCache().defaultLoader().m_func, true ), m_template->m_skyBox.c_str() );

		return m_pSkyBox;
	}
	qtexture_t* getDiffuse() const override {
		return m_pDiffuse;
	}
	qtexture_t* getBump() const override {
		return m_pBump;
	}
	qtexture_t* getSpecular() const override {
		return m_pSpecular;
	}
// get shader name
	const char* getName() const override {
		return m_Name.c_str();
	}
	bool IsInUse() const override {
		return m_bInUse;
	}
	void SetInUse( bool bInUse ) override {
		m_bInUse = bInUse;
		g_ActiveShadersChangedNotify();
	}
// get the shader flags
	int getFlags() const override {
		return m_template->m_nFlags;
	}
// get the transparency value
	float getTrans() const override {
		return m_template->m_fTrans;
	}
// test if it's a true shader, or a default shader created to wrap around a texture
	bool IsDefault() const override {
		return string_empty( m_filename.c_str() );
	}
// get the alphaFunc
	void getAlphaFunc( EAlphaFunc *func, float *ref ) override {
		*func = m_template->m_AlphaFunc;
		*ref = m_template->m_AlphaRef;
	};
	BlendFunc getBlendFunc() const override {
		return m_blendFunc;
	}
// get the cull type
	ECull getCull() override {
		return m_template->m_Cull;
	};
// get shader file name (ie the file where this one is defined)
	const char* getShaderFileName() const override {
		return m_filename.c_str();
	}
// -----------------------------------------

	void realise(){
		m_pTexture = evaluateTexture( m_template->m_textureName, m_template->m_params, m_args );

		if ( m_pTexture->texture_number == 0 ) {
			m_notfound = m_pTexture;

			const auto name = StringStream( GlobalRadiant().getAppPath(), "bitmaps/",
				( string_equal( m_template->getName(), "nomodel" )? "nomodel.png"
				: IsDefault() ? "notex.png"
				: "shadernotex.png" ) );
			m_pTexture = GlobalTexturesCache().capture( LoadImageCallback( 0, loadBitmap ), name );
		}

		realiseStages();
		realiseLighting();
	}

	void unrealise(){
		GlobalTexturesCache().release( m_pTexture );

		if ( m_notfound != 0 ) {
			GlobalTexturesCache().release( m_notfound );
		}

		if ( m_pSkyBox != 0 ) {
			GlobalTexturesCache().release( m_pSkyBox );
		}

		unrealiseStages();
		unrealiseLighting();
	}

	void realiseStages(){
		m_q3Stages.clear();
		m_q3Animated = false;

		if ( g_shaderLanguage != SHADERLANGUAGE_QUAKE3 || !g_enableQ3ShaderStages ) {
			return;
		}

		const LoadImageCallback loader( 0, loadSpecial );

		for ( const auto& stageTemplate : m_template->m_q3Stages )
		{
			Q3Stage stage;
			stage.animFps = stageTemplate.animFps;
			stage.clampToEdge = stageTemplate.mapType == Q3StageTemplate::MAP_CLAMP
			                    || stageTemplate.mapType == Q3StageTemplate::MAP_ANIM_CLAMP;
			stage.hasBlendFunc = stageTemplate.hasBlendFunc;
			stage.blendFunc = stageTemplate.blendFunc;

			stage.rgbGenType = stageTemplate.rgbGenType;
			stage.rgbConst = stageTemplate.rgbConst;
			stage.rgbWave = stageTemplate.rgbWave;
			stage.alphaGenType = stageTemplate.alphaGenType;
			stage.alphaConst = stageTemplate.alphaConst;
			stage.alphaWave = stageTemplate.alphaWave;
			stage.alphaPortalRange = stageTemplate.alphaPortalRange;

			stage.alphaFunc = stageTemplate.alphaFunc;
			stage.depthFunc = stageTemplate.depthFunc;
			stage.depthWrite = stageTemplate.depthWrite;
			stage.detail = stageTemplate.detail;

			stage.tcGen = stageTemplate.tcGen;
			stage.tcGenVec0 = stageTemplate.tcGenVec0;
			stage.tcGenVec1 = stageTemplate.tcGenVec1;
			stage.tcMods = stageTemplate.tcMods;

			if ( stageTemplate.mapType == Q3StageTemplate::MAP_TEXTURE || stageTemplate.mapType == Q3StageTemplate::MAP_CLAMP ) {
				stage.textures.push_back( evaluateTexture( stageTemplate.map, m_template->m_params, m_args, loader ) );
			}
			else if ( stageTemplate.mapType == Q3StageTemplate::MAP_ANIM || stageTemplate.mapType == Q3StageTemplate::MAP_ANIM_CLAMP ) {
				for ( const auto& frame : stageTemplate.animMaps )
				{
					stage.textures.push_back( evaluateTexture( frame, m_template->m_params, m_args, loader ) );
				}
			}

			stage.usesVertexColour =
			    stage.rgbGenType == Q3_RGB_VERTEX
			    || stage.rgbGenType == Q3_RGB_EXACTVERTEX
			    || stage.rgbGenType == Q3_RGB_LIGHTING_DIFFUSE
			    || stage.rgbGenType == Q3_RGB_ONE_MINUS_VERTEX
			    || stage.alphaGenType == Q3_ALPHA_VERTEX
			    || stage.alphaGenType == Q3_ALPHA_ONE_MINUS_VERTEX
			    || stage.alphaGenType == Q3_ALPHA_LIGHTING_SPECULAR;

			const bool animMap = stage.textures.size() > 1 && stage.animFps > 0.0f;
			const bool rgbWave = stage.rgbGenType == Q3_RGB_WAVE;
			const bool alphaWave = stage.alphaGenType == Q3_ALPHA_WAVE;
			bool tcModAnimated = false;
			for ( const auto& tcMod : stage.tcMods )
			{
				if ( tcMod.type == Q3_TCMOD_SCROLL && ( tcMod.params[0] != 0.0f || tcMod.params[1] != 0.0f ) ) {
					tcModAnimated = true;
				}
				else if ( tcMod.type == Q3_TCMOD_ROTATE && tcMod.params[0] != 0.0f ) {
					tcModAnimated = true;
				}
				else if ( tcMod.type == Q3_TCMOD_STRETCH && ( tcMod.wave.frequency != 0.0f || tcMod.wave.amplitude != 0.0f ) ) {
					tcModAnimated = true;
				}
				else if ( tcMod.type == Q3_TCMOD_TURB && ( tcMod.wave.frequency != 0.0f || tcMod.wave.amplitude != 0.0f ) ) {
					tcModAnimated = true;
				}
			}

			stage.animated = animMap || rgbWave || alphaWave || tcModAnimated;
			m_q3Animated = m_q3Animated || stage.animated;

			if ( !stage.textures.empty() ) {
				m_q3Stages.push_back( stage );
			}
		}
	}

	void unrealiseStages(){
		if ( g_shaderLanguage != SHADERLANGUAGE_QUAKE3 ) {
			return;
		}
		for ( auto& stage : m_q3Stages )
		{
			for ( auto *texture : stage.textures )
			{
				GlobalTexturesCache().release( texture );
			}
		}
		m_q3Stages.clear();
		m_q3Animated = false;
	}

	void evaluateStage( const Q3Stage& stage, float time, ShaderStage& out ) const {
		out = ShaderStage();

		// Q3 default overbrightBits=1 maps identityLighting to 0.5.
		static constexpr float c_q3IdentityLight = 0.5f;

		if ( stage.textures.empty() ) {
			out.texture = m_pTexture;
		}
		else if ( stage.textures.size() == 1 || stage.animFps <= 0.0f ) {
			out.texture = stage.textures.front();
		}
		else
		{
			const std::size_t count = stage.textures.size();
			const std::size_t frame = static_cast<std::size_t>( std::floor( time * stage.animFps ) ) % count;
			out.texture = stage.textures[frame];
		}
		if ( out.texture == nullptr || out.texture->texture_number == 0 ) {
			out.texture = m_pTexture;
		}

		Vector3 rgb( 1.0f, 1.0f, 1.0f );
		switch ( stage.rgbGenType )
		{
		case Q3_RGB_IDENTITY_LIGHTING:
			rgb = Vector3( c_q3IdentityLight, c_q3IdentityLight, c_q3IdentityLight );
			break;
		case Q3_RGB_CONST:
			rgb = stage.rgbConst;
			break;
		case Q3_RGB_WAVE:
			{
				const float v = Q3Shader_clamp01( Q3Shader_waveValue( stage.rgbWave, time ) );
				rgb = Vector3( v, v, v );
			}
			break;
		default:
			break;
		}

		float alpha = 1.0f;
		switch ( stage.alphaGenType )
		{
		case Q3_ALPHA_CONST:
			alpha = Q3Shader_clamp01( stage.alphaConst );
			break;
		case Q3_ALPHA_WAVE:
			alpha = Q3Shader_clamp01( Q3Shader_waveValue( stage.alphaWave, time ) );
			break;
		case Q3_ALPHA_PORTAL:
			alpha = 1.0f;
			break;
		default:
			break;
		}

		rgb.x() = Q3Shader_clamp01( rgb.x() );
		rgb.y() = Q3Shader_clamp01( rgb.y() );
		rgb.z() = Q3Shader_clamp01( rgb.z() );

		out.colour = Vector4( rgb, Q3Shader_clamp01( alpha ) );
		out.blendFunc = stage.blendFunc;
		out.hasBlendFunc = stage.hasBlendFunc;
		out.clampToEdge = stage.clampToEdge;
		out.depthWrite = stage.depthWrite || !stage.hasBlendFunc;
		out.depthFunc = stage.depthFunc;
		out.alphaFunc = stage.alphaFunc;
		out.alphaRef = 0.0f;
		if ( stage.alphaFunc == eStageAlphaLT128 || stage.alphaFunc == eStageAlphaGE128 ) {
			out.alphaRef = 0.5f;
		}
		out.texMatrix = Q3Shader_buildTexMatrix( stage, time );
		out.tcGen = stage.tcGen;
		out.tcGenVec0 = stage.tcGenVec0;
		out.tcGenVec1 = stage.tcGenVec1;
		out.usesVertexColour = stage.usesVertexColour;
	}

	void realiseLighting(){
		if ( m_lightingEnabled && g_shaderLanguage != SHADERLANGUAGE_QUAKE3 ) {
			LoadImageCallback loader = GlobalTexturesCache().defaultLoader();
			if ( !string_empty( m_template->m_heightmapScale.c_str() ) ) {
				m_heightmapScale = evaluateFloat( m_template->m_heightmapScale, m_template->m_params, m_args );
				loader = LoadImageCallback( &m_heightmapScale, loadHeightmap );
			}
			m_pDiffuse = evaluateTexture( m_template->m_diffuse, m_template->m_params, m_args );
			m_pBump = evaluateTexture( m_template->m_bump, m_template->m_params, m_args, loader );
			m_pSpecular = evaluateTexture( m_template->m_specular, m_template->m_params, m_args );
			m_pLightFalloffImage = evaluateTexture( m_template->m_lightFalloffImage, m_template->m_params, m_args );

			for ( const auto& layer : m_template->m_layers )
			{
				m_layers.push_back( evaluateLayer( layer, m_template->m_params, m_args ) );
			}

			if ( m_layers.size() == 1 ) {
				const BlendFuncExpression& blendFunc = m_template->m_layers.front().blendFunc();
				if ( !string_empty( blendFunc.second.c_str() ) ) {
					m_blendFunc = BlendFunc(
					                  evaluateBlendFactor( blendFunc.first.c_str(), m_template->m_params, m_args ),
					                  evaluateBlendFactor( blendFunc.second.c_str(), m_template->m_params, m_args )
					              );
				}
				else
				{
					const char* blend = evaluateShaderValue( blendFunc.first.c_str(), m_template->m_params, m_args );

					if ( string_equal_nocase( blend, "add" ) ) {
						m_blendFunc = BlendFunc( BLEND_ONE, BLEND_ONE );
					}
					else if ( string_equal_nocase( blend, "filter" ) ) {
						m_blendFunc = BlendFunc( BLEND_DST_COLOUR, BLEND_ZERO );
					}
					else if ( string_equal_nocase( blend, "blend" ) ) {
						m_blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
					}
					else
					{
						globalErrorStream() << "parsing blend value failed: " << Quoted( blend ) << '\n';
					}
				}
			}
		}
	}

	void unrealiseLighting(){
		if ( m_lightingEnabled && g_shaderLanguage != SHADERLANGUAGE_QUAKE3 ) {
			GlobalTexturesCache().release( m_pDiffuse );
			GlobalTexturesCache().release( m_pBump );
			GlobalTexturesCache().release( m_pSpecular );

			GlobalTexturesCache().release( m_pLightFalloffImage );

			for ( auto& layer : m_layers )
			{
				GlobalTexturesCache().release( layer.texture() );
			}
			m_layers.clear();

			m_blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
		}
	}

// set shader name
	void setName( const char* name ){
		m_Name = name;
	}

	class MapLayer final : public ShaderLayer
	{
		qtexture_t* m_texture;
		BlendFunc m_blendFunc;
		bool m_clampToBorder;
		float m_alphaTest;
	public:
		MapLayer( qtexture_t* texture, BlendFunc blendFunc, bool clampToBorder, float alphaTest ) :
			m_texture( texture ),
			m_blendFunc( blendFunc ),
			m_clampToBorder( false ),
			m_alphaTest( alphaTest ){
		}
		qtexture_t* texture() const override {
			return m_texture;
		}
		BlendFunc blendFunc() const override {
			return m_blendFunc;
		}
		bool clampToBorder() const override {
			return m_clampToBorder;
		}
		float alphaTest() const override {
			return m_alphaTest;
		}
	};

	static MapLayer evaluateLayer( const ShaderTemplate::MapLayerTemplate& layerTemplate, const ShaderParameters& params, const ShaderArguments& args ){
		return MapLayer(
		           evaluateTexture( layerTemplate.texture(), params, args ),
		           evaluateBlendFunc( layerTemplate.blendFunc(), params, args ),
		           layerTemplate.clampToBorder(),
		           evaluateFloat( layerTemplate.alphaTest(), params, args )
		       );
	}

	typedef std::vector<MapLayer> MapLayers;
	MapLayers m_layers;

	const ShaderLayer* firstLayer() const override {
		if ( m_layers.empty() ) {
			return 0;
		}
		return &m_layers.front();
	}
	void forEachLayer( const ShaderLayerCallback& callback ) const override {
		for ( const auto& layer : m_layers )
		{
			callback( layer );
		}
	}

	bool hasStages() const override {
		return g_shaderLanguage == SHADERLANGUAGE_QUAKE3 && g_enableQ3ShaderStages && !m_q3Stages.empty();
	}

	bool isAnimated() const override {
		return g_enableQ3ShaderStages && m_q3Animated;
	}

	void forEachStage( float time, const ShaderStageCallback& callback ) const override {
		if ( g_shaderLanguage != SHADERLANGUAGE_QUAKE3 ) {
			return;
		}

		if ( !g_enableQ3ShaderStages ) {
			ShaderStage stage;
			stage.texture = m_pTexture;
			stage.depthWrite = true;
			callback( stage );
			return;
		}

		if ( m_q3Stages.empty() ) {
			ShaderStage stage;
			stage.texture = m_pTexture;
			stage.depthWrite = true;
			callback( stage );
			return;
		}

		for ( const auto& stage : m_q3Stages )
		{
			ShaderStage out;
			evaluateStage( stage, time, out );
			callback( out );
		}
	}

	qtexture_t* lightFalloffImage() const override {
		if ( !m_template->m_lightFalloffImage.empty() ) {
			return m_pLightFalloffImage;
		}
		return 0;
	}
};

bool CShader::m_lightingEnabled = false;

typedef SmartPointer<CShader> ShaderPointer;
typedef std::map<CopiedString, ShaderPointer, shader_less_t> shaders_t;

shaders_t g_ActiveShaders;

static shaders_t::iterator g_ActiveShadersIterator;

void ActiveShaders_IteratorBegin(){
	g_ActiveShadersIterator = g_ActiveShaders.begin();
}

bool ActiveShaders_IteratorAtEnd(){
	return g_ActiveShadersIterator == g_ActiveShaders.end();
}

IShader *ActiveShaders_IteratorCurrent(){
	return static_cast<CShader*>( g_ActiveShadersIterator->second );
}

void ActiveShaders_IteratorIncrement(){
	++g_ActiveShadersIterator;
}

void debug_check_shaders( shaders_t& shaders ){
	for ( const auto& [ name, shader ] : shaders )
	{
		ASSERT_MESSAGE( shader->refcount() == 1, "orphan shader still referenced" );
	}
}

// will free all GL binded qtextures and shaders
// NOTE: doesn't make much sense out of Radiant exit or called during a reload
void FreeShaders(){
	// reload shaders
	// empty the actives shaders list
	debug_check_shaders( g_ActiveShaders );
	g_ActiveShaders.clear();
	g_shaders.clear();
	g_shaderTemplates.clear();
	g_shaderDefinitions.clear();
	g_ActiveShadersChangedNotify();
}

bool ShaderTemplate::parseQuake3( Tokeniser& tokeniser ){
	// name of the qtexture_t we'll use to represent this shader (this one has the "textures\" before)
	m_textureName = m_Name;
	m_q3Stages.clear();
	const bool parseStages = g_enableQ3ShaderStages;

	tokeniser.nextLine();

	// we need to read until we hit a balanced }
	int depth = 0;
	Q3StageTemplate currentStage;
	for (;; )
	{
		if ( !parseStages ) {
			tokeniser.nextLine();
		}
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			return false;
		}

		if ( string_equal( token, "{" ) ) {
			++depth;
			if ( parseStages && depth == 2 ) {
				currentStage = Q3StageTemplate();
			}
			continue;
		}
		else if ( string_equal( token, "}" ) ) {
			--depth;
			if ( depth < 0 ) { // underflow
				return false;
			}
			if ( parseStages && depth == 1 ) {
				const bool hasMap = ( currentStage.mapType == Q3StageTemplate::MAP_TEXTURE || currentStage.mapType == Q3StageTemplate::MAP_CLAMP )
				                    ? !currentStage.map.empty()
				                    : !currentStage.animMaps.empty();
				if ( hasMap ) {
					m_q3Stages.push_back( currentStage );
				}
			}
			if ( depth == 0 ) { // end of shader
				break;
			}

			continue;
		}

		if ( depth == 1 ) {
			if ( string_equal_nocase( token, "qer_nocarve" ) ) {
				m_nFlags |= QER_NOCARVE;
			}
			else if ( string_equal_nocase( token, "qer_trans" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, m_fTrans ) );
				m_nFlags |= QER_TRANS;
			}
			else if ( string_equal_nocase( token, "qer_editorimage" ) ) {
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, m_textureName ) );
			}
			else if ( string_equal_nocase( token, "qer_alphafunc" ) ) {
				const char* alphafunc = tokeniser.getToken();

				if ( alphafunc == 0 ) {
					Tokeniser_unexpectedError( tokeniser, alphafunc, "#alphafunc" );
					return false;
				}

				if ( string_equal_nocase( alphafunc, "equal" ) ) {
					m_AlphaFunc = IShader::eEqual;
				}
				else if ( string_equal_nocase( alphafunc, "greater" ) ) {
					m_AlphaFunc = IShader::eGreater;
				}
				else if ( string_equal_nocase( alphafunc, "less" ) ) {
					m_AlphaFunc = IShader::eLess;
				}
				else if ( string_equal_nocase( alphafunc, "gequal" ) ) {
					m_AlphaFunc = IShader::eGEqual;
				}
				else if ( string_equal_nocase( alphafunc, "lequal" ) ) {
					m_AlphaFunc = IShader::eLEqual;
				}
				else
				{
					m_AlphaFunc = IShader::eAlways;
				}

				m_nFlags |= QER_ALPHATEST;

				RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, m_AlphaRef ) );
			}
			else if ( string_equal_nocase( token, "skyparms" ) ) {
				const char* sky = tokeniser.getToken();

				if ( sky == 0 ) {
					Tokeniser_unexpectedError( tokeniser, sky, "#skyparms" );
					return false;
				}

				if( !string_equal( sky, "-" ) ){
					m_skyBox = sky;
				}

				m_nFlags |= QER_SKY;
			}
			else if ( string_equal_nocase( token, "cull" ) ) {
				const char* cull = tokeniser.getToken();

				if ( cull == 0 ) {
					Tokeniser_unexpectedError( tokeniser, cull, "#cull" );
					return false;
				}

				if ( string_equal_nocase( cull, "none" )
				  || string_equal_nocase( cull, "twosided" )
				  || string_equal_nocase( cull, "disable" ) ) {
					m_Cull = IShader::eCullNone;
				}
				else if ( string_equal_nocase( cull, "back" )
				       || string_equal_nocase( cull, "backside" )
				       || string_equal_nocase( cull, "backsided" ) ) {
					m_Cull = IShader::eCullBack;
				}
				else
				{
					m_Cull = IShader::eCullBack;
				}

				m_nFlags |= QER_CULL;
			}
			else if ( string_equal_nocase( token, "surfaceparm" ) ) {
				const char* surfaceparm = tokeniser.getToken();

				if ( surfaceparm == 0 ) {
					Tokeniser_unexpectedError( tokeniser, surfaceparm, "#surfaceparm" );
					return false;
				}

				if ( string_equal_nocase( surfaceparm, "fog" ) ) {
					m_nFlags |= QER_FOG;
					m_nFlags |= QER_TRANS;
					if ( m_fTrans == 1 ) { // has not been explicitly set by qer_trans
						m_fTrans = 0.35f;
					}
				}
				else if ( string_equal_nocase( surfaceparm, "nodraw" ) ) {
					m_nFlags |= QER_NODRAW;
				}
				else if ( string_equal_nocase( surfaceparm, "nonsolid" ) ) {
					m_nFlags |= QER_NONSOLID;
				}
				else if ( string_equal_nocase( surfaceparm, "water" ) ||
				          string_equal_nocase( surfaceparm, "lava" ) ||
				          string_equal_nocase( surfaceparm, "slime") ){
					m_nFlags |= QER_LIQUID;
				}
				else if ( string_equal_nocase( surfaceparm, "areaportal" ) ) {
					m_nFlags |= QER_AREAPORTAL;
				}
				else if ( string_equal_nocase( surfaceparm, "playerclip" ) ) {
					m_nFlags |= QER_CLIP;
				}
				else if ( string_equal_nocase( surfaceparm, "botclip" ) ) {
					m_nFlags |= QER_BOTCLIP;
				}
			}
		}
		else if ( depth == 2 ) {
			if ( !parseStages ) {
				continue;
			}
			if ( string_equal_nocase( token, "map" ) ) {
				currentStage.mapType = Q3StageTemplate::MAP_TEXTURE;
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, currentStage.map ) );
			}
			else if ( string_equal_nocase( token, "clampmap" ) ) {
				currentStage.mapType = Q3StageTemplate::MAP_CLAMP;
				RETURN_FALSE_IF_FAIL( Tokeniser_parseTextureName( tokeniser, currentStage.map ) );
			}
			else if ( string_equal_nocase( token, "animmap" ) || string_equal_nocase( token, "clampanimmap" ) ) {
				currentStage.mapType = string_equal_nocase( token, "clampanimmap" )
				                       ? Q3StageTemplate::MAP_ANIM_CLAMP
				                       : Q3StageTemplate::MAP_ANIM;
				currentStage.animMaps.clear();
				RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, currentStage.animFps ) );
				for (;; )
				{
					const char* frame = tokeniser.getToken();
					if ( frame == 0 ) {
						return false;
					}
					if ( string_equal( frame, "}" ) || Q3Shader_isStageDirective( frame ) ) {
						tokeniser.ungetToken();
						break;
					}
					TextureExpression frameName;
					parseTextureName( frameName, frame );
					currentStage.animMaps.push_back( frameName );
				}
			}
			else if ( string_equal_nocase( token, "videomap" ) ) {
				const char* videoName = tokeniser.getToken();
				if ( videoName == 0 ) {
					Tokeniser_unexpectedError( tokeniser, videoName, "#videomap" );
					return false;
				}
				currentStage.mapType = Q3StageTemplate::MAP_TEXTURE;
				currentStage.map = "$whiteimage";
			}
			else if ( string_equal_nocase( token, "blendfunc" ) ) {
				const char* blend = tokeniser.getToken();
				if ( blend == 0 ) {
					Tokeniser_unexpectedError( tokeniser, blend, "#blendfunc" );
					return false;
				}
				if ( string_equal_nocase( blend, "add" ) ) {
					currentStage.blendFunc = BlendFunc( BLEND_ONE, BLEND_ONE );
				}
				else if ( string_equal_nocase( blend, "filter" ) ) {
					currentStage.blendFunc = BlendFunc( BLEND_DST_COLOUR, BLEND_ZERO );
				}
				else if ( string_equal_nocase( blend, "blend" ) ) {
					currentStage.blendFunc = BlendFunc( BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA );
				}
				else
				{
					const char* dst = tokeniser.getToken();
					if ( dst == 0 ) {
						Tokeniser_unexpectedError( tokeniser, dst, "#blendfunc-dst" );
						return false;
					}
					currentStage.blendFunc = BlendFunc(
					                             Q3Shader_parseBlendFactor( blend ),
					                             Q3Shader_parseBlendFactor( dst )
					                         );
				}
				currentStage.hasBlendFunc = true;
			}
			else if ( string_equal_nocase( token, "rgbgen" ) ) {
				const char* gen = tokeniser.getToken();
				if ( gen == 0 ) {
					Tokeniser_unexpectedError( tokeniser, gen, "#rgbgen" );
					return false;
				}
				if ( string_equal_nocase( gen, "identity" ) ) {
					currentStage.rgbGenType = Q3_RGB_IDENTITY;
				}
				else if ( string_equal_nocase( gen, "identitylighting" ) ) {
					currentStage.rgbGenType = Q3_RGB_IDENTITY_LIGHTING;
				}
				else if ( string_equal_nocase( gen, "const" ) ) {
					currentStage.rgbGenType = Q3_RGB_CONST;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseConstColor( tokeniser, currentStage.rgbConst ) );
				}
				else if ( string_equal_nocase( gen, "wave" ) ) {
					currentStage.rgbGenType = Q3_RGB_WAVE;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseWaveForm( tokeniser, currentStage.rgbWave ) );
				}
				else if ( string_equal_nocase( gen, "vertex" ) ) {
					currentStage.rgbGenType = Q3_RGB_VERTEX;
				}
				else if ( string_equal_nocase( gen, "exactvertex" ) ) {
					currentStage.rgbGenType = Q3_RGB_EXACTVERTEX;
				}
				else if ( string_equal_nocase( gen, "entity" ) ) {
					currentStage.rgbGenType = Q3_RGB_ENTITY;
				}
				else if ( string_equal_nocase( gen, "oneminusentity" ) ) {
					currentStage.rgbGenType = Q3_RGB_ONE_MINUS_ENTITY;
				}
				else if ( string_equal_nocase( gen, "lightingdiffuse" ) ) {
					currentStage.rgbGenType = Q3_RGB_LIGHTING_DIFFUSE;
				}
				else if ( string_equal_nocase( gen, "oneminusvertex" ) ) {
					currentStage.rgbGenType = Q3_RGB_ONE_MINUS_VERTEX;
				}
			}
			else if ( string_equal_nocase( token, "alphagen" ) ) {
				const char* gen = tokeniser.getToken();
				if ( gen == 0 ) {
					Tokeniser_unexpectedError( tokeniser, gen, "#alphagen" );
					return false;
				}
				if ( string_equal_nocase( gen, "identity" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_IDENTITY;
				}
				else if ( string_equal_nocase( gen, "const" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_CONST;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseConstAlpha( tokeniser, currentStage.alphaConst ) );
				}
				else if ( string_equal_nocase( gen, "wave" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_WAVE;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseWaveForm( tokeniser, currentStage.alphaWave ) );
				}
				else if ( string_equal_nocase( gen, "vertex" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_VERTEX;
				}
				else if ( string_equal_nocase( gen, "oneminusvertex" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_ONE_MINUS_VERTEX;
				}
				else if ( string_equal_nocase( gen, "entity" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_ENTITY;
				}
				else if ( string_equal_nocase( gen, "oneminusentity" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_ONE_MINUS_ENTITY;
				}
				else if ( string_equal_nocase( gen, "portal" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_PORTAL;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, currentStage.alphaPortalRange ) );
				}
				else if ( string_equal_nocase( gen, "lightingspecular" ) ) {
					currentStage.alphaGenType = Q3_ALPHA_LIGHTING_SPECULAR;
				}
			}
			else if ( string_equal_nocase( token, "alphafunc" ) ) {
				const char* func = tokeniser.getToken();
				if ( func == 0 ) {
					Tokeniser_unexpectedError( tokeniser, func, "#alphafunc" );
					return false;
				}
				if ( string_equal_nocase( func, "gt0" ) ) {
					currentStage.alphaFunc = eStageAlphaGT0;
				}
				else if ( string_equal_nocase( func, "lt128" ) ) {
					currentStage.alphaFunc = eStageAlphaLT128;
				}
				else if ( string_equal_nocase( func, "ge128" ) ) {
					currentStage.alphaFunc = eStageAlphaGE128;
				}
			}
			else if ( string_equal_nocase( token, "depthfunc" ) ) {
				const char* func = tokeniser.getToken();
				if ( func == 0 ) {
					Tokeniser_unexpectedError( tokeniser, func, "#depthfunc" );
					return false;
				}
				if ( string_equal_nocase( func, "less" ) ) {
					currentStage.depthFunc = eStageDepthLess;
				}
				else if ( string_equal_nocase( func, "lequal" ) ) {
					currentStage.depthFunc = eStageDepthLEqual;
				}
				else if ( string_equal_nocase( func, "equal" ) ) {
					currentStage.depthFunc = eStageDepthEqual;
				}
				else if ( string_equal_nocase( func, "greater" ) ) {
					currentStage.depthFunc = eStageDepthGreater;
				}
				else if ( string_equal_nocase( func, "gequal" ) ) {
					currentStage.depthFunc = eStageDepthGEqual;
				}
				else if ( string_equal_nocase( func, "always" ) ) {
					currentStage.depthFunc = eStageDepthAlways;
				}
			}
			else if ( string_equal_nocase( token, "depthwrite" ) ) {
				currentStage.depthWrite = true;
			}
			else if ( string_equal_nocase( token, "detail" ) ) {
				currentStage.detail = true;
			}
			else if ( string_equal_nocase( token, "tcgen" ) ) {
				const char* gen = tokeniser.getToken();
				if ( gen == 0 ) {
					Tokeniser_unexpectedError( tokeniser, gen, "#tcgen" );
					return false;
				}
				if ( string_equal_nocase( gen, "base" ) ) {
					currentStage.tcGen = eTcGenBase;
				}
				else if ( string_equal_nocase( gen, "lightmap" ) ) {
					currentStage.tcGen = eTcGenLightmap;
				}
				else if ( string_equal_nocase( gen, "environment" ) ) {
					currentStage.tcGen = eTcGenEnvironment;
				}
				else if ( string_equal_nocase( gen, "vector" ) ) {
					currentStage.tcGen = eTcGenVector;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseVec3( tokeniser, currentStage.tcGenVec0 ) );
					RETURN_FALSE_IF_FAIL( Q3Shader_parseVec3( tokeniser, currentStage.tcGenVec1 ) );
				}
			}
			else if ( string_equal_nocase( token, "tcmod" ) ) {
				const char* mod = tokeniser.getToken();
				if ( mod == 0 ) {
					Tokeniser_unexpectedError( tokeniser, mod, "#tcmod" );
					return false;
				}
				Q3TcMod tcmod;
				if ( string_equal_nocase( mod, "scroll" ) ) {
					tcmod.type = Q3_TCMOD_SCROLL;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[0] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[1] ) );
					currentStage.tcMods.push_back( tcmod );
				}
				else if ( string_equal_nocase( mod, "scale" ) ) {
					tcmod.type = Q3_TCMOD_SCALE;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[0] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[1] ) );
					currentStage.tcMods.push_back( tcmod );
				}
				else if ( string_equal_nocase( mod, "rotate" ) ) {
					tcmod.type = Q3_TCMOD_ROTATE;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[0] ) );
					currentStage.tcMods.push_back( tcmod );
				}
				else if ( string_equal_nocase( mod, "stretch" ) ) {
					tcmod.type = Q3_TCMOD_STRETCH;
					RETURN_FALSE_IF_FAIL( Q3Shader_parseWaveForm( tokeniser, tcmod.wave ) );
					currentStage.tcMods.push_back( tcmod );
				}
				else if ( string_equal_nocase( mod, "transform" ) ) {
					tcmod.type = Q3_TCMOD_TRANSFORM;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[0] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[1] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[2] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[3] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[4] ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.params[5] ) );
					currentStage.tcMods.push_back( tcmod );
				}
				else if ( string_equal_nocase( mod, "turb" ) ) {
					tcmod.type = Q3_TCMOD_TURB;
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.wave.base ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.wave.amplitude ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.wave.phase ) );
					RETURN_FALSE_IF_FAIL( Tokeniser_getFloat( tokeniser, tcmod.wave.frequency ) );
					currentStage.tcMods.push_back( tcmod );
				}
			}
		}
	}

	if ( parseStages && ( m_textureName.empty() || string_equal( m_textureName.c_str(), m_Name.c_str() ) ) ) {
		for ( const auto& stage : m_q3Stages )
		{
			if ( stage.mapType == Q3StageTemplate::MAP_TEXTURE || stage.mapType == Q3StageTemplate::MAP_CLAMP ) {
				if ( !stage.map.empty() && !string_equal_nocase( stage.map.c_str(), "$lightmap" ) ) {
					m_textureName = stage.map;
					break;
				}
			}
			else if ( stage.mapType == Q3StageTemplate::MAP_ANIM || stage.mapType == Q3StageTemplate::MAP_ANIM_CLAMP ) {
				if ( !stage.animMaps.empty() ) {
					m_textureName = stage.animMaps.front();
					break;
				}
			}
		}
	}

	return true;
}

class Layer
{
public:
	LayerTypeId m_type;
	TextureExpression m_texture;
	BlendFunc m_blendFunc;
	bool m_clampToBorder;
	float m_alphaTest;
	float m_heightmapScale;

	Layer() : m_type( LAYER_NONE ), m_blendFunc( BLEND_ONE, BLEND_ZERO ), m_clampToBorder( false ), m_alphaTest( -1 ), m_heightmapScale( 0 ){
	}
};

float Q3Shader_wrap01( float value ){
	value = float_mod( value, 1.0f );
	return value < 0.0f ? value + 1.0f : value;
}

float Q3Shader_noise( float phase ){
	const float n = sin( phase * 12.9898f + 78.233f ) * 43758.5453f;
	const float frac = n - std::floor( n );
	return frac * 2.0f - 1.0f;
}

float Q3Shader_waveSample( const Q3WaveForm& wave, float time ){
	const float phase = wave.phase + time * wave.frequency;
	const float frac = Q3Shader_wrap01( phase );
	switch ( wave.func )
	{
	case Q3_WAVE_SIN:
		return sin( phase * static_cast<float>( c_2pi ) );
	case Q3_WAVE_TRIANGLE:
		return 2.0f * std::fabs( 2.0f * frac - 1.0f ) - 1.0f;
	case Q3_WAVE_SQUARE:
		return frac < 0.5f ? 1.0f : -1.0f;
	case Q3_WAVE_SAWTOOTH:
		return frac;
	case Q3_WAVE_INVERSESAWTOOTH:
		return 1.0f - frac;
	case Q3_WAVE_NOISE:
		return Q3Shader_noise( phase );
	}
	return 0.0f;
}

float Q3Shader_waveValue( const Q3WaveForm& wave, float time ){
	const float sample = Q3Shader_waveSample( wave, time );
	return wave.base + wave.amplitude * sample;
}

float Q3Shader_clamp01( float value ){
	return std::max( 0.0f, std::min( 1.0f, value ) );
}

Matrix4 Q3Shader_buildTexMatrix( const Q3Stage& stage, float time ){
	Matrix4 texMatrix = g_matrix4_identity;
	for ( const auto& mod : stage.tcMods )
	{
		Matrix4 modMatrix = g_matrix4_identity;
		switch ( mod.type )
		{
		case Q3_TCMOD_SCROLL:
			matrix4_translate_by_vec3( modMatrix, Vector3( Q3Shader_wrap01( mod.params[0] * time ),
			                                               Q3Shader_wrap01( mod.params[1] * time ),
			                                               0.0f ) );
			break;
		case Q3_TCMOD_SCALE:
			matrix4_scale_by_vec3( modMatrix, Vector3( mod.params[0], mod.params[1], 1.0f ) );
			break;
		case Q3_TCMOD_ROTATE:
			matrix4_translate_by_vec3( modMatrix, Vector3( 0.5f, 0.5f, 0.0f ) );
			matrix4_rotate_by_euler_xyz_degrees( modMatrix, Vector3( 0.0f, 0.0f, -mod.params[0] * time ) );
			matrix4_translate_by_vec3( modMatrix, Vector3( -0.5f, -0.5f, 0.0f ) );
			break;
		case Q3_TCMOD_STRETCH:
			{
				float scale = Q3Shader_waveValue( mod.wave, time );
				if ( scale == 0.0f ) {
					scale = 1.0f;
				}
				matrix4_translate_by_vec3( modMatrix, Vector3( 0.5f, 0.5f, 0.0f ) );
				matrix4_scale_by_vec3( modMatrix, Vector3( scale, scale, 1.0f ) );
				matrix4_translate_by_vec3( modMatrix, Vector3( -0.5f, -0.5f, 0.0f ) );
			}
			break;
		case Q3_TCMOD_TRANSFORM:
			modMatrix = Matrix4(
			              mod.params[0], mod.params[1], 0.0f, 0.0f,
			              mod.params[2], mod.params[3], 0.0f, 0.0f,
			              0.0f,          0.0f,          1.0f, 0.0f,
			              mod.params[4], mod.params[5], 0.0f, 1.0f
			          );
			break;
		case Q3_TCMOD_TURB:
			{
				const float offset = mod.wave.base + mod.wave.amplitude * sin( ( mod.wave.phase + time * mod.wave.frequency ) * static_cast<float>( c_2pi ) );
				matrix4_translate_by_vec3( modMatrix, Vector3( offset, offset, 0.0f ) );
			}
			break;
		}
		matrix4_premultiply_by_matrix4( texMatrix, modMatrix );
	}
	return texMatrix;
}

std::list<CopiedString> g_shaderFilenames;

void ParseShaderFile( Tokeniser& tokeniser, const char* filename ){
	g_shaderFilenames.push_back( filename );
	filename = g_shaderFilenames.back().c_str();
	tokeniser.nextLine();
	for (;; )
	{
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			break;
		}

		if ( string_equal( token, "table" ) ) {
			if ( tokeniser.getToken() == 0 ) {
				Tokeniser_unexpectedError( tokeniser, 0, "#table-name" );
				return;
			}
			if ( !Tokeniser_parseToken( tokeniser, "{" ) ) {
				return;
			}
			for (;; )
			{
				const char* option = tokeniser.getToken();
				if ( string_equal( option, "{" ) ) {
					for (;; )
					{
						const char* value = tokeniser.getToken();
						if ( string_equal( value, "}" ) ) {
							break;
						}
					}

					if ( !Tokeniser_parseToken( tokeniser, "}" ) ) {
						return;
					}
					break;
				}
			}
		}
		else
		{
			if ( string_equal( token, "guide" ) ) {
				parseTemplateInstance( tokeniser, filename );
			}
			else
			{
				if ( !string_equal( token, "material" )
				  && !string_equal( token, "particle" )
				  && !string_equal( token, "skin" ) ) {
					tokeniser.ungetToken();
				}
				// first token should be the path + name.. (from base)
				CopiedString name;
				if ( !Tokeniser_parseShaderName( tokeniser, name ) ) {
				}
				ShaderTemplatePointer shaderTemplate( new ShaderTemplate() );
				shaderTemplate->setName( name.c_str() );

				g_shaders.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) );

				bool result = ( g_shaderLanguage == SHADERLANGUAGE_QUAKE3 )
				              ? shaderTemplate->parseQuake3( tokeniser )
				              : shaderTemplate->parseDoom3( tokeniser );
				if ( result ) {
					// do we already have this shader?
					if ( !g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( shaderTemplate->getName(), ShaderDefinition( shaderTemplate.get(), ShaderArguments(), filename ) ) ).second ) {
#ifdef _DEBUG
						globalWarningStream() << "WARNING: shader " << shaderTemplate->getName() << " is already in memory, definition in " << filename << " ignored.\n";
#endif
					}
				}
				else
				{
					globalErrorStream() << "Error parsing shader " << shaderTemplate->getName() << '\n';
					return;
				}
			}
		}
	}
}

ShaderTemplate* ParseShaderTextForPreview( Tokeniser& tokeniser, const char* shaderName ){
	tokeniser.nextLine();

	ShaderTemplate* first = 0;
	ShaderTemplate* match = 0;

	for (;; )
	{
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			break;
		}

		if ( string_equal( token, "table" ) ) {
			if ( tokeniser.getToken() == 0 ) {
				Tokeniser_unexpectedError( tokeniser, 0, "#table-name" );
				break;
			}
			if ( !Tokeniser_parseToken( tokeniser, "{" ) ) {
				break;
			}
			for (;; )
			{
				const char* option = tokeniser.getToken();
				if ( string_equal( option, "{" ) ) {
					for (;; )
					{
						const char* value = tokeniser.getToken();
						if ( string_equal( value, "}" ) ) {
							break;
						}
					}

					if ( !Tokeniser_parseToken( tokeniser, "}" ) ) {
						break;
					}
					break;
				}
			}
			continue;
		}

		if ( string_equal( token, "guide" ) || string_equal( token, "inlineGuide" ) ) {
			std::size_t depth = 0;
			for (;; )
			{
				token = tokeniser.getToken();
				if ( token == 0 ) {
					break;
				}
				if ( string_equal( token, "{" ) ) {
					++depth;
				}
				else if ( string_equal( token, "}" ) ) {
					if ( depth > 0 && --depth == 0 ) {
						break;
					}
				}
			}
			continue;
		}

		if ( !string_equal( token, "material" )
		  && !string_equal( token, "particle" )
		  && !string_equal( token, "skin" ) ) {
			tokeniser.ungetToken();
		}

		CopiedString name;
		if ( !Tokeniser_parseShaderName( tokeniser, name ) ) {
			break;
		}

		ShaderTemplate* shaderTemplate = new ShaderTemplate();
		shaderTemplate->setName( name.c_str() );

		const bool parsed = ( g_shaderLanguage == SHADERLANGUAGE_QUAKE3 )
		                    ? shaderTemplate->parseQuake3( tokeniser )
		                    : shaderTemplate->parseDoom3( tokeniser );

		if ( !parsed ) {
			delete shaderTemplate;
			break;
		}

		if ( first == 0 ) {
			first = shaderTemplate;
		}

		if ( shaderName != 0 && shader_equal( shaderName, name.c_str() ) ) {
			match = shaderTemplate;
			break;
		}

		if ( shaderTemplate != first ) {
			delete shaderTemplate;
		}
	}

	if ( match != 0 ) {
		if ( first != 0 && first != match ) {
			delete first;
		}
		return match;
	}
	return first;
}

void parseGuideFile( Tokeniser& tokeniser, const char* filename ){
	tokeniser.nextLine();
	for (;; )
	{
		const char* token = tokeniser.getToken();

		if ( token == 0 ) {
			break;
		}

		if ( string_equal( token, "guide" ) ) {
			// first token should be the path + name.. (from base)
			ShaderTemplatePointer shaderTemplate( new ShaderTemplate );
			shaderTemplate->parseTemplate( tokeniser );
			if ( !g_shaderTemplates.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) ).second ) {
				globalErrorStream() << "guide " << Quoted( shaderTemplate->getName() ) << ": already defined, second definition ignored\n";
			}
		}
		else if ( string_equal( token, "inlineGuide" ) ) {
			// skip entire inlineGuide definition
			std::size_t depth = 0;
			for (;; )
			{
				tokeniser.nextLine();
				token = tokeniser.getToken();
				if ( string_equal( token, "{" ) ) {
					++depth;
				}
				else if ( string_equal( token, "}" ) ) {
					if ( --depth == 0 ) {
						break;
					}
				}
			}
		}
	}
}

void LoadShaderFile( const char* filename ){
	ArchiveTextFile* file = GlobalFileSystem().openTextFile( filename );

	if ( file != 0 ) {
		globalOutputStream() << "Parsing shaderfile " << filename << '\n';

		Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewScriptTokeniser( file->getInputStream() );

		ParseShaderFile( tokeniser, filename );

		tokeniser.release();
		file->release();
	}
	else
	{
		globalWarningStream() << "Unable to read shaderfile " << filename << '\n';
	}
}

void loadGuideFile( const char* filename ){
	const auto fullname = StringStream( "guides/", filename );
	ArchiveTextFile* file = GlobalFileSystem().openTextFile( fullname );

	if ( file != 0 ) {
		globalOutputStream() << "Parsing guide file " << fullname << '\n';

		Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewScriptTokeniser( file->getInputStream() );

		parseGuideFile( tokeniser, fullname );

		tokeniser.release();
		file->release();
	}
	else
	{
		globalWarningStream() << "Unable to read guide file " << fullname << '\n';
	}
}

CShader* Try_Shader_ForName( const char* name ){
	{
		shaders_t::iterator i = g_ActiveShaders.find( name );
		if ( i != g_ActiveShaders.end() ) {
			return ( *i ).second;
		}
	}
	// active shader was not found

	// find matching shader definition
	ShaderDefinitionMap::iterator i = g_shaderDefinitions.find( name );
	if ( i == g_shaderDefinitions.end() ) {
		// shader definition was not found

		// create new shader definition from default shader template
		ShaderTemplatePointer shaderTemplate( new ShaderTemplate() );
		shaderTemplate->CreateDefault( name );
		g_shaderTemplates.insert( ShaderTemplateMap::value_type( shaderTemplate->getName(), shaderTemplate ) );

		i = g_shaderDefinitions.insert( ShaderDefinitionMap::value_type( name, ShaderDefinition( shaderTemplate.get(), ShaderArguments(), "" ) ) ).first;
	}

	// create shader from existing definition
	ShaderPointer pShader( new CShader( ( *i ).second ) );
	pShader->setName( name );
	g_ActiveShaders.insert( shaders_t::value_type( name, pShader ) );
	g_ActiveShadersChangedNotify();
	return pShader;
}

IShader *Shader_ForName( const char *name ){
	ASSERT_NOTNULL( name );

	IShader *pShader = Try_Shader_ForName( name );
	pShader->IncRef();
	return pShader;
}




// the list of scripts/*.shader files we need to work with
// those are listed in shaderlist file
std::vector<CopiedString> l_shaderfiles;

/*
   ==================
   DumpUnreferencedShaders
   useful function: dumps the list of .shader files that are not referenced to the console
   ==================
 */
void IfFound_dumpUnreferencedShader( bool& bFound, const char* filename ){
	bool listed = false;

	for ( const CopiedString& sh : l_shaderfiles )
	{
		if ( !strcmp( sh.c_str(), filename ) ) {
			listed = true;
			break;
		}
	}

	if ( !listed ) {
		if ( !bFound ) {
			bFound = true;
			globalOutputStream() << "Following shader files are not referenced in any shaderlist.txt:\n";
		}
		globalOutputStream() << '\t' << filename << '\n';
	}
}
typedef ReferenceCaller<bool, void(const char*), IfFound_dumpUnreferencedShader> IfFoundDumpUnreferencedShaderCaller;

void DumpUnreferencedShaders(){
	bool bFound = false;
	GlobalFileSystem().forEachFile( g_shadersDirectory, g_shadersExtension, IfFoundDumpUnreferencedShaderCaller( bFound ) );
}

void ShaderList_addShaderFile( const char* dirstring ){
	bool found = false;

	for ( const CopiedString& sh : l_shaderfiles )
	{
		if ( string_equal_nocase( dirstring, sh.c_str() ) ) {
			found = true;
			globalOutputStream() << "duplicate entry " << Quoted( sh ) << " in shaderlist.txt\n";
			break;
		}
	}

	if ( !found ) {
		l_shaderfiles.emplace_back( dirstring );
	}
}

/*
   ==================
   BuildShaderList
   build a CStringList of shader names
   ==================
 */
void BuildShaderList( TextInputStream& shaderlist ){
	Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewSimpleTokeniser( shaderlist );
	StringOutputStream shaderFile( 64 );
	for( const char* token; tokeniser.nextLine(), token = tokeniser.getToken(); )
	{
		// each token should be a shader filename
		shaderFile( token );
		if( !path_extension_is( token, g_shadersExtension ) )
			shaderFile << '.' << g_shadersExtension;

		ShaderList_addShaderFile( shaderFile );
	}
	tokeniser.release();
}

void ShaderList_addFromArchive( const char *archivename ){
	const char *shaderpath = GlobalRadiant().getGameDescriptionKeyValue( "shaderpath" );
	if ( string_empty( shaderpath ) ) {
		return;
	}

	Archive *archive = GlobalFileSystem().getArchive( archivename, false );
	if ( archive ) {
		ArchiveTextFile *file = archive->openTextFile( StringStream<64>( DirectoryCleaned( shaderpath ), "shaderlist.txt" ) );
		if ( file ) {
			globalOutputStream() << "Found shaderlist.txt in " << archivename << '\n';
			BuildShaderList( file->getInputStream() );
			file->release();
		}
	}
}

#include "stream/filestream.h"

bool shaderlist_findOrInstall( const char* enginePath, const char* toolsPath, const char* shaderPath, const char* gamename ){
	const auto absShaderList = StringStream( enginePath, gamename, '/', shaderPath, "shaderlist.txt" );
	if ( file_exists( absShaderList ) ) {
		return true;
	}
	{
		const auto directory = StringStream( enginePath, gamename, '/', shaderPath );
		if ( !file_exists( directory ) && !Q_mkdir( directory ) ) {
			return false;
		}
	}
	{
		const auto defaultShaderList = StringStream( toolsPath, gamename, '/', "default_shaderlist.txt" );
		if ( file_exists( defaultShaderList ) ) {
			return file_copy( defaultShaderList, absShaderList );
		}
	}
	return false;
}

void Shaders_Load(){
	if ( g_shaderLanguage == SHADERLANGUAGE_QUAKE4 ) {
		GlobalFileSystem().forEachFile( "guides/", "guide", makeCallbackF( loadGuideFile ), 0 );
	}

	const char* shaderPath = GlobalRadiant().getGameDescriptionKeyValue( "shaderpath" );
	if ( !string_empty( shaderPath ) ) {
		const auto path = StringStream<64>( DirectoryCleaned( shaderPath ) );

		if ( g_useShaderList ) {
			// preload shader files that have been listed in shaderlist.txt
			const char* basegame = GlobalRadiant().getRequiredGameDescriptionKeyValue( "basegame" );
			const char* gamename = GlobalRadiant().getGameName();
			const char* enginePath = GlobalRadiant().getEnginePath();
			const char* toolsPath = GlobalRadiant().getGameToolsPath();

			bool isMod = !string_equal( basegame, gamename );

			if ( !isMod || !shaderlist_findOrInstall( enginePath, toolsPath, path, gamename ) ) {
				gamename = basegame;
				shaderlist_findOrInstall( enginePath, toolsPath, path, gamename );
			}

			GlobalFileSystem().forEachArchive( makeCallbackF( ShaderList_addFromArchive ), false, true );
			if( !l_shaderfiles.empty() ){
				DumpUnreferencedShaders();
			}
			else{
				globalOutputStream() << "No shaderlist.txt found: loading all shaders\n";
				GlobalFileSystem().forEachFile( path, g_shadersExtension, makeCallbackF( ShaderList_addShaderFile ), 1 );
			}
		}
		else
		{
			GlobalFileSystem().forEachFile( path, g_shadersExtension, makeCallbackF( ShaderList_addShaderFile ), 0 );
		}

		StringOutputStream shadername( 256 );
		for( const CopiedString& sh : l_shaderfiles )
		{
			LoadShaderFile( shadername( path, sh ) );
		}
	}

	//StringPool_analyse( ShaderPool::instance() );
}

void Shaders_Free(){
	FreeShaders();
	l_shaderfiles.clear();
	g_shaderFilenames.clear();
}

ModuleObservers g_observers;

std::size_t g_shaders_unrealised = 1; // wait until filesystem and is realised before loading anything
bool Shaders_realised(){
	return g_shaders_unrealised == 0;
}
void Shaders_Realise(){
	if ( --g_shaders_unrealised == 0 ) {
		Shaders_Load();
		g_observers.realise();
	}
}
void Shaders_Unrealise(){
	if ( ++g_shaders_unrealised == 1 ) {
		g_observers.unrealise();
		Shaders_Free();
	}
}

void Shaders_Refresh(){
	Shaders_Unrealise();
	Shaders_Realise();
}

class Quake3ShaderSystem : public ShaderSystem, public ModuleObserver
{
public:
	void realise() override {
		Shaders_Realise();
	}
	void unrealise() override {
		Shaders_Unrealise();
	}
	void refresh() override {
		Shaders_Refresh();
	}

	IShader* getShaderForName( const char* name ) override {
		return Shader_ForName( name );
	}
	IShader* createShaderFromText( const char* shaderText, const char* shaderName ) override {
		if ( shaderText == 0 ) {
			return 0;
		}

		BufferInputStream stream( shaderText, string_length( shaderText ) );
		Tokeniser& tokeniser = GlobalScriptLibrary().m_pfnNewScriptTokeniser( stream );

		ShaderTemplate* shaderTemplate = ParseShaderTextForPreview( tokeniser, shaderName );
		tokeniser.release();

		if ( shaderTemplate == 0 ) {
			return 0;
		}

		ShaderTemplatePointer shaderTemplatePtr( shaderTemplate );
		ShaderArguments args;
		const bool hasName = shaderName != 0 && !string_empty( shaderName );
		const char* previewFilename = hasName ? shaderName : "preview";
		auto *shader = new CShader( shaderTemplatePtr, args, previewFilename );
		shader->setName( hasName ? shaderName : shaderTemplatePtr->getName() );
		shader->IncRef();
		return shader;
	}

	void foreachShaderName( const ShaderNameCallback& callback ) override {
		for ( const auto& [ name, shader ] : g_shaderDefinitions )
		{
			callback( name.c_str() );
		}
	}

	void beginActiveShadersIterator() override {
		ActiveShaders_IteratorBegin();
	}
	bool endActiveShadersIterator() override {
		return ActiveShaders_IteratorAtEnd();
	}
	IShader* dereferenceActiveShadersIterator() override {
		return ActiveShaders_IteratorCurrent();
	}
	void incrementActiveShadersIterator() override {
		ActiveShaders_IteratorIncrement();
	}
	void setActiveShadersChangedNotify( const Callback<void()>& notify ) override {
		g_ActiveShadersChangedNotify = notify;
	}

	void attach( ModuleObserver& observer ) override {
		g_observers.attach( observer );
	}
	void detach( ModuleObserver& observer ) override {
		g_observers.detach( observer );
	}

	void setLightingEnabled( bool enabled ) override {
		if ( CShader::m_lightingEnabled != enabled ) {
			for ( const auto& [ name, shader ] : g_ActiveShaders )
			{
				shader->unrealiseLighting();
			}
			CShader::m_lightingEnabled = enabled;
			for ( const auto& [ name, shader ] : g_ActiveShaders )
			{
				shader->realiseLighting();
			}
		}
	}

	const char* getTexturePrefix() const override {
		return g_texturePrefix;
	}
};

Quake3ShaderSystem g_Quake3ShaderSystem;

ShaderSystem& GetShaderSystem(){
	return g_Quake3ShaderSystem;
}

void Shaders_Construct(){
	GlobalFileSystem().attach( g_Quake3ShaderSystem );
}
void Shaders_Destroy(){
	GlobalFileSystem().detach( g_Quake3ShaderSystem );

	if ( Shaders_realised() ) {
		Shaders_Free();
	}
}
