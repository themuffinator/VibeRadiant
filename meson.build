project(
  'VibeRadiant',
  ['c', 'cpp'],
  meson_version: '>=0.63.0',
  default_options: [
    'warning_level=2',
    'cpp_std=c++20',
  ],
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
sh = find_program('sh', required: true)
git_prog = find_program('git', required: false)

system_name = host_machine.system()
windows = system_name == 'windows'
linux = system_name == 'linux'
darwin = system_name == 'darwin'

if not windows and not linux and not darwin
  error('Unsupported OS: ' + system_name)
endif

build_profile = get_option('build_profile')
qt_major = get_option('qt_major')

version_number = run_command(sh, '-c', 'cat VERSION', check: true).stdout().strip()
version_parts = version_number.split('.')
radiant_major = version_parts.length() > 1 ? version_parts[1] : '0'
radiant_minor = version_parts.length() > 2 ? version_parts[2] : '0'

q3map_version = '2.5.17n'
git_hash = ''
if git_prog.found()
  git_hash = run_command(git_prog, 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
endif

radiant_version = version_number
if git_hash != ''
  radiant_version += '-git-' + git_hash
  q3map_version += '-git-' + git_hash
endif

radiant_exec_suffix = windows ? 'exe' : host_machine.cpu_family()

q3map2_resources = []
q2map_resources = []
qdata3_resources = []
h2data_resources = []
radiant_resources = []
if windows
  windows_module = import('windows')

  q3map2_icon_config = configuration_data()
  q3map2_icon_config.set('ICON_FILE', meson.project_source_root() / 'icons/q3map2.ico')
  q3map2_icon_rc = configure_file(input: 'icons/app-icon.rc.in', output: 'q3map2-icon.rc', configuration: q3map2_icon_config)
  q3map2_resources = windows_module.compile_resources(q3map2_icon_rc, depend_files: files('icons/q3map2.ico'))

  q2map_icon_config = configuration_data()
  q2map_icon_config.set('ICON_FILE', meson.project_source_root() / 'icons/q2map.ico')
  q2map_icon_rc = configure_file(input: 'icons/app-icon.rc.in', output: 'q2map-icon.rc', configuration: q2map_icon_config)
  q2map_resources = windows_module.compile_resources(q2map_icon_rc, depend_files: files('icons/q2map.ico'))

  qdata3_icon_config = configuration_data()
  qdata3_icon_config.set('ICON_FILE', meson.project_source_root() / 'icons/qdata3.ico')
  qdata3_icon_rc = configure_file(input: 'icons/app-icon.rc.in', output: 'qdata3-icon.rc', configuration: qdata3_icon_config)
  qdata3_resources = windows_module.compile_resources(qdata3_icon_rc, depend_files: files('icons/qdata3.ico'))

  h2data_icon_config = configuration_data()
  h2data_icon_config.set('ICON_FILE', meson.project_source_root() / 'icons/h2data.ico')
  h2data_icon_rc = configure_file(input: 'icons/app-icon.rc.in', output: 'h2data-icon.rc', configuration: h2data_icon_config)
  h2data_resources = windows_module.compile_resources(h2data_icon_rc, depend_files: files('icons/h2data.ico'))

  radiant_icon_config = configuration_data()
  radiant_icon_config.set('ICON_FILE', meson.project_source_root() / 'icons/radiant.ico')
  radiant_icon_rc = configure_file(input: 'icons/app-icon.rc.in', output: 'radiant-icon.rc', configuration: radiant_icon_config)
  radiant_resources = windows_module.compile_resources(radiant_icon_rc, depend_files: files('icons/radiant.ico'))
endif

common_compile_args = [
  '-W',
  '-Wall',
  '-Wcast-align',
  '-Wcast-qual',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-fno-strict-aliasing',
]

c_warning_suppress = [
  '-Wno-maybe-uninitialized',
  '-Wno-deprecated-declarations',
  '-Wno-sign-compare',
  '-Wno-pointer-sign',
  '-Wno-int-to-pointer-cast',
  '-Wno-pointer-to-int-cast',
  '-Wno-unused-but-set-parameter',
  '-Wno-cast-function-type',
  '-Wno-stringop-truncation',
  '-Wno-absolute-value',
  '-Wno-type-limits',
  '-Wno-array-bounds',
  '-Wno-switch',
  '-Wno-extra',
  '-Wno-cast-qual',
]

cpp_warning_suppress = [
  '-Wno-maybe-uninitialized',
  '-Wno-deprecated-declarations',
  '-Wno-sign-compare',
  '-Wno-unused-but-set-parameter',
  '-Wno-class-memaccess',
  '-Wno-dangling-pointer',
  '-Wno-stringop-truncation',
  '-Wno-type-limits',
  '-Wno-array-bounds',
  '-Wno-switch',
  '-Wno-extra',
  '-Wno-cast-qual',
]

profile_compile_args = []
profile_link_args = []
profile_define_symbols = []

if build_profile == 'debug'
  profile_compile_args += ['-ggdb', '-Og']
elif build_profile == 'extradebug'
  profile_compile_args += ['-ggdb3']
  profile_define_symbols += ['_DEBUG']
elif build_profile == 'extradebug_quicker'
  profile_compile_args += ['-ggdb3']
  profile_define_symbols += ['_DEBUG', '_DEBUG_QUICKER']
elif build_profile == 'profile'
  profile_compile_args += ['-g', '-O', '-pg']
  profile_link_args += ['-pg']
elif build_profile == 'release'
  profile_compile_args += ['-O3']
  profile_define_symbols += ['NDEBUG']
  profile_link_args += ['-s']
elif build_profile == 'native'
  profile_compile_args += ['-O3', '-march=native', '-mtune=native']
  profile_define_symbols += ['NDEBUG']
  profile_link_args += ['-s']
else
  error('Unsupported build profile: ' + build_profile)
endif

os_compile_args = []
os_define_symbols = []
os_common_deps = []

if windows
  os_compile_args += ['-mms-bitfields']
  os_define_symbols += ['WIN32', '_WIN32', '_inline=inline']
  os_common_deps += [
    cc.find_library('ws2_32', required: true),
    cc.find_library('user32', required: true),
    cc.find_library('gdi32', required: true),
    cc.find_library('ole32', required: true),
  ]
else
  os_compile_args += ['-fPIC']
  os_define_symbols += ['POSIX', 'XWINDOWS']
  if linux
    os_common_deps += [dependency('threads', required: true)]
  endif
endif

profile_define_args = []
foreach sym : profile_define_symbols
  profile_define_args += ['-D' + sym]
endforeach

os_define_args = []
foreach sym : os_define_symbols
  os_define_args += ['-D' + sym]
endforeach

version_define_args = [
  '-DRADIANT_VERSION_NUMBER="' + version_number + '"',
  '-DRADIANT_VERSION="' + radiant_version + '"',
  '-DRADIANT_MAJOR_VERSION="' + radiant_major + '"',
  '-DRADIANT_MINOR_VERSION="' + radiant_minor + '"',
  '-DRADIANT_ABOUTMSG="' + get_option('radiant_aboutmsg') + '"',
  '-DQ3MAP_VERSION="' + q3map_version + '"',
  '-DRADIANT_EXECUTABLE="' + radiant_exec_suffix + '"',
  '-DRADIANT_UPDATE_URL="' + get_option('radiant_update_url') + '"',
  '-DRADIANT_RELEASES_URL="' + get_option('radiant_releases_url') + '"',
  '-DRADIANT_GITHUB_REPO="' + get_option('radiant_github_repo') + '"',
]
qt_keyword_args = ['-DQT_NO_KEYWORDS']

# Enforce C++20 for older build directories where compiler options can remain at cpp_std=none.
cpp20_fallback_args = []
if get_option('cpp_std') == 'none'
  cpp20_arg = ''
  foreach arg : ['-std=gnu++20', '-std=c++20', '/std:c++20']
    if cpp.has_argument(arg)
      cpp20_arg = arg
      break
    endif
  endforeach
  if cpp20_arg == ''
    error('A compiler with C++20 support is required.')
  endif
  cpp20_fallback_args += [cpp20_arg]
endif

project_c_args = common_compile_args + c_warning_suppress + profile_compile_args + os_compile_args + profile_define_args + os_define_args + version_define_args + qt_keyword_args
project_cpp_args = cpp20_fallback_args + common_compile_args + cpp_warning_suppress + [
  '-Wreorder',
  '-fno-exceptions',
  '-fno-rtti',
] + profile_compile_args + os_compile_args + profile_define_args + os_define_args + version_define_args + qt_keyword_args

add_project_arguments(project_c_args, language: 'c')
add_project_arguments(project_cpp_args, language: 'cpp')
if profile_link_args.length() > 0
  add_project_link_arguments(profile_link_args, language: ['c', 'cpp'])
endif

glib_dep = dependency('glib-2.0', required: true)
xml_dep = dependency('libxml-2.0', required: true)
png_dep = dependency('libpng', required: true)
jpeg_dep = dependency('libjpeg', required: false)
if not jpeg_dep.found()
  jpeg_dep = dependency('libjpeg-turbo', required: true)
endif
zlib_dep = dependency('zlib', required: true)

if windows
  opengl_dep = cc.find_library('opengl32', required: true)
else
  opengl_dep = dependency('opengl', required: true)
endif

if darwin
  dl_dep = disabler()
  x11_dep = dependency('x11', required: false)
  if x11_dep.found()
    os_common_deps += [x11_dep]
  endif
elif windows
  dl_dep = disabler()
else
  dl_dep = cc.find_library('dl', required: true)
endif

qt_pkg = qt_major == '6' ? 'qt6' : 'qt5'
qt_core_dep = dependency(qt_pkg, modules: ['Core'], required: true)
qt_gui_dep = dependency(qt_pkg, modules: ['Gui'], required: true)
qt_widgets_dep = dependency(qt_pkg, modules: ['Widgets'], required: true)
qt_svg_dep = dependency(qt_pkg, modules: ['Svg'], required: true)
qt_network_dep = dependency(qt_pkg, modules: ['Network'], required: true)
qt_multimedia_dep = dependency(qt_pkg, modules: ['Multimedia'], required: true)
qt_opengl_dep = dependency(qt_pkg, modules: ['OpenGL'], required: true)
qt_openglwidgets_dep = dependency(qt_pkg, modules: ['OpenGLWidgets'], required: qt_major == '6')
if qt_major == '5' and not qt_openglwidgets_dep.found()
  qt_openglwidgets_dep = qt_widgets_dep
endif

inc_libs = include_directories('libs')
inc_include = include_directories('include')
inc_libs_include = include_directories('libs', 'include')
inc_tools_q3_common = include_directories('tools/quake3/common')
inc_tools_q2_common = include_directories('tools/quake2/common')
inc_tools_h2_common = include_directories(
  'tools/quake2/qdata_heretic2/common',
  'tools/quake2/qdata_heretic2/qcommon',
  'tools/quake2/qdata_heretic2',
  'tools/quake2/common',
)
inc_assimp_public = include_directories('libs/assimp/include')
inc_assimp_internal = include_directories(
  'libs/assimp/include',
  'libs/assimp/code',
  'libs/assimp/contrib/pugixml/src',
  'libs/assimp/contrib/unzip',
  'libs/assimp',
  'libs/assimp/contrib/openddlparser/include',
  'libs/assimp/contrib/rapidjson/include',
  'libs/assimp/contrib',
)

assimp_internal_enabled = get_option('assimp_internal')
assimp_include_define = assimp_internal_enabled ? '-DASSIMP_INCLUDE(x)=#x' : '-DASSIMP_INCLUDE(x)=<x>'
assimp_target_dep = declare_dependency()

# This file is generated from Makefile source lists to keep Meson target inputs aligned.

src_libcommandlib = files(
  'libs/commandlib.cpp',
)

src_libcrnlib = files(
  'libs/crnlib/crnlib.cpp',
)

src_libddslib = files(
  'libs/ddslib/ddslib.c',
)

src_libetclib = files(
  'libs/etclib.c',
)

src_libfilematch = files(
  'libs/filematch.c',
)

src_libgtkutil = files(
  'libs/gtkutil/accelerator.cpp',
  'libs/gtkutil/clipboard.cpp',
  'libs/gtkutil/dialog.cpp',
  'libs/gtkutil/entry.cpp',
  'libs/gtkutil/filechooser.cpp',
  'libs/gtkutil/glfont.cpp',
  'libs/gtkutil/glwidget.cpp',
  'libs/gtkutil/guisettings.cpp',
  'libs/gtkutil/i18n.cpp',
  'libs/gtkutil/idledraw.cpp',
  'libs/gtkutil/image.cpp',
  'libs/gtkutil/menu.cpp',
  'libs/gtkutil/messagebox.cpp',
  'libs/gtkutil/nonmodal.cpp',
  'libs/gtkutil/toolbar.cpp',
  'libs/gtkutil/widget.cpp',
  'libs/gtkutil/xorrectangle.cpp',
)

src_libl_net = files(
  'libs/l_net/l_net.c',
  'libs/l_net/l_net_wins.c',
)

src_libmathlib = files(
  'libs/mathlib/bbox.c',
  'libs/mathlib/line.c',
  'libs/mathlib/m4x4.c',
  'libs/mathlib/mathlib.c',
  'libs/mathlib/ray.c',
)

src_libpicomodel = files(
  'libs/picomodel/lwo/clip.c',
  'libs/picomodel/lwo/envelope.c',
  'libs/picomodel/lwo/list.c',
  'libs/picomodel/lwo/lwio.c',
  'libs/picomodel/lwo/lwo2.c',
  'libs/picomodel/lwo/lwob.c',
  'libs/picomodel/lwo/pntspols.c',
  'libs/picomodel/lwo/surface.c',
  'libs/picomodel/lwo/vecmath.c',
  'libs/picomodel/lwo/vmap.c',
  'libs/picomodel/picointernal.c',
  'libs/picomodel/picomodel.c',
  'libs/picomodel/picomodules.c',
  'libs/picomodel/pm_3ds.c',
  'libs/picomodel/pm_ase.c',
  'libs/picomodel/pm_fm.c',
  'libs/picomodel/pm_lwo.c',
  'libs/picomodel/pm_md2.c',
  'libs/picomodel/pm_md3.c',
  'libs/picomodel/pm_mdc.c',
  'libs/picomodel/pm_ms3d.c',
  'libs/picomodel/pm_obj.c',
  'libs/picomodel/pm_terrain.c',
)

src_libquickhull = files(
  'libs/quickhull/QuickHull.cpp',
)

src_libwebplib = files(
  'libs/webplib/webplib.cpp',
)

src_libxmllib = files(
  'libs/xml/ixml.cpp',
  'libs/xml/xmlelement.cpp',
  'libs/xml/xmlparser.cpp',
  'libs/xml/xmltextags.cpp',
  'libs/xml/xmlwriter.cpp',
)

src_h2data = files(
  'tools/quake2/qdata_heretic2/common/bspfile.c',
  'tools/quake2/qdata_heretic2/common/cmdlib.c',
  'tools/quake2/qdata_heretic2/common/inout.c',
  'tools/quake2/qdata_heretic2/common/l3dslib.c',
  'tools/quake2/qdata_heretic2/common/lbmlib.c',
  'tools/quake2/qdata_heretic2/common/mathlib.c',
  'tools/quake2/qdata_heretic2/common/md4.c',
  'tools/quake2/qdata_heretic2/common/path_init.c',
  'tools/quake2/qdata_heretic2/common/qfiles.c',
  'tools/quake2/qdata_heretic2/common/scriplib.c',
  'tools/quake2/qdata_heretic2/common/threads.c',
  'tools/quake2/qdata_heretic2/common/token.c',
  'tools/quake2/qdata_heretic2/common/trilib.c',
  'tools/quake2/qdata_heretic2/qcommon/reference.c',
  'tools/quake2/qdata_heretic2/qcommon/resourcemanager.c',
  'tools/quake2/qdata_heretic2/qcommon/skeletons.c',
  'tools/quake2/qdata_heretic2/animcomp.c',
  'tools/quake2/qdata_heretic2/book.c',
  'tools/quake2/qdata_heretic2/fmodels.c',
  'tools/quake2/qdata_heretic2/images.c',
  'tools/quake2/qdata_heretic2/jointed.c',
  'tools/quake2/qdata_heretic2/models.c',
  'tools/quake2/qdata_heretic2/pics.c',
  'tools/quake2/qdata_heretic2/qdata.c',
  'tools/quake2/qdata_heretic2/qd_skeletons.c',
  'tools/quake2/qdata_heretic2/sprites.c',
  'tools/quake2/qdata_heretic2/svdcmp.c',
  'tools/quake2/qdata_heretic2/tables.c',
  'tools/quake2/qdata_heretic2/tmix.c',
  'tools/quake2/qdata_heretic2/video.c',
)

src_libassimp_ = files(
  'libs/assimp/code/Common/Assimp.cpp',
  'libs/assimp/code/CApi/CInterfaceIOWrapper.cpp',
  'libs/assimp/code/Common/BaseImporter.cpp',
  'libs/assimp/code/Common/BaseProcess.cpp',
  'libs/assimp/code/Common/PostStepRegistry.cpp',
  'libs/assimp/code/Common/ImporterRegistry.cpp',
  'libs/assimp/code/Common/DefaultIOStream.cpp',
  'libs/assimp/code/Common/DefaultIOSystem.cpp',
  'libs/assimp/code/Common/ZipArchiveIOSystem.cpp',
  'libs/assimp/code/Common/Importer.cpp',
  'libs/assimp/code/Common/SGSpatialSort.cpp',
  'libs/assimp/code/Common/VertexTriangleAdjacency.cpp',
  'libs/assimp/code/Common/SpatialSort.cpp',
  'libs/assimp/code/Common/SceneCombiner.cpp',
  'libs/assimp/code/Common/ScenePreprocessor.cpp',
  'libs/assimp/code/Common/SkeletonMeshBuilder.cpp',
  'libs/assimp/code/Common/StandardShapes.cpp',
  'libs/assimp/code/Common/TargetAnimation.cpp',
  'libs/assimp/code/Common/RemoveComments.cpp',
  'libs/assimp/code/Common/Subdivision.cpp',
  'libs/assimp/code/Common/scene.cpp',
  'libs/assimp/code/Common/Bitmap.cpp',
  'libs/assimp/code/Common/Version.cpp',
  'libs/assimp/code/Common/CreateAnimMesh.cpp',
  'libs/assimp/code/Common/simd.cpp',
  'libs/assimp/code/Common/material.cpp',
  'libs/assimp/code/Common/AssertHandler.cpp',
  'libs/assimp/code/Common/Exceptional.cpp',
  'libs/assimp/code/Common/DefaultLogger.cpp',
  'libs/assimp/code/PostProcessing/CalcTangentsProcess.cpp',
  'libs/assimp/code/PostProcessing/ComputeUVMappingProcess.cpp',
  'libs/assimp/code/PostProcessing/ConvertToLHProcess.cpp',
  'libs/assimp/code/PostProcessing/EmbedTexturesProcess.cpp',
  'libs/assimp/code/PostProcessing/FindDegenerates.cpp',
  'libs/assimp/code/PostProcessing/FindInstancesProcess.cpp',
  'libs/assimp/code/PostProcessing/FindInvalidDataProcess.cpp',
  'libs/assimp/code/PostProcessing/FixNormalsStep.cpp',
  'libs/assimp/code/PostProcessing/DropFaceNormalsProcess.cpp',
  'libs/assimp/code/PostProcessing/GenFaceNormalsProcess.cpp',
  'libs/assimp/code/PostProcessing/GenVertexNormalsProcess.cpp',
  'libs/assimp/code/PostProcessing/PretransformVertices.cpp',
  'libs/assimp/code/PostProcessing/ImproveCacheLocality.cpp',
  'libs/assimp/code/PostProcessing/JoinVerticesProcess.cpp',
  'libs/assimp/code/PostProcessing/LimitBoneWeightsProcess.cpp',
  'libs/assimp/code/PostProcessing/RemoveRedundantMaterials.cpp',
  'libs/assimp/code/PostProcessing/RemoveVCProcess.cpp',
  'libs/assimp/code/PostProcessing/SortByPTypeProcess.cpp',
  'libs/assimp/code/PostProcessing/SplitLargeMeshes.cpp',
  'libs/assimp/code/PostProcessing/TextureTransform.cpp',
  'libs/assimp/code/PostProcessing/TriangulateProcess.cpp',
  'libs/assimp/code/PostProcessing/ValidateDataStructure.cpp',
  'libs/assimp/code/PostProcessing/OptimizeGraph.cpp',
  'libs/assimp/code/PostProcessing/OptimizeMeshes.cpp',
  'libs/assimp/code/PostProcessing/DeboneProcess.cpp',
  'libs/assimp/code/PostProcessing/ProcessHelper.cpp',
  'libs/assimp/code/PostProcessing/MakeVerboseFormat.cpp',
  'libs/assimp/code/PostProcessing/ScaleProcess.cpp',
  'libs/assimp/code/PostProcessing/ArmaturePopulate.cpp',
  'libs/assimp/code/PostProcessing/GenBoundingBoxesProcess.cpp',
  'libs/assimp/code/PostProcessing/SplitByBoneCountProcess.cpp',
  'libs/assimp/code/Material/MaterialSystem.cpp',
  'libs/assimp/code/AssetLib/STEPParser/STEPFileReader.cpp',
  'libs/assimp/code/AssetLib/STEPParser/STEPFileEncoding.cpp',
  'libs/assimp/code/AssetLib/AMF/AMFImporter.cpp',
  'libs/assimp/code/AssetLib/AMF/AMFImporter_Geometry.cpp',
  'libs/assimp/code/AssetLib/AMF/AMFImporter_Material.cpp',
  'libs/assimp/code/AssetLib/AMF/AMFImporter_Postprocess.cpp',
  'libs/assimp/code/AssetLib/3DS/3DSConverter.cpp',
  'libs/assimp/code/AssetLib/3DS/3DSLoader.cpp',
  'libs/assimp/code/AssetLib/AC/ACLoader.cpp',
  'libs/assimp/code/AssetLib/ASE/ASELoader.cpp',
  'libs/assimp/code/AssetLib/ASE/ASEParser.cpp',
  'libs/assimp/code/AssetLib/Assbin/AssbinLoader.cpp',
  'libs/assimp/code/AssetLib/B3D/B3DImporter.cpp',
  'libs/assimp/code/AssetLib/BVH/BVHLoader.cpp',
  'libs/assimp/code/AssetLib/Collada/ColladaHelper.cpp',
  'libs/assimp/code/AssetLib/Collada/ColladaLoader.cpp',
  'libs/assimp/code/AssetLib/Collada/ColladaParser.cpp',
  'libs/assimp/code/AssetLib/DXF/DXFLoader.cpp',
  'libs/assimp/code/AssetLib/CSM/CSMLoader.cpp',
  'libs/assimp/code/AssetLib/HMP/HMPLoader.cpp',
  'libs/assimp/code/AssetLib/Irr/IRRMeshLoader.cpp',
  'libs/assimp/code/AssetLib/Irr/IRRShared.cpp',
  'libs/assimp/code/AssetLib/Irr/IRRLoader.cpp',
  'libs/assimp/code/AssetLib/LWO/LWOAnimation.cpp',
  'libs/assimp/code/AssetLib/LWO/LWOBLoader.cpp',
  'libs/assimp/code/AssetLib/LWO/LWOLoader.cpp',
  'libs/assimp/code/AssetLib/LWO/LWOMaterial.cpp',
  'libs/assimp/code/AssetLib/LWS/LWSLoader.cpp',
  'libs/assimp/code/AssetLib/M3D/M3DImporter.cpp',
  'libs/assimp/code/AssetLib/M3D/M3DWrapper.cpp',
  'libs/assimp/code/AssetLib/MD2/MD2Loader.cpp',
  'libs/assimp/code/AssetLib/MD3/MD3Loader.cpp',
  'libs/assimp/code/AssetLib/MD5/MD5Loader.cpp',
  'libs/assimp/code/AssetLib/MD5/MD5Parser.cpp',
  'libs/assimp/code/AssetLib/MDC/MDCLoader.cpp',
  'libs/assimp/code/AssetLib/MDL/MDLLoader.cpp',
  'libs/assimp/code/AssetLib/MDL/MDLMaterialLoader.cpp',
  'libs/assimp/code/AssetLib/MDL/HalfLife/HL1MDLLoader.cpp',
  'libs/assimp/code/AssetLib/MDL/HalfLife/UniqueNameGenerator.cpp',
  'libs/assimp/code/AssetLib/NFF/NFFLoader.cpp',
  'libs/assimp/code/AssetLib/NDO/NDOLoader.cpp',
  'libs/assimp/code/AssetLib/OFF/OFFLoader.cpp',
  'libs/assimp/code/AssetLib/Obj/ObjFileImporter.cpp',
  'libs/assimp/code/AssetLib/Obj/ObjFileMtlImporter.cpp',
  'libs/assimp/code/AssetLib/Obj/ObjFileParser.cpp',
  'libs/assimp/code/AssetLib/Ogre/OgreImporter.cpp',
  'libs/assimp/code/AssetLib/Ogre/OgreStructs.cpp',
  'libs/assimp/code/AssetLib/Ogre/OgreBinarySerializer.cpp',
  'libs/assimp/code/AssetLib/Ogre/OgreXmlSerializer.cpp',
  'libs/assimp/code/AssetLib/Ogre/OgreMaterial.cpp',
  'libs/assimp/code/AssetLib/OpenGEX/OpenGEXImporter.cpp',
  'libs/assimp/code/AssetLib/Ply/PlyLoader.cpp',
  'libs/assimp/code/AssetLib/Ply/PlyParser.cpp',
  'libs/assimp/code/AssetLib/MS3D/MS3DLoader.cpp',
  'libs/assimp/code/AssetLib/COB/COBLoader.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderLoader.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderDNA.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderScene.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderModifier.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderBMesh.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderTessellator.cpp',
  'libs/assimp/code/AssetLib/Blender/BlenderCustomData.cpp',
  'libs/assimp/code/AssetLib/XGL/XGLLoader.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXImporter.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXParser.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXTokenizer.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXConverter.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXUtil.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXDocument.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXProperties.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXMeshGeometry.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXMaterial.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXModel.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXAnimation.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXNodeAttribute.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXDeformer.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXBinaryTokenizer.cpp',
  'libs/assimp/code/AssetLib/FBX/FBXDocumentUtil.cpp',
  'libs/assimp/code/AssetLib/IQM/IQMImporter.cpp',
  'libs/assimp/code/AssetLib/Q3D/Q3DLoader.cpp',
  'libs/assimp/code/AssetLib/Q3BSP/Q3BSPFileParser.cpp',
  'libs/assimp/code/AssetLib/Q3BSP/Q3BSPFileImporter.cpp',
  'libs/assimp/code/AssetLib/Raw/RawLoader.cpp',
  'libs/assimp/code/AssetLib/SIB/SIBImporter.cpp',
  'libs/assimp/code/AssetLib/SMD/SMDLoader.cpp',
  'libs/assimp/code/AssetLib/STL/STLLoader.cpp',
  'libs/assimp/code/AssetLib/Terragen/TerragenLoader.cpp',
  'libs/assimp/code/AssetLib/Unreal/UnrealLoader.cpp',
  'libs/assimp/code/AssetLib/X/XFileImporter.cpp',
  'libs/assimp/code/AssetLib/X/XFileParser.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DGeoHelper.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Geometry2D.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Geometry3D.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Group.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Light.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Metadata.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Networking.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Postprocess.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Rendering.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Shape.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DImporter_Texturing.cpp',
  'libs/assimp/code/AssetLib/X3D/X3DXmlHelper.cpp',
  'libs/assimp/code/AssetLib/glTF/glTFCommon.cpp',
  'libs/assimp/code/AssetLib/glTF/glTFImporter.cpp',
  'libs/assimp/code/AssetLib/glTF2/glTF2Importer.cpp',
  'libs/assimp/code/AssetLib/3MF/D3MFImporter.cpp',
  'libs/assimp/code/AssetLib/3MF/D3MFOpcPackage.cpp',
  'libs/assimp/code/AssetLib/3MF/XmlSerializer.cpp',
  'libs/assimp/code/AssetLib/MMD/MMDImporter.cpp',
  'libs/assimp/code/AssetLib/MMD/MMDPmxParser.cpp',
  'libs/assimp/contrib/unzip/crypt.c',
  'libs/assimp/contrib/unzip/ioapi.c',
  'libs/assimp/contrib/unzip/unzip.c',
  'libs/assimp/contrib/clipper/clipper.cpp',
  'libs/assimp/contrib/openddlparser/code/OpenDDLParser.cpp',
  'libs/assimp/contrib/openddlparser/code/DDLNode.cpp',
  'libs/assimp/contrib/openddlparser/code/OpenDDLCommon.cpp',
  'libs/assimp/contrib/openddlparser/code/OpenDDLExport.cpp',
  'libs/assimp/contrib/openddlparser/code/Value.cpp',
  'libs/assimp/contrib/openddlparser/code/OpenDDLStream.cpp',
  'libs/assimp/contrib/Open3DGC/o3dgcArithmeticCodec.cpp',
  'libs/assimp/contrib/Open3DGC/o3dgcDynamicVectorDecoder.cpp',
  'libs/assimp/contrib/Open3DGC/o3dgcDynamicVectorEncoder.cpp',
  'libs/assimp/contrib/Open3DGC/o3dgcTools.cpp',
  'libs/assimp/contrib/Open3DGC/o3dgcTriangleFans.cpp',
  'libs/assimp/contrib/zip/src/zip.c',
)

src_mbspc = files(
  'tools/mbspc/botlib/be_aas_bspq3.c',
  'tools/mbspc/botlib/be_aas_cluster.c',
  'tools/mbspc/botlib/be_aas_move.c',
  'tools/mbspc/botlib/be_aas_optimize.c',
  'tools/mbspc/botlib/be_aas_reach.c',
  'tools/mbspc/botlib/be_aas_sample.c',
  'tools/mbspc/botlib/l_libvar.c',
  'tools/mbspc/botlib/l_precomp.c',
  'tools/mbspc/botlib/l_script.c',
  'tools/mbspc/botlib/l_struct.c',
  'tools/mbspc/mbspc/aas_areamerging.c',
  'tools/mbspc/mbspc/aas_cfg.c',
  'tools/mbspc/mbspc/aas_create.c',
  'tools/mbspc/mbspc/aas_edgemelting.c',
  'tools/mbspc/mbspc/aas_facemerging.c',
  'tools/mbspc/mbspc/aas_file.c',
  'tools/mbspc/mbspc/aas_gsubdiv.c',
  'tools/mbspc/mbspc/aas_map.c',
  'tools/mbspc/mbspc/aas_prunenodes.c',
  'tools/mbspc/mbspc/aas_store.c',
  'tools/mbspc/mbspc/be_aas_bspc.c',
  'tools/mbspc/mbspc/brushbsp.c',
  'tools/mbspc/mbspc/bspc.c',
  'tools/mbspc/mbspc/csg.c',
  'tools/mbspc/mbspc/faces.c',
  'tools/mbspc/mbspc/glfile.c',
  'tools/mbspc/mbspc/l_bsp_ent.c',
  'tools/mbspc/mbspc/l_bsp_hl.c',
  'tools/mbspc/mbspc/l_bsp_q1.c',
  'tools/mbspc/mbspc/l_bsp_q2.c',
  'tools/mbspc/mbspc/l_bsp_q3.c',
  'tools/mbspc/mbspc/l_bsp_sin.c',
  'tools/mbspc/mbspc/l_cmd.c',
  'tools/mbspc/mbspc/l_log.c',
  'tools/mbspc/mbspc/l_math.c',
  'tools/mbspc/mbspc/l_mem.c',
  'tools/mbspc/mbspc/l_poly.c',
  'tools/mbspc/mbspc/l_qfiles.c',
  'tools/mbspc/mbspc/l_threads.c',
  'tools/mbspc/mbspc/l_utils.c',
  'tools/mbspc/mbspc/leakfile.c',
  'tools/mbspc/mbspc/map.c',
  'tools/mbspc/mbspc/map_hl.c',
  'tools/mbspc/mbspc/map_q1.c',
  'tools/mbspc/mbspc/map_q2.c',
  'tools/mbspc/mbspc/map_q3.c',
  'tools/mbspc/mbspc/map_sin.c',
  'tools/mbspc/mbspc/nodraw.c',
  'tools/mbspc/mbspc/portals.c',
  'tools/mbspc/mbspc/prtfile.c',
  'tools/mbspc/mbspc/textures.c',
  'tools/mbspc/mbspc/tree.c',
  'tools/mbspc/mbspc/writebsp.c',
  'tools/mbspc/qcommon/cm_load.c',
  'tools/mbspc/qcommon/cm_patch.c',
  'tools/mbspc/qcommon/cm_test.c',
  'tools/mbspc/qcommon/cm_trace.c',
  'tools/mbspc/qcommon/md4.c',
  'tools/mbspc/qcommon/unzip.c',
)

src_modules_archivepak = files(
  'plugins/archivepak/archive.cpp',
  'plugins/archivepak/pak.cpp',
  'plugins/archivepak/plugin.cpp',
)

src_modules_archivewad = files(
  'plugins/archivewad/archive.cpp',
  'plugins/archivewad/plugin.cpp',
  'plugins/archivewad/wad.cpp',
)

src_modules_archivezip = files(
  'plugins/archivezip/archive.cpp',
  'plugins/archivezip/pkzip.cpp',
  'plugins/archivezip/plugin.cpp',
  'plugins/archivezip/zlibstream.cpp',
)

src_modules_assmodel = files(
  'plugins/assmodel/mdlimage.cpp',
  'plugins/assmodel/model.cpp',
  'plugins/assmodel/plugin.cpp',
)

src_modules_entity = files(
  'plugins/entity/angle.cpp',
  'plugins/entity/angles.cpp',
  'plugins/entity/colour.cpp',
  'plugins/entity/doom3group.cpp',
  'plugins/entity/eclassmodel.cpp',
  'plugins/entity/entity.cpp',
  'plugins/entity/filters.cpp',
  'plugins/entity/generic.cpp',
  'plugins/entity/group.cpp',
  'plugins/entity/light.cpp',
  'plugins/entity/miscmodel.cpp',
  'plugins/entity/model.cpp',
  'plugins/entity/modelskinkey.cpp',
  'plugins/entity/namedentity.cpp',
  'plugins/entity/origin.cpp',
  'plugins/entity/plugin.cpp',
  'plugins/entity/rotation.cpp',
  'plugins/entity/scale.cpp',
  'plugins/entity/skincache.cpp',
  'plugins/entity/targetable.cpp',
)

src_modules_image = files(
  'plugins/image/bmp.cpp',
  'plugins/image/crn.cpp',
  'plugins/image/dds.cpp',
  'plugins/image/image.cpp',
  'plugins/image/jpeg.cpp',
  'plugins/image/ktx.cpp',
  'plugins/image/pcx.cpp',
  'plugins/image/tga.cpp',
  'plugins/image/webp.cpp',
)

src_modules_imagehl = files(
  'plugins/imagehl/hlw.cpp',
  'plugins/imagehl/imagehl.cpp',
  'plugins/imagehl/mip.cpp',
  'plugins/imagehl/sprite.cpp',
)

src_modules_imagepng = files(
  'plugins/imagepng/plugin.cpp',
)

src_modules_imageq2 = files(
  'plugins/imageq2/imageq2.cpp',
  'plugins/imageq2/wal32.cpp',
  'plugins/imageq2/wal.cpp',
)

src_modules_mapq3 = files(
  'plugins/mapq3/parse.cpp',
  'plugins/mapq3/plugin.cpp',
  'plugins/mapq3/write.cpp',
)

src_modules_mapxml = files(
  'plugins/mapxml/plugin.cpp',
  'plugins/mapxml/xmlparse.cpp',
  'plugins/mapxml/xmlwrite.cpp',
)

src_modules_shaders = files(
  'plugins/shaders/plugin.cpp',
  'plugins/shaders/shaders.cpp',
)

src_modules_vfspk3 = files(
  'plugins/vfspk3/archive.cpp',
  'plugins/vfspk3/vfs.cpp',
  'plugins/vfspk3/vfspk3.cpp',
)

src_plugins_bkgrnd2d = files(
  'contrib/bkgrnd2d/bkgrnd2d.cpp',
  'contrib/bkgrnd2d/dialog.cpp',
  'contrib/bkgrnd2d/plugin.cpp',
)

src_plugins_bobtoolz = files(
  'contrib/bobtoolz/bobToolz-GTK.cpp',
  'contrib/bobtoolz/bsploader.cpp',
  'contrib/bobtoolz/cportals.cpp',
  'contrib/bobtoolz/DBobView.cpp',
  'contrib/bobtoolz/DBrush.cpp',
  'contrib/bobtoolz/DEntity.cpp',
  'contrib/bobtoolz/DEPair.cpp',
  'contrib/bobtoolz/dialogs/dialogs-gtk.cpp',
  'contrib/bobtoolz/DMap.cpp',
  'contrib/bobtoolz/DPatch.cpp',
  'contrib/bobtoolz/DPlane.cpp',
  'contrib/bobtoolz/DPoint.cpp',
  'contrib/bobtoolz/DShape.cpp',
  'contrib/bobtoolz/DTrainDrawer.cpp',
  'contrib/bobtoolz/DTreePlanter.cpp',
  'contrib/bobtoolz/DVisDrawer.cpp',
  'contrib/bobtoolz/DWinding.cpp',
  'contrib/bobtoolz/funchandlers-GTK.cpp',
  'contrib/bobtoolz/lists.cpp',
  'contrib/bobtoolz/misc.cpp',
  'contrib/bobtoolz/ScriptParser.cpp',
  'contrib/bobtoolz/shapes.cpp',
  'contrib/bobtoolz/visfind.cpp',
)

src_plugins_brushexport = files(
  'contrib/brushexport/callbacks.cpp',
  'contrib/brushexport/export.cpp',
  'contrib/brushexport/interface.cpp',
  'contrib/brushexport/plugin.cpp',
)

src_plugins_meshtex = files(
  'contrib/meshtex/GeneralFunctionDialog.cpp',
  'contrib/meshtex/GenericDialog.cpp',
  'contrib/meshtex/GenericMainMenu.cpp',
  'contrib/meshtex/GenericPluginUI.cpp',
  'contrib/meshtex/GetInfoDialog.cpp',
  'contrib/meshtex/MainMenu.cpp',
  'contrib/meshtex/MeshEntity.cpp',
  'contrib/meshtex/MeshVisitor.cpp',
  'contrib/meshtex/PluginModule.cpp',
  'contrib/meshtex/PluginRegistration.cpp',
  'contrib/meshtex/PluginUI.cpp',
  'contrib/meshtex/RefCounted.cpp',
  'contrib/meshtex/SetScaleDialog.cpp',
)

src_plugins_prtview = files(
  'contrib/prtview/AboutDialog.cpp',
  'contrib/prtview/ConfigDialog.cpp',
  'contrib/prtview/LoadPortalFileDialog.cpp',
  'contrib/prtview/portals.cpp',
  'contrib/prtview/prtview.cpp',
)

src_plugins_shaderplug = files(
  'contrib/shaderplug/shaderplug.cpp',
)

src_plugins_sunplug = files(
  'contrib/sunplug/sunplug.cpp',
)

src_plugins_ufoaiplug = files(
  'contrib/ufoaiplug/ufoai_filters.cpp',
  'contrib/ufoaiplug/ufoai_gtk.cpp',
  'contrib/ufoaiplug/ufoai_level.cpp',
  'contrib/ufoaiplug/ufoai.cpp',
)

src_q2map = files(
  'tools/quake2/common/bspfile.c',
  'tools/quake2/common/cmdlib.c',
  'tools/quake2/common/inout.c',
  'tools/quake2/common/l3dslib.c',
  'tools/quake2/common/lbmlib.c',
  'tools/quake2/common/mathlib.c',
  'tools/quake2/common/md4.c',
  'tools/quake2/common/path_init.c',
  'tools/quake2/common/polylib.c',
  'tools/quake2/common/scriplib.c',
  'tools/quake2/common/threads.c',
  'tools/quake2/common/trilib.c',
  'tools/quake2/q2map/brushbsp.c',
  'tools/quake2/q2map/csg.c',
  'tools/quake2/q2map/faces.c',
  'tools/quake2/q2map/flow.c',
  'tools/quake2/q2map/glfile.c',
  'tools/quake2/q2map/leakfile.c',
  'tools/quake2/q2map/lightmap.c',
  'tools/quake2/q2map/main.c',
  'tools/quake2/q2map/map.c',
  'tools/quake2/q2map/nodraw.c',
  'tools/quake2/q2map/patches.c',
  'tools/quake2/q2map/portals.c',
  'tools/quake2/q2map/prtfile.c',
  'tools/quake2/q2map/qbsp.c',
  'tools/quake2/q2map/qrad.c',
  'tools/quake2/q2map/qvis.c',
  'tools/quake2/q2map/textures.c',
  'tools/quake2/q2map/trace.c',
  'tools/quake2/q2map/tree.c',
  'tools/quake2/q2map/writebsp.c',
)

src_q3map2 = files(
  'tools/quake3/common/cmdlib.cpp',
  'tools/quake3/common/qimagelib.cpp',
  'tools/quake3/common/inout.cpp',
  'tools/quake3/common/jpeg.cpp',
  'tools/quake3/common/md4.cpp',
  'tools/quake3/common/mutex.cpp',
  'tools/quake3/common/polylib.cpp',
  'tools/quake3/common/scriplib.cpp',
  'tools/quake3/common/threads.cpp',
  'tools/quake3/common/unzip.cpp',
  'tools/quake3/common/vfs.cpp',
  'tools/quake3/common/miniz.cpp',
  'tools/quake3/q3map2/autopk3.cpp',
  'tools/quake3/q3map2/brush.cpp',
  'tools/quake3/q3map2/bspfile_abstract.cpp',
  'tools/quake3/q3map2/bspfile_ibsp.cpp',
  'tools/quake3/q3map2/bspfile_rbsp.cpp',
  'tools/quake3/q3map2/bsp.cpp',
  'tools/quake3/q3map2/convert_ase.cpp',
  'tools/quake3/q3map2/convert_bsp.cpp',
  'tools/quake3/q3map2/convert_json.cpp',
  'tools/quake3/q3map2/convert_map.cpp',
  'tools/quake3/q3map2/convert_obj.cpp',
  'tools/quake3/q3map2/decals.cpp',
  'tools/quake3/q3map2/exportents.cpp',
  'tools/quake3/q3map2/facebsp.cpp',
  'tools/quake3/q3map2/fog.cpp',
  'tools/quake3/q3map2/games.cpp',
  'tools/quake3/q3map2/help.cpp',
  'tools/quake3/q3map2/image.cpp',
  'tools/quake3/q3map2/leakfile.cpp',
  'tools/quake3/q3map2/light_bounce.cpp',
  'tools/quake3/q3map2/lightmaps_ydnar.cpp',
  'tools/quake3/q3map2/light.cpp',
  'tools/quake3/q3map2/light_trace.cpp',
  'tools/quake3/q3map2/light_ydnar.cpp',
  'tools/quake3/q3map2/main.cpp',
  'tools/quake3/q3map2/map.cpp',
  'tools/quake3/q3map2/minimap.cpp',
  'tools/quake3/q3map2/mesh.cpp',
  'tools/quake3/q3map2/model.cpp',
  'tools/quake3/q3map2/patch.cpp',
  'tools/quake3/q3map2/path_init.cpp',
  'tools/quake3/q3map2/portals.cpp',
  'tools/quake3/q3map2/prtfile.cpp',
  'tools/quake3/q3map2/shaders.cpp',
  'tools/quake3/q3map2/surface_extra.cpp',
  'tools/quake3/q3map2/surface_foliage.cpp',
  'tools/quake3/q3map2/surface_fur.cpp',
  'tools/quake3/q3map2/surface_meta.cpp',
  'tools/quake3/q3map2/surface.cpp',
  'tools/quake3/q3map2/tjunction.cpp',
  'tools/quake3/q3map2/tree.cpp',
  'tools/quake3/q3map2/visflow.cpp',
  'tools/quake3/q3map2/vis.cpp',
  'tools/quake3/q3map2/writebsp.cpp',
)

src_qdata3 = files(
  'tools/quake2/common/bspfile.c',
  'tools/quake2/common/cmdlib.c',
  'tools/quake2/common/inout.c',
  'tools/quake2/common/l3dslib.c',
  'tools/quake2/common/lbmlib.c',
  'tools/quake2/common/mathlib.c',
  'tools/quake2/common/md4.c',
  'tools/quake2/common/path_init.c',
  'tools/quake2/common/polylib.c',
  'tools/quake2/common/scriplib.c',
  'tools/quake2/common/threads.c',
  'tools/quake2/common/trilib.c',
  'tools/quake2/qdata/images.c',
  'tools/quake2/qdata/models.c',
  'tools/quake2/qdata/qdata.c',
  'tools/quake2/qdata/sprites.c',
  'tools/quake2/qdata/tables.c',
  'tools/quake2/qdata/video.c',
)

src_radiant = files(
  'radiant/autosave.cpp',
  'radiant/assetbrowser.cpp',
  'radiant/assetdrop.cpp',
  'radiant/brushmanip.cpp',
  'radiant/brushmodule.cpp',
  'radiant/brushnode.cpp',
  'radiant/brush.cpp',
  'radiant/brush_primit.cpp',
  'radiant/brushtokens.cpp',
  'radiant/brushxml.cpp',
  'radiant/build.cpp',
  'radiant/camwindow.cpp',
  'radiant/clippertool.cpp',
  'radiant/colors.cpp',
  'radiant/commands.cpp',
  'radiant/console.cpp',
  'radiant/csg.cpp',
  'radiant/dialog.cpp',
  'radiant/eclass_def.cpp',
  'radiant/eclass_doom3.cpp',
  'radiant/eclass_fgd.cpp',
  'radiant/eclass.cpp',
  'radiant/eclass_xml.cpp',
  'radiant/entitybrowser.cpp',
  'radiant/entityinspector.cpp',
  'radiant/entitylist.cpp',
  'radiant/entity.cpp',
  'radiant/environment.cpp',
  'radiant/error.cpp',
  'radiant/feedback.cpp',
  'radiant/filetypes.cpp',
  'radiant/filterbar.cpp',
  'radiant/filters.cpp',
  'radiant/findtexturedialog.cpp',
  'radiant/genai.cpp',
  'radiant/glwidget.cpp',
  'radiant/grid.cpp',
  'radiant/groupdialog.cpp',
  'radiant/gtkdlgs.cpp',
  'radiant/gtkmisc.cpp',
  'radiant/help.cpp',
  'radiant/image.cpp',
  'radiant/issuebrowser.cpp',
  'radiant/layerswindow.cpp',
  'radiant/linkedgroups.cpp',
  'radiant/localization.cpp',
  'radiant/mainframe.cpp',
  'radiant/main.cpp',
  'radiant/map.cpp',
  'radiant/modelwindow.cpp',
  'radiant/mru.cpp',
  'radiant/nullmodel.cpp',
  'radiant/parse.cpp',
  'radiant/patchdialog.cpp',
  'radiant/patchmanip.cpp',
  'radiant/patchmodule.cpp',
  'radiant/patch.cpp',
  'radiant/pluginapi.cpp',
  'radiant/pluginmanager.cpp',
  'radiant/pluginmenu.cpp',
  'radiant/plugin.cpp',
  'radiant/plugintoolbar.cpp',
  'radiant/points.cpp',
  'radiant/preferencedictionary.cpp',
  'radiant/preferences.cpp',
  'radiant/qe3.cpp',
  'radiant/qgl.cpp',
  'radiant/referencecache.cpp',
  'radiant/renderer.cpp',
  'radiant/renderstate.cpp',
  'radiant/previewlighting.cpp',
  'radiant/scenegraph.cpp',
  'radiant/selection.cpp',
  'radiant/select.cpp',
  'radiant/server.cpp',
  'radiant/sockets.cpp',
  'radiant/soundbrowser.cpp',
  'radiant/stacktrace.cpp',
  'radiant/surfacedialog.cpp',
  'radiant/texmanip.cpp',
  'radiant/textures.cpp',
  'radiant/texwindow.cpp',
  'radiant/theme.cpp',
  'radiant/tools.cpp',
  'radiant/treemodel.cpp',
  'radiant/update.cpp',
  'radiant/undo.cpp',
  'radiant/uvview.cpp',
  'radiant/url.cpp',
  'radiant/view.cpp',
  'radiant/watchbsp.cpp',
  'radiant/winding.cpp',
  'radiant/windowobservers.cpp',
  'radiant/xmlstuff.cpp',
  'radiant/xywindow.cpp',
  'radiant/zwindow.cpp',
)



libcommandlib_lib = static_library(
  'commandlib',
  src_libcommandlib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libcrnlib_lib = static_library(
  'crnlib',
  src_libcrnlib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libddslib_lib = static_library(
  'ddslib',
  src_libddslib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libetclib_lib = static_library(
  'etclib',
  src_libetclib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libfilematch_lib = static_library(
  'filematch',
  src_libfilematch,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libgtkutil_lib = static_library(
  'gtkutil',
  src_libgtkutil,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_openglwidgets_dep],
  install: false,
  implicit_include_directories: false,
)

libl_net_lib = static_library(
  'l_net',
  src_libl_net,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libmathlib_lib = static_library(
  'mathlib',
  src_libmathlib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

picomodel_c_args = []
if cc.has_argument('-Wno-stringop-overflow')
  picomodel_c_args += ['-Wno-stringop-overflow']
endif

libpicomodel_lib = static_library(
  'picomodel',
  src_libpicomodel,
  include_directories: [inc_libs],
  c_args: picomodel_c_args,
  install: false,
  implicit_include_directories: false,
)

libquickhull_lib = static_library(
  'quickhull',
  src_libquickhull,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libwebplib_lib = static_library(
  'webplib',
  src_libwebplib,
  include_directories: [inc_libs],
  install: false,
  implicit_include_directories: false,
)

libxmllib_lib = static_library(
  'xmllib',
  src_libxmllib,
  include_directories: [inc_libs, inc_include],
  dependencies: [xml_dep, glib_dep],
  install: false,
  implicit_include_directories: false,
)

static_deps_h2data = [libl_net_lib]
static_deps_modules_image = [libcrnlib_lib, libddslib_lib, libetclib_lib, libwebplib_lib]
static_deps_modules_shaders = [libcommandlib_lib]
static_deps_modules_vfspk3 = [libfilematch_lib]
static_deps_plugins_bobtoolz = [libcommandlib_lib, libmathlib_lib]
static_deps_plugins_shaderplug = [libxmllib_lib]
static_deps_q2map = [libl_net_lib]
static_deps_q3map2 = [libcrnlib_lib, libddslib_lib, libetclib_lib, libfilematch_lib, libl_net_lib, libwebplib_lib]
static_deps_qdata3 = [libl_net_lib]
static_deps_radiant = [libcommandlib_lib, libgtkutil_lib, libl_net_lib, libquickhull_lib, libxmllib_lib]

if assimp_internal_enabled
  assimp_internal_common_args = [
    '-DASSIMP_BUILD_DLL_EXPORT',
    '-DASSIMP_BUILD_NO_C4D_IMPORTER',
    '-DASSIMP_BUILD_NO_EXPORT',
    '-DASSIMP_BUILD_NO_IFC_IMPORTER',
    '-DASSIMP_BUILD_NO_OWN_ZLIB',
    '-DASSIMP_IMPORTER_GLTF_USE_OPEN3DGC=1',
    '-DMINIZ_USE_UNALIGNED_LOADS_AND_STORES=0',
    '-DOPENDDLPARSER_BUILD',
    '-DRAPIDJSON_HAS_STDSTRING=1',
    '-DRAPIDJSON_NOMEMBERITERATORCLASS',
    '-DWIN32_LEAN_AND_MEAN',
    '-Dassimp_EXPORTS',
  ]

  dll_libassimp_ = shared_library(
    'assimp_',
    src_libassimp_,
    include_directories: [inc_assimp_internal],
    dependencies: [zlib_dep] + os_common_deps,
    c_args: assimp_internal_common_args,
    cpp_args: assimp_internal_common_args + [
      '-fvisibility=hidden',
      '-Wno-long-long',
      '-fexceptions',
      '-frtti',
      '-Wno-cast-qual',
    ],
    name_prefix: 'lib',
    install: true,
    install_dir: '.',
    implicit_include_directories: false,
  )

  assimp_target_dep = declare_dependency(
    link_with: dll_libassimp_,
    include_directories: [inc_assimp_public],
    dependencies: [zlib_dep],
    compile_args: [assimp_include_define],
  )
else
  assimp_system_dep = dependency('assimp', required: true)
  assimp_target_dep = declare_dependency(
    dependencies: [assimp_system_dep],
    compile_args: [assimp_include_define],
  )
endif

q3map2_link_args = []
if windows
  q3map2_link_args += ['-Wl,--stack,4194304']
endif

ericw_tools_source_dir = meson.project_source_root() / 'tools/quake2/ericw-tools'
ericw_tools_build_dir = meson.current_build_dir() / 'ericw-tools'
ericw_tools_script = meson.project_source_root() / 'scripts/build-ericw-tools.sh'
ericw_tools_build_type = 'Release'
if build_profile == 'debug'
  ericw_tools_build_type = 'Debug'
elif build_profile == 'extradebug' or build_profile == 'extradebug_quicker' or build_profile == 'profile'
  ericw_tools_build_type = 'RelWithDebInfo'
endif

radiant_link_args = []
if windows
  radiant_link_args += ['-mwindows']
endif

ericw_tools_build_stamp = custom_target(
  'ericw-tools-build',
  output: 'ericw-tools-build.stamp',
  command: [
    sh,
    ericw_tools_script,
    '--mode', 'build',
    '--source', ericw_tools_source_dir,
    '--build-dir', ericw_tools_build_dir,
    '--build-type', ericw_tools_build_type,
    '--stamp', '@OUTPUT@',
  ],
  build_by_default: true,
  build_always_stale: true,
)

dll_modules_archivezip = shared_module(
  'archivezip',
  src_modules_archivezip,
  include_directories: [inc_libs, inc_include],
  dependencies: [zlib_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_archivewad = shared_module(
  'archivewad',
  src_modules_archivewad,
  include_directories: [inc_libs, inc_include],
  dependencies: os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_archivepak = shared_module(
  'archivepak',
  src_modules_archivepak,
  include_directories: [inc_libs, inc_include],
  dependencies: os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_entity = shared_module(
  'entity',
  src_modules_entity,
  include_directories: [inc_libs, inc_include],
  dependencies: [qt_gui_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_image = shared_module(
  'image',
  src_modules_image,
  include_directories: [inc_libs, inc_include],
  dependencies: [jpeg_dep] + os_common_deps,
  link_with: static_deps_modules_image,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_imageq2 = shared_module(
  'imageq2',
  src_modules_imageq2,
  include_directories: [inc_libs, inc_include],
  dependencies: os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_imagehl = shared_module(
  'imagehl',
  src_modules_imagehl,
  include_directories: [inc_libs, inc_include],
  dependencies: os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_imagepng = shared_module(
  'imagepng',
  src_modules_imagepng,
  include_directories: [inc_libs, inc_include],
  dependencies: [png_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_mapq3 = shared_module(
  'mapq3',
  src_modules_mapq3,
  include_directories: [inc_libs, inc_include],
  dependencies: os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_mapxml = shared_module(
  'mapxml',
  src_modules_mapxml,
  include_directories: [inc_libs, inc_include],
  dependencies: [xml_dep, glib_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

assmodel_rpath = ''
if not windows and assimp_internal_enabled
  assmodel_rpath = '$ORIGIN/..'
endif

dll_modules_assmodel = shared_module(
  'assmodel',
  src_modules_assmodel,
  include_directories: [inc_libs, inc_include],
  dependencies: [assimp_target_dep, qt_gui_dep, qt_opengl_dep] + os_common_deps,
  install_rpath: assmodel_rpath,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_shaders = shared_module(
  'shaders',
  src_modules_shaders,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_opengl_dep] + os_common_deps,
  link_with: static_deps_modules_shaders,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_modules_vfspk3 = shared_module(
  'vfspk3',
  src_modules_vfspk3,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep] + os_common_deps,
  link_with: static_deps_modules_vfspk3,
  name_prefix: '',
  install: true,
  install_dir: 'modules',
  implicit_include_directories: false,
)

dll_plugins_bobtoolz = shared_module(
  'bobtoolz',
  src_plugins_bobtoolz,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  link_with: static_deps_plugins_bobtoolz,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_brushexport = shared_module(
  'brushexport',
  src_plugins_brushexport,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_prtview = shared_module(
  'prtview',
  src_plugins_prtview,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_shaderplug = shared_module(
  'shaderplug',
  src_plugins_shaderplug,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep, xml_dep] + os_common_deps,
  link_with: static_deps_plugins_shaderplug,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_sunplug = shared_module(
  'sunplug',
  src_plugins_sunplug,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_ufoaiplug = shared_module(
  'ufoaiplug',
  src_plugins_ufoaiplug,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

dll_plugins_meshtex = shared_module(
  'meshtex',
  src_plugins_meshtex,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  install: true,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

# Not built by default in the legacy "all" target graph.
dll_plugins_bkgrnd2d = shared_module(
  'bkgrnd2d',
  src_plugins_bkgrnd2d,
  include_directories: [inc_libs, inc_include],
  dependencies: [glib_dep, qt_widgets_dep, qt_opengl_dep] + os_common_deps,
  name_prefix: '',
  build_by_default: false,
  install: false,
  install_dir: 'plugins',
  implicit_include_directories: false,
)

exe_q3map2 = executable(
  'q3map2',
  src_q3map2 + q3map2_resources,
  include_directories: [inc_tools_q3_common, inc_libs, inc_include],
  dependencies: [xml_dep, glib_dep, png_dep, jpeg_dep, zlib_dep, assimp_target_dep] + os_common_deps,
  link_with: static_deps_q3map2,
  link_args: q3map2_link_args,
  name_suffix: radiant_exec_suffix,
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

exe_q2map = executable(
  'q2map',
  src_q2map + q2map_resources,
  include_directories: [inc_tools_q2_common, inc_libs, inc_include],
  dependencies: [xml_dep] + os_common_deps,
  link_with: static_deps_q2map,
  c_args: ['-Wno-format-overflow'],
  cpp_args: ['-Wno-format-overflow'],
  name_suffix: radiant_exec_suffix,
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

exe_qdata3 = executable(
  'qdata3',
  src_qdata3 + qdata3_resources,
  include_directories: [inc_tools_q2_common, inc_libs, inc_include],
  dependencies: [xml_dep] + os_common_deps,
  link_with: static_deps_qdata3,
  c_args: ['-Wno-format-overflow'],
  cpp_args: ['-Wno-format-overflow'],
  name_suffix: radiant_exec_suffix,
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

exe_h2data = executable(
  'h2data',
  src_h2data + h2data_resources,
  include_directories: [inc_tools_h2_common, inc_libs, inc_include],
  dependencies: [xml_dep] + os_common_deps,
  link_with: static_deps_h2data,
  c_args: ['-Wno-format-overflow'],
  cpp_args: ['-Wno-format-overflow'],
  name_suffix: radiant_exec_suffix,
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

exe_mbspc = executable(
  'mbspc',
  src_mbspc,
  include_directories: [inc_libs],
  dependencies: os_common_deps,
  c_args: ['-Wstrict-prototypes', '-Wno-format-overflow', '-DNDEBUG', '-DBSPC', '-DBSPCINCLUDE'],
  cpp_args: ['-Wno-format-overflow', '-DNDEBUG', '-DBSPC', '-DBSPCINCLUDE'],
  name_suffix: radiant_exec_suffix,
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

radiant_deps = [
  opengl_dep,
  xml_dep,
  glib_dep,
  qt_widgets_dep,
  qt_svg_dep,
  qt_network_dep,
  qt_multimedia_dep,
  zlib_dep,
  qt_opengl_dep,
  qt_openglwidgets_dep,
] + os_common_deps
if not windows
  radiant_deps += [dl_dep]
endif

exe_radiant = executable(
  'radiant',
  src_radiant + radiant_resources,
  include_directories: [inc_libs, inc_include],
  dependencies: radiant_deps,
  link_with: static_deps_radiant,
  link_args: radiant_link_args,
  name_suffix: radiant_exec_suffix,
  win_subsystem: windows ? 'windows' : 'console',
  install: true,
  install_dir: '.',
  implicit_include_directories: false,
)

alias_target('binaries-tools', [ericw_tools_build_stamp, exe_q2map, exe_qdata3, exe_h2data, exe_q3map2, exe_mbspc])
alias_target('binaries-radiant-modules', [
  dll_modules_archivepak,
  dll_modules_archivewad,
  dll_modules_archivezip,
  dll_modules_entity,
  dll_modules_image,
  dll_modules_imagehl,
  dll_modules_imagepng,
  dll_modules_imageq2,
  dll_modules_mapq3,
  dll_modules_mapxml,
  dll_modules_assmodel,
  dll_modules_shaders,
  dll_modules_vfspk3,
])
alias_target('binaries-radiant-plugins', [
  dll_plugins_bobtoolz,
  dll_plugins_brushexport,
  dll_plugins_prtview,
  dll_plugins_shaderplug,
  dll_plugins_sunplug,
  dll_plugins_ufoaiplug,
  dll_plugins_meshtex,
])
alias_target('binaries-radiant-core', [exe_radiant])
alias_target('binaries-radiant', [
  dll_modules_archivepak,
  dll_modules_archivewad,
  dll_modules_archivezip,
  dll_modules_entity,
  dll_modules_image,
  dll_modules_imagehl,
  dll_modules_imagepng,
  dll_modules_imageq2,
  dll_modules_mapq3,
  dll_modules_mapxml,
  dll_modules_assmodel,
  dll_modules_shaders,
  dll_modules_vfspk3,
  dll_plugins_bobtoolz,
  dll_plugins_brushexport,
  dll_plugins_prtview,
  dll_plugins_shaderplug,
  dll_plugins_sunplug,
  dll_plugins_ufoaiplug,
  dll_plugins_meshtex,
  exe_radiant,
])
alias_target('binaries', [
  ericw_tools_build_stamp,
  exe_q2map,
  exe_qdata3,
  exe_h2data,
  exe_q3map2,
  exe_mbspc,
  dll_modules_archivepak,
  dll_modules_archivewad,
  dll_modules_archivezip,
  dll_modules_entity,
  dll_modules_image,
  dll_modules_imagehl,
  dll_modules_imagepng,
  dll_modules_imageq2,
  dll_modules_mapq3,
  dll_modules_mapxml,
  dll_modules_assmodel,
  dll_modules_shaders,
  dll_modules_vfspk3,
  dll_plugins_bobtoolz,
  dll_plugins_brushexport,
  dll_plugins_prtview,
  dll_plugins_shaderplug,
  dll_plugins_sunplug,
  dll_plugins_ufoaiplug,
  dll_plugins_meshtex,
  exe_radiant,
])

meson.add_install_script(
  sh,
  '-c',
  'printf "%s\\n" "' + radiant_major + '" > "$MESON_INSTALL_DESTDIR_PREFIX/RADIANT_MAJOR"',
  install_tag: 'runtime',
)
meson.add_install_script(
  sh,
  '-c',
  'printf "%s\\n" "' + radiant_minor + '" > "$MESON_INSTALL_DESTDIR_PREFIX/RADIANT_MINOR"',
  install_tag: 'runtime',
)

if not windows
  foreach tool_name : ['radiant', 'q3map2', 'q2map', 'qdata3', 'h2data', 'mbspc']
    install_symlink(
      tool_name,
      pointing_to: tool_name + '.' + radiant_exec_suffix,
      install_dir: '.',
      install_tag: 'runtime',
    )
  endforeach
endif

install_ericw_tools_cmd = [
  'sh "' + ericw_tools_script + '"'
  + ' --mode stage'
  + ' --source "' + ericw_tools_source_dir + '"'
  + ' --build-dir "' + ericw_tools_build_dir + '"'
  + ' --dest "$MESON_INSTALL_DESTDIR_PREFIX/ericw"'
  + ' --build-type "' + ericw_tools_build_type + '"'
  + ' --executable-suffix "' + radiant_exec_suffix + '"',
]
meson.add_install_script(sh, '-c', install_ericw_tools_cmd[0], install_tag: 'runtime')

if get_option('install_data')
  # Runtime expects data roots at <prefix>/gl, <prefix>/bitmaps, etc.
  # Strip the source "tools" directory level so install layout matches legacy.
  install_subdir('setup/data/tools', install_dir: '.', strip_directory: true, install_tag: 'runtime')
  install_subdir('docs', install_dir: 'docs', strip_directory: true, install_tag: 'runtime')

  install_gamepacks_cmd = [
    'cd "' + meson.project_source_root() + '"',
    'DOWNLOAD_GAMEPACKS="' + get_option('download_gamepacks') + '"'
    + ' GAMEPACK_SOURCE="' + get_option('gamepack_source') + '"'
    + ' sh install-gamepacks.sh "$MESON_INSTALL_DESTDIR_PREFIX/gamepacks"',
  ]
  meson.add_install_script(sh, '-c', ' && '.join(install_gamepacks_cmd), install_tag: 'runtime')
endif

if windows and get_option('install_dlls')
  meson.add_install_script(
    sh,
    '-c',
    'cd "' + meson.project_source_root() + '" && INSTALLDIR="$MESON_INSTALL_DESTDIR_PREFIX" sh install-dlls-msys2-mingw.sh',
    install_tag: 'runtime',
  )
endif

summary(
  {
    'Builder': 'Native Meson + Ninja',
    'Build profile': build_profile,
    'Qt major': qt_major,
    'Assimp mode': assimp_internal_enabled ? 'internal (bundled)' : 'system dependency',
    'Install data': get_option('install_data') ? 'yes' : 'no',
    'Install DLLs': get_option('install_dlls') ? 'yes' : 'no',
    'Download gamepacks': get_option('download_gamepacks'),
    'Gamepack source': get_option('gamepack_source'),
    'Version': radiant_version,
  },
  section: 'VibeRadiant',
)
