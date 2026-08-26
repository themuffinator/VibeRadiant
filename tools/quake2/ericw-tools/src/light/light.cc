/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <light/light.hh>

#include <cstdint>
#include <iostream>
#include <fmt/chrono.h>

#include <light/lightgrid.hh>
#include <light/phong.hh>
#include <light/bounce.hh>
#include <light/surflight.hh> //mxd
#include <light/entities.hh>
#include <light/ltface.hh>
#include <light/write.hh> // for facesup_t
#include <light/trace_embree.hh>

#include <common/log.hh>
#include <common/bsputils.hh>
#include <common/numeric_cast.hh>
#include <common/fs.hh>
#include <common/imglib.hh>
#include <common/parallel.hh>
#include <common/ostream.hh>

#if defined(HAVE_EMBREE) && defined(__SSE2__)
#include <xmmintrin.h>
// #include <pmmintrin.h>
#endif

#include <memory>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

#include <common/qvec.hh>
#include <common/json.hh>

#include <light/lightcontext.hh>

bool dirt_in_use = false;

// Context Global
vibey::light::LightContext *g_ctx = nullptr;

namespace
{
std::mutex light_main_mutex;
thread_local bool light_main_active_on_this_thread = false;
} // namespace

// Legacy Proxy Accessors
std::span<lightsurf_t> &LightSurfaces()
{
    return g_ctx->light_surfaces_span;
}
std::vector<lightsurf_t *> &EmissiveLightSurfaces()
{
    return g_ctx->emissive_light_surfaces;
}

lightsurf_t::~lightsurf_t() = default;
const std::unordered_map<int, std::vector<uint8_t>> &UncompressedVis()
{
    return g_ctx->uncompressed_vis;
}

bool IsOutputtingSupplementaryData()
{
    // Use stored vector implicitly via ctx if possible or just check accessor?
    // The previous implementation checked faces_sup.empty()
    return !g_ctx->faces_sup.empty();
}

// Global Definitions (that were extern) - these must seemingly exist but point to nothing?
// Actually, since we removed extern from header, these definitions in .cc are now local
// unless we deleted them. But the goal is to delete them.
// The code below previously defined:
// std::vector<modelinfo_t *> modelinfo;
// etc.
// We remove them.

// Re-implement modelinfo accessors if needed, or update call sites.
// modelinfo_t methods...

float modelinfo_t::getResolvedPhongAngle() const
{
    const float s = phong_angle.value();
    if (s != 0) {
        return s;
    }
    if (phong.value() > 0) {
        return DEFAULT_PHONG_ANGLE;
    }
    return 0;
}

bool modelinfo_t::isWorld() const
{
    // &bsp->dmodels[0] == model;
    // We need access to bsp. 'this' has a bsp pointer usually?
    // checking constructor: modelinfo_t(const mbsp_t *b, ...) : bsp{b}
    // Yes.
    return &bsp->dmodels[0] == model;
}

modelinfo_t::modelinfo_t(const mbsp_t *b, const dmodelh2_t *m, float lmscale)
    : bsp{b},
      model{m},
      lightmapscale{lmscale},
      offset{},
      minlight{this, "minlight", 0},
      maxlight{this, "maxlight", 0},
      minlightMottle{this, {"minlight_mottle", "minlightMottle"}, false},
      shadow{this, "shadow", 0},
      shadowself{this, {"shadowself", "selfshadow"}, 0},
      shadowworldonly{this, "shadowworldonly", 0},
      switchableshadow{this, "switchableshadow", 0},
      switchshadstyle{this, "switchshadstyle", 0},
      dirt{this, "dirt", 0},
      phong{this, "phong", 0},
      phong_angle{this, "phong_angle", 0},
      alpha{this, "alpha", 1.0},
      minlight_color{this, {"minlight_color", "mincolor"}, 255.0, 255.0, 255.0},
      lightignore{this, "lightignore", false},
      lightcolorscale{this, "lightcolorscale", 1},
      object_channel_mask{this, "object_channel_mask", CHANNEL_MASK_DEFAULT},
      surflight_minlight_scale{this, "surflight_minlight_scale", 1.f},
      surflight_atten{this, "surflight_atten", 1.f},
      autominlight{this, "autominlight", false},
      autominlight_target{this, "autominlight_target", ""}
{
}

namespace settings
{
// worldspawn_keys

worldspawn_keys::worldspawn_keys()
    : scaledist{this, "dist", 1.0, 0.0, 100.0, &worldspawn_group},
      rangescale{this, "range", 0.5, 0.0, 100.0, &worldspawn_group},
      global_anglescale{this, {"anglescale", "anglesense"}, 0.5, 0.0, 1.0, &worldspawn_group},
      lightmapgamma{this, "gamma", 1.0, 0.0, 100.0, &worldspawn_group},
      addminlight{this, "addmin", false, &worldspawn_group},
      minlight{this, {"light", "minlight"}, 0, &worldspawn_group},
      minlight_grid{this, {"mingridlight", "minlight_grid"}, 0, &worldspawn_group,
          "override minlight for BSPX lightgrids; inherits minlight when unspecified"},
      minlightMottle{this, {"minlight_mottle", "minlightMottle"}, false, &worldspawn_group},
      maxlight{this, "maxlight", 0, &worldspawn_group},
      minlight_color{this, {"minlight_color", "mincolor"}, 255.0, 255.0, 255.0, &worldspawn_group},
      spotlightautofalloff{this, "spotlightautofalloff", false, &worldspawn_group},
      compilerstyle_start{this, "compilerstyle_start", 32, &worldspawn_group},
      compilerstyle_max{this, "compilerstyle_max", 64, &worldspawn_group},
      dirt{this, {"dirt", "dirty"}, false, &worldspawn_group,
          "apply dirt to all lights (unless they override it) + sunlight + minlight"},
      dirtmode{this, "dirtmode", 0.0f, &worldspawn_group},
      dirtdepth{this, "dirtdepth", 128.0, 1.0, std::numeric_limits<float>::infinity(), &worldspawn_group},
      dirtscale{this, "dirtscale", 1.0, 0.0, 100.0, &worldspawn_group},
      dirtgain{this, "dirtgain", 1.0, 0.0, 100.0, &worldspawn_group},
      dirtangle{this, "dirtangle", 88.0, 1.0, 90.0, &worldspawn_group},
      minlight_dirt{this, "minlight_dirt", false, &worldspawn_group},
      phongallowed{this, "phong", true, &worldspawn_group},
      phongangle{this, "phong_angle", 0, &worldspawn_group},
      bounce{this, "bounce", 0, 0, 100, settings::can_omit_argument_tag(), 1, &worldspawn_group},
      bouncestyled{this, "bouncestyled", false, &worldspawn_group},
      bouncescale{this, "bouncescale", 1.0, 0.0, 100.0, &worldspawn_group},
      bouncecolorscale{this, "bouncecolorscale", 0.0, 0.0, 1.0, &worldspawn_group},
      bouncelightsubdivision{this, "bouncelightsubdivision", 64.0, 1.0, 8192.0, &worldspawn_group},
      surflightscale{this, "surflightscale", 1.0, &worldspawn_group},
      surflightskyscale{this, "surflightskyscale", 1.0, &worldspawn_group},
      surflightskydist{this, "surflightskydist", 0.0, &worldspawn_group},
      surflightsubdivision{this, {"surflightsubdivision", "choplight"}, 16.0, 1.0, 8192.0, &worldspawn_group},
      surflight_minlight_scale{this, "surflight_minlight_scale", 1.0f, 0.f, 510.f, &worldspawn_group},
      surflight_atten{this, "surflight_atten", 1.f, 0.f, std::numeric_limits<float>::max(), &worldspawn_group},
      sunlight{this, {"sunlight", "sun_light"}, 0.0, &worldspawn_group},
      sunlight_color{this, {"sunlight_color", "sun_color"}, 255.0, 255.0, 255.0, &worldspawn_group},
      sun2{this, "sun2", 0.0, &worldspawn_group},
      sun2_color{this, "sun2_color", 255.0, 255.0, 255.0, &worldspawn_group},
      sunlight2{this, "sunlight2", 0.0, &worldspawn_group},
      sunlight2_color{this, {"sunlight2_color", "sunlight_color2"}, 255.0, 255.0, 255.0, &worldspawn_group},
      sunlight3{this, "sunlight3", 0.0, &worldspawn_group},
      sunlight3_color{this, {"sunlight3_color", "sunlight_color3"}, 255.0, 255.0, 255.0, &worldspawn_group},
      sunlight_dirt{this, "sunlight_dirt", 0.0, &worldspawn_group},
      sunlight2_dirt{this, "sunlight2_dirt", 0.0, &worldspawn_group},
      // NOTE: the default mangle needs to be in direction vector form, not euler angle
      sunvec{this, {"sunlight_mangle", "sun_mangle", "sun_angle"}, 0.0, 0.0, -1.0, &worldspawn_group},
      sun2vec{this, "sun2_mangle", 0.0, 0.0, -1.0, &worldspawn_group},
      sun_deviance{this, "sunlight_penumbra", 0.0, 0.0, 180.0, &worldspawn_group},
      sky_surface{this, {"sky_surface", "sun_surface"}, 0, 0, 0, &worldspawn_group},
      surflight_radiosity{this, "surflight_radiosity", SURFLIGHT_Q1, &worldspawn_group,
          "whether to use Q1-style surface subdivision (0) or Q2-style surface radiosity"}
{
}

// light_settings::setting_soft

void light_settings::setting_action::reset()
{
    _source = source::DEFAULT;
}

bool light_settings::setting_action::parse(
    const std::string &setting_name, parser_base_t &parser, source setting_source)
{
    // Match setting_value precedence. Command-line actions are parsed before
    // worldspawn keys, so a later, lower-priority map value must not mutate the
    // already selected output/debug mode.
    if (setting_source < get_source()) {
        return true;
    }

    if (!setting_func::parse(setting_name, parser, setting_source)) {
        return false;
    }

    // The callback owns the effective state; retain only the highest-priority
    // provenance here so the manifest can report how the action was requested.
    change_source(setting_source);
    return true;
}

std::string light_settings::setting_action::string_value() const
{
    return is_changed() ? "1" : "0";
}

std::string light_settings::setting_soft::format() const
{
    return "[n]";
}

// light_settings::setting_extra

bool light_settings::setting_extra::parse(const std::string &setting_name, parser_base_t &parser, source source)
{
    if (setting_name.back() == '4') {
        set_value(4, source);
    } else {
        set_value(2, source);
    }

    return true;
}

std::string light_settings::setting_extra::string_value() const
{
    return std::to_string(_value);
};

std::string light_settings::setting_extra::format() const
{
    return "";
};

void light_settings::CheckNoDebugModeSet()
{
    if (debugmode != debugmodes::none) {
        Error("Only one debug mode is allowed at a time");
    }
}

setting_group worldspawn_group{"Overridable worldspawn keys", 500, expected_source::worldspawn};
setting_group output_group{"Output format options", 30, expected_source::commandline};
setting_group debug_group{"Debug modes", 40, expected_source::commandline};
setting_group postprocessing_group{"Postprocessing options", 50, expected_source::commandline};
setting_group experimental_group{"Experimental options", 60, expected_source::commandline};

light_settings::light_settings()
    : surflight_dump{this, "surflight_dump", false, &debug_group, "dump surface lights to a .map file"},
      surflight_subdivide{
          this, "surflight_subdivide", 128.0, 1.0, 2048.0, &performance_group, "surface light subdivision size"},
      onlyents{this, "onlyents", false, &output_group, "only update entities"},
      write_normals{this, "wrnormals", false, &output_group, "output normals, tangents and bitangents in a BSPX lump"},
      novanilla{this, "novanilla", false, &experimental_group, "implies -bspxlit; don't write vanilla lighting"},
      gate{this, "gate", LIGHT_EQUAL_EPSILON, &performance_group, "cutoff lights at this brightness level"},
      sunsamples{this, "sunsamples", 64, 8, 2048, &performance_group, "set samples for _sunlight2, default 64"},
      arghradcompat{this, "arghradcompat", false, &output_group, "enable compatibility for Arghrad-specific keys"},
      nolighting{this, "nolighting", false, &output_group, "don't output main world lighting (Q2RTX)"},
      debugface{this, "debugface", std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::quiet_NaN(), &debug_group, ""},
      debugvert{this, "debugvert", std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::quiet_NaN(), &debug_group, ""},
      highlightseams{this, "highlightseams", false, &debug_group, ""},
      soft{this, "soft", 0, -1, MAX_SOFT_RADIUS, can_omit_argument_tag(), -1, &postprocessing_group,
          "blurs the lightmap. specify n to blur radius in samples, otherwise auto"},
      radlights{this, "radlights", "\"filename.rad\"", &experimental_group,
          "loads a <surfacename> <r> <g> <b> <intensity> file"},
      lightmap_scale{
          this, "lightmap_scale", 0, &experimental_group, "force change lightmap scale; vanilla engines only allow 16"},
      extra{
          this, {"extra", "extra4"}, 1, &performance_group, "supersampling; 2x2 (extra) or 4x4 (extra4) respectively"},
      super{this, {"super", "supersample"}, 1, 1, 8, &performance_group, "deterministic N x N supersampling (1 to 8)"},
      emissivequality{this, "emissivequality", emissivequality_t::LOW,
          {{"LOW", emissivequality_t::LOW}, {"MEDIUM", emissivequality_t::MEDIUM}, {"HIGH", emissivequality_t::HIGH}},
          &performance_group,
          "low = one point in the center of the face, med = center + all verts, high = spread points out for antialiasing"},
      visapprox{this, "visapprox", visapprox_t::AUTO,
          {{"auto", visapprox_t::AUTO}, {"none", visapprox_t::NONE}, {"vis", visapprox_t::VIS},
              {"rays", visapprox_t::RAYS}},
          &debug_group,
          "change approximate visibility algorithm. auto = choose default based on format. vis = use BSP vis data (slow but precise). rays = use sphere culling with fired rays (fast but may miss faces)"},
      lit{this, "lit",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::lit;
              return true;
          },
          &output_group, "write .lit file"},
      lit2{this, "lit2",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::lit2;
              return true;
          },
          &experimental_group, "write .lit2 file"},
      bspxlit{this, "bspxlit",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::bspx;
              return true;
          },
          &experimental_group, "writes rgb data into the bsp itself"},
      lux{this, "lux",
          [&](const std::string &, parser_base_t &, source) {
              write_luxfile |= luxfile_t::lux;
              return true;
          },
          &experimental_group, "write .lux file"},
      bspxlux{this, "bspxlux",
          [&](const std::string &, parser_base_t &, source) {
              write_luxfile |= luxfile_t::bspx;
              return true;
          },
          &experimental_group, "writes lux data into the bsp itself"},
      bspxonly{this, "bspxonly",
          [&](const std::string &, parser_base_t &, source src) {
              write_litfile |= lightfile_t::bspx;
              write_luxfile |= luxfile_t::bspx;
              novanilla.set_value(true, src);
              return true;
          },
          &experimental_group, "writes both rgb and directions data *only* into the bsp itself"},
      bspx{this, "bspx",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::bspx;
              write_luxfile |= luxfile_t::bspx;
              return true;
          },
          &experimental_group, "writes both rgb and directions data into the bsp itself"},
      hdr{this, "hdr",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::hdr;
              return true;
          },
          &experimental_group, "write .lit file with e5bgr9 data"},
      bspxhdr{this, "bspxhdr",
          [&](const std::string &, parser_base_t &, source) {
              write_litfile |= lightfile_t::bspxhdr;
              return true;
          },
          &experimental_group, "writes e5bgr9 data into the bsp itself"},
      world_units_per_luxel{
          this, "world_units_per_luxel", 0, 0, 1024, &output_group, "enables output of DECOUPLED_LM BSPX lump"},
      force_world_units_per_luxel{this, {"force_world_units_per_luxel", "world_units_per_luxel_force"}, false,
          &output_group, "ignore per-entity lightmap scales and use -world_units_per_luxel globally"},
      embedsettings{this, "embedsettings", false, &output_group,
          "embed a path-sanitized lighting settings manifest in the LIGHTING_SETTINGS BSPX lump"},
      litonly{this, "litonly", false, &output_group, "only write .lit file, don't modify BSP"},
      nolights{this, "nolights", false, &output_group, "ignore light entities (only sunlight/minlight)"},
      facestyles{this, "facestyles", 4, &output_group, "max amount of styles per face; requires BSPX lump if > 4"},
      exportobj{this, "exportobj", false, &output_group, "export an .OBJ for inspection"},
      lmshift{this, "lmshift", 4, &output_group,
          "force a specified lmshift to be applied to the entire map; this is useful if you want to re-light a map with higher quality BSPX lighting without the sources. Will add the LMSHIFT lump to the BSP."},
      lightgrid{this, "lightgrid", false, &experimental_group,
          "generates a lightgrid and writes it to a bspx lump (LIGHTGRID_OCTREE)"},
      lightgrid_dist{this, "lightgrid_dist", 32.f, 32.f, 32.f, &experimental_group,
          "distance between lightgrid sample points, in world units. controls lightgrid size."},
      lightgrid_format{this, "lightgrid_format", lightgrid_format_t::OCTREE,
          {{"octree", lightgrid_format_t::OCTREE}, {"lightgrids", lightgrid_format_t::LIGHTGRIDS}}, &experimental_group,
          "lightgrid BSPX lump to use"},

      dirtdebug{this, {"dirtdebug", "debugdirt"},
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::dirt;
              return true;
          },
          &debug_group, "only save the AO values to the lightmap"},

      bouncedebug{this, "bouncedebug",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::bounce;
              return true;
          },
          &debug_group, "only save bounced lighting to the lightmap"},

      bouncelightsdebug{this, "bouncelightsdebug",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::bouncelights;
              return true;
          },
          &debug_group, "only save bounced emitters lighting to the lightmap"},

      phongdebug{this, "phongdebug",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::phong;
              return true;
          },
          &debug_group, "only save phong normals to the lightmap"},

      phongdebug_obj{this, "phongdebug_obj",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::phong_obj;
              return true;
          },
          &debug_group, "save map as .obj with phonged normals"},

      debugoccluded{this, "debugoccluded",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::debugoccluded;
              return true;
          },
          &debug_group, "save light occlusion data to lightmap"},

      debugneighbours{this, "debugneighbours",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::debugneighbours;
              return true;
          },
          &debug_group, "save neighboring faces data to lightmap (requires -debugface)"},

      debugmottle{this, "debugmottle",
          [&](const std::string &, parser_base_t &, source) {
              CheckNoDebugModeSet();
              debugmode = debugmodes::mottle;
              return true;
          },
          &debug_group, "save mottle pattern to lightmap"},

      debug_lightgrid_octree{
          this, "debug_lightgrid_octree", false, &debug_group, "write .octree.prt file for light grid"}
{
}

void light_settings::set_parameters(int argc, const char **argv)
{
    common_settings::set_parameters(argc, argv);
    program_description = "light compiles lightmap data for BSPs\n\n";
    remainder_name = "mapname.bsp";
}

void light_settings::initialize(int argc, const char **argv)
{
    try {
        common_settings::initialize(argc - 1, argv + 1);

        if (remainder.empty() || remainder.size() > 1) {
            throw parse_exception("expected exactly one BSP input file");
        }

        sourceMap = remainder[0];

        if (super.is_changed() && extra.is_changed()) {
            throw parse_exception("-super/-supersample cannot be combined with -extra or -extra4");
        }
        if (super.is_changed()) {
            extra.set_value(super.value(), super.get_source());
        }

        if ((write_litfile & lightfile_t::lit2) &&
            (write_litfile != lightfile_t::lit2 || write_luxfile != luxfile_t::none || litonly.value() ||
                embedsettings.value())) {
            FError("-lit2 is a standalone output mode and cannot be combined with -lit, -hdr, BSPX/LUX output, or "
                   "-litonly/-embedsettings");
        }

        if (embedsettings.value() && (litonly.value() || debugmode == debugmodes::phong_obj)) {
            FError("-embedsettings cannot be used with a mode that does not write the BSP (-litonly or "
                   "-phongdebug_obj)");
        }
    } catch (parse_exception &ex) {
        print_help(false);
        logging::print("ERROR OCCURRED WHEN TRYING TO PARSE ARGUMENTS:\n");
        logging::print(ex.what());
        logging::print("\n\n");
        throw settings::quit_after_help_exception(1);
    }
}

void light_settings::light_postinitialize(int argc, const char **argv)
{
    if (force_world_units_per_luxel.value() && !world_units_per_luxel.is_changed()) {
        FError("-force_world_units_per_luxel requires -world_units_per_luxel\n");
    }

    if (gate.value() > 1) {
        logging::print("WARNING: -gate value greater than 1 may cause artifacts\n");
    }

    if (radlights.is_changed()) {
        if (!ParseLightsFile(*radlights.values().begin())) {
            logging::print("Unable to read surface lights file {}\n", *radlights.values().begin());
        }
    }

    if (soft.value() == -1) {
        soft.set_value(extra.value() / 2, soft.get_source());
    }

    if (litonly.value()) {
        write_litfile |= lightfile_t::lit;
    }

    if (write_litfile & lightfile_t::lit)
        logging::print(".lit colored light output requested on command line.\n");
    if (write_litfile & lightfile_t::bspx)
        logging::print("BSPX colored light output requested on command line.\n");
    if (write_litfile & lightfile_t::lit2)
        logging::print(".lit (version 2) colored light output requested on command line.\n");
    if (write_litfile & lightfile_t::hdr)
        logging::print(".lit (HDR) light output requested on command line.\n");
    if (write_litfile & lightfile_t::bspxhdr)
        logging::print("BSPX HDR light output requested on command line.\n");
    if (write_luxfile & luxfile_t::lux)
        logging::print(".lux light directions output requested on command line.\n");
    if (write_luxfile & luxfile_t::bspx)
        logging::print("BSPX light directions output requested on command line.\n");

    if (debugmode == debugmodes::dirt) {
        light_options.dirt.set_value(true, settings::source::COMMANDLINE);
    } else if (debugmode == debugmodes::bounce || debugmode == debugmodes::bouncelights) {
        light_options.bounce.set_value(true, settings::source::COMMANDLINE);
    } else if (debugmode == debugmodes::debugneighbours && !debugface.is_changed()) {
        FError("-debugneighbours without -debugface specified\n");
    }

    if (light_options.q2rtx.value()) {
        if (!light_options.nolighting.is_changed()) {
            light_options.nolighting.set_value(true, settings::source::GAME_TARGET);
        }

        if (!light_options.write_normals.is_changed()) {
            light_options.write_normals.set_value(true, settings::source::GAME_TARGET);
        }
    }

    // upgrade to uint16 if facestyles is specified
    if (light_options.facestyles.value() > MAXLIGHTMAPS && !light_options.compilerstyle_max.is_changed()) {
        light_options.compilerstyle_max.set_value(INVALID_LIGHTSTYLE, settings::source::COMMANDLINE);
    }
}

void light_settings::reset()
{
    common_settings::reset();

    sourceMap = fs::path();

    write_litfile = lightfile_t::none;
    write_luxfile = luxfile_t::none;
    debugmode = debugmodes::none;
}
} // namespace settings

settings::light_settings light_options;

namespace
{
static_assert(
    LIGHTING_SETTINGS_BSPX_LUMP.size() < 24, "BSPX lump names must fit in the 24-byte, null-terminated name field");

const char *EmbeddedSettingSourceName(settings::source setting_source)
{
    switch (setting_source) {
        case settings::source::DEFAULT: return "default";
        case settings::source::GAME_TARGET: return "game_target";
        case settings::source::IMPLIED: return "implied";
        case settings::source::MAP: return "map";
        case settings::source::COMMANDLINE: return "command_line";
    }

    FError("unknown light setting source");
}

bool OmitEmbeddedSetting(const settings::setting_base &setting)
{
    // Input/output file names are not registered settings. Omit every
    // filesystem-bearing registered type plus the string-valued log path.
    // setting_set currently covers search paths and external .rad files.
    return setting.primary_name() == "embedsettings" || setting.primary_name() == "logfile" ||
           dynamic_cast<const settings::setting_path *>(&setting) != nullptr ||
           dynamic_cast<const settings::setting_set *>(&setting) != nullptr ||
           dynamic_cast<const settings::setting_redirect *>(&setting) != nullptr;
}
} // namespace

void UpdateEmbeddedLightingSettings(bspdata_t &bspdata, const settings::light_settings &options)
{
    if (!options.embedsettings.value()) {
        return;
    }

    std::vector<const settings::setting_base *> ordered_settings;
    for (const settings::setting_base *setting : options) {
        if (!OmitEmbeddedSetting(*setting)) {
            ordered_settings.push_back(setting);
        }
    }
    std::sort(ordered_settings.begin(), ordered_settings.end(),
        [](const auto *left, const auto *right) { return left->primary_name() < right->primary_name(); });

    Json::Value document(Json::objectValue);
    document["schema"] = "vibeymaptools.light-settings";
    document["schema_version"] = 1;
    document["encoding"] = "UTF-8";
    document["tool"]["name"] = "vmt-light";
    document["tool"]["version"] = VIBEYMAPTOOLS_VERSION;

    Json::Value manifest_settings(Json::arrayValue);
    for (const settings::setting_base *setting : ordered_settings) {
        Json::Value entry(Json::objectValue);
        entry["name"] = setting->primary_name();
        entry["value"] = setting->string_value();
        entry["source"] = EmbeddedSettingSourceName(setting->get_source());
        manifest_settings.append(std::move(entry));
    }
    document["settings"] = std::move(manifest_settings);

    Json::StreamWriterBuilder writer;
    writer["commentStyle"] = "None";
    writer["indentation"] = "  ";
    writer["emitUTF8"] = true;
    std::string json = Json::writeString(writer, document);
    if (json.size() > std::numeric_limits<uint32_t>::max()) {
        FError("embedded lighting settings manifest is too large ({} bytes)", json.size());
    }

    // Constructing the complete payload first leaves any old/unknown lump
    // intact if JSON serialization or allocation fails.
    std::vector<uint8_t> payload(json.begin(), json.end());
    bspdata.bspx.transfer(LIGHTING_SETTINGS_BSPX_LUMP, std::move(payload));
}

void FixupGlobalSettings()
{
    // NOTE: This is confusing.. Setting "dirt" "1" implies "minlight_dirt" "1"
    // (and sunlight_dir/sunlight2_dirt as well), unless those variables were
    // set by the user to "0".
    //
    // We can't just default "minlight_dirt" to "1" because that would enable
    // dirtmapping by default.

    if (light_options.dirt.value()) {
        if (!light_options.minlight_dirt.is_changed()) {
            light_options.minlight_dirt.set_value(true, settings::source::COMMANDLINE);
        }
        if (!light_options.sunlight_dirt.is_changed()) {
            light_options.sunlight_dirt.set_value(1, settings::source::COMMANDLINE);
        }
        if (!light_options.sunlight2_dirt.is_changed()) {
            light_options.sunlight2_dirt.set_value(1, settings::source::COMMANDLINE);
        }
    }
}

const modelinfo_t *ModelInfoForModel(const mbsp_t *bsp, int modelnum)
{
    return g_ctx->modelinfo.at(modelnum);
}

const modelinfo_t *ModelInfoForFace(const mbsp_t *bsp, int facenum)
{
    int i;
    const dmodelh2_t *model;

    /* Find the correct model offset */
    for (i = 0, model = bsp->dmodels.data(); i < bsp->dmodels.size(); i++, model++) {
        if (facenum < model->firstface)
            continue;
        if (facenum < model->firstface + model->numfaces)
            break;
    }
    if (i == bsp->dmodels.size()) {
        return NULL;
    }
    return g_ctx->modelinfo.at(i);
}

const img::texture *Face_Texture(const mbsp_t *bsp, const mface_t *face)
{
    return g_ctx->face_textures[face - bsp->dfaces.data()].image;
}

const qvec3b &Face_LookupTextureColor(const mbsp_t *bsp, const mface_t *face)
{
    return g_ctx->face_textures[face - bsp->dfaces.data()].averageColor;
}

const qvec3f &Face_LookupTextureBounceColor(const mbsp_t *bsp, const mface_t *face)
{
    return g_ctx->face_textures[face - bsp->dfaces.data()].bounceColor;
}

// needs to be done after settings are loaded from worldspawn, because it uses light_options
static void CacheTextures(const mbsp_t &bsp)
{
    auto &face_textures = g_ctx->face_textures;
    auto &extended_texinfo_flags = g_ctx->extended_texinfo_flags;

    face_textures.resize(bsp.dfaces.size());

    for (size_t i = 0; i < bsp.dfaces.size(); i++) {
        const char *name = Face_TextureName(&bsp, &bsp.dfaces[i]);

        if (!name || !*name) {
            face_textures[i] = {nullptr, {127}, {0.5}};
        } else {
            auto tex = img::find(name);
            auto &ext = extended_texinfo_flags[bsp.dfaces[i].texinfo];
            auto avg = ext.surflight_color.value_or(tex->averageColor);
            face_textures[i] = {tex, avg,
                // lerp between gray and the texture color according to `bouncecolorscale` (0 = use gray, 1 = use
                // texture color)
                mix(qvec3f{127}, qvec3f(avg), light_options.bouncecolorscale.value()) / 255.0};
        }
    }
}

static void CreateLightmapSurfaces(mbsp_t *bsp)
{
    g_ctx->light_surfaces = std::make_unique<lightsurf_t[]>(bsp->dfaces.size());
    g_ctx->light_surfaces_span = {g_ctx->light_surfaces.get(), g_ctx->light_surfaces.get() + bsp->dfaces.size()};
    auto &light_surfaces = g_ctx->light_surfaces;
    auto &light_surfaces_span = g_ctx->light_surfaces_span;

    logging::funcheader();
    logging::parallel_for(static_cast<size_t>(0), bsp->dfaces.size(), [&bsp, &light_surfaces](size_t i) {
        auto facesup = g_ctx->faces_sup.empty() ? nullptr : &g_ctx->faces_sup[i];
        // decoupled global commented out in ctx for now, assuming not used or placeholder
        // auto facesup_decoupled = facesup_decoupled_global.empty() ? nullptr : &facesup_decoupled_global[i];
        const bspx_decoupled_lm_perface *facesup_decoupled =
            g_ctx->facesup_decoupled_global.empty() ? nullptr : &g_ctx->facesup_decoupled_global[i];

        auto face = &bsp->dfaces[i];

        /* One extra lightmap is allocated to simplify handling overflow */
        if (!light_options.litonly.value()) {
            // if litonly is set we need to preserve the existing lightofs

            /* some surfaces don't need lightmaps */
            if (facesup) {
                facesup->lightofs = -1;
                for (size_t i = 0; i < MAXLIGHTMAPSSUP; i++) {
                    facesup->styles[i] = INVALID_LIGHTSTYLE;
                }
            } else {
                face->lightofs = -1;
                for (size_t i = 0; i < MAXLIGHTMAPS; i++) {
                    face->styles[i] = INVALID_LIGHTSTYLE_OLD;
                }

                if (facesup_decoupled) {
                    // facesup_decoupled is const pointer here, need to cast or access via non-const?
                    // The original code was using a non-const pointer.
                    // Let's fix the pointer type above.
                }
            }
        }

        // Re-acquiring non-const pointer for modification
        bspx_decoupled_lm_perface *facesup_decoupled_nc =
            g_ctx->facesup_decoupled_global.empty() ? nullptr : &g_ctx->facesup_decoupled_global[i];
        if (!light_options.litonly.value() && facesup_decoupled_nc) {
            facesup_decoupled_nc->offset = -1;
        }

        light_surfaces[i] = CreateLightmapSurface(bsp, face, facesup, facesup_decoupled_nc, light_options);
    });
}

static void UpdateEmissiveLightSurfacesList()
{
    g_ctx->emissive_light_surfaces.clear();
    if (!g_ctx->light_surfaces_span.data()) {
        return;
    }

    for (auto &surf : g_ctx->light_surfaces_span) {
        if (surf.vpl) {
            g_ctx->emissive_light_surfaces.push_back(&surf);
        }
    }
}

static void ClearLightmapSurfaces()
{
    logging::funcheader();
    g_ctx->light_surfaces.reset();
    g_ctx->light_surfaces_span = {};
}

static void FindModelInfo(const mbsp_t *bsp)
{
    auto &modelinfo = g_ctx->modelinfo;
    auto &tracelist = g_ctx->tracelist;
    auto &selfshadowlist = g_ctx->selfshadowlist;
    auto &shadowworldonlylist = g_ctx->shadowworldonlylist;
    auto &switchableshadowlist = g_ctx->switchableshadowlist;

    Q_assert(modelinfo.size() == 0);
    Q_assert(tracelist.size() == 0);
    Q_assert(selfshadowlist.size() == 0);
    Q_assert(shadowworldonlylist.size() == 0);
    Q_assert(switchableshadowlist.size() == 0);

    if (light_options.lightmap_scale.is_changed()) {
        WorldEnt().set("_lightmap_scale", light_options.lightmap_scale.string_value());
    }

    float lightmapscale = WorldEnt().get_int("_lightmap_scale");
    if (!lightmapscale)
        lightmapscale = LMSCALE_DEFAULT; /* the default */
    if (lightmapscale <= 0)
        FError("lightmap scale is 0 or negative\n");
    if (light_options.lightmap_scale.is_changed() || lightmapscale != LMSCALE_DEFAULT)
        logging::print("Forcing lightmap scale of {}qu\n", lightmapscale);
    /*I'm going to do this check in the hopes that there's a benefit to cheaper scaling in engines (especially software
     * ones that might be able to just do some mip hacks). This tool doesn't really care.*/
    {
        int i;
        for (i = 1; i < lightmapscale;) {
            i++;
        }
        if (i != lightmapscale) {
            logging::print("WARNING: lightmap scale is not a power of 2\n");
        }
    }

    /* The world always casts shadows */
    auto world_owner = std::make_unique<modelinfo_t>(bsp, &bsp->dmodels[0], lightmapscale);
    modelinfo_t *world = world_owner.get();
    world->shadow.set_value(1.0f, settings::source::MAP); /* world always casts shadows */
    world->phong_angle.copy_from(light_options.phongangle);
    g_ctx->owned_modelinfo.push_back(std::move(world_owner));
    modelinfo.push_back(world);
    tracelist.push_back(world);

    for (int i = 1; i < bsp->dmodels.size(); i++) {
        auto info_owner = std::make_unique<modelinfo_t>(bsp, &bsp->dmodels[i], lightmapscale);
        modelinfo_t *info = info_owner.get();
        g_ctx->owned_modelinfo.push_back(std::move(info_owner));
        modelinfo.push_back(info);

        /* Find the entity for the model */
        std::string modelname = fmt::format("*{}", i);

        const entdict_t *entdict = FindEntDictWithKeyPair("model", modelname);
        if (entdict == nullptr)
            FError("Couldn't find entity for model {}.\n", modelname);

        // apply settings
        info->set_settings(*entdict, settings::source::MAP);

        // vanilla-compatible switchable shadows
        if (auto *light = LightWithSwitchableShadowTargetValue(entdict->get("targetname"))) {
            // take the "style" key from this light entity and enable switchable shadows on ourself
            info->switchableshadow.set_value(true, settings::source::DEFAULT);
            info->switchshadstyle.set_value(light->style.value(), settings::source::DEFAULT);
        }

        /* Check if this model will cast shadows (shadow => shadowself) */
        if (info->switchableshadow.boolValue()) {
            Q_assert(info->switchshadstyle.value() != 0);
            switchableshadowlist.push_back(info);
        } else if (info->shadow.boolValue()) {
            tracelist.push_back(info);
        } else if (info->shadowself.boolValue()) {
            selfshadowlist.push_back(info);
        } else if (info->shadowworldonly.boolValue()) {
            shadowworldonlylist.push_back(info);
        }

        /* Set up the offset for rotate_* entities */
        entdict->get_vector("origin", info->offset);
    }

    Q_assert(modelinfo.size() == bsp->dmodels.size());
}

/*
 * =============
 *  LightWorld
 * =============
 */
static void LightWorld(bspdata_t *bspdata, const fs::path &source, bool forcedscale)
{
    logging::funcheader();

    // Context is process-global for now, initialized in main.
    // vibey::light::LightContext ctx(&light_options, &std::get<mbsp_t>(bspdata->bsp));
    // g_ctx = &ctx;

    mbsp_t &bsp = std::get<mbsp_t>(bspdata->bsp);

    ClearLightmapSurfaces();

    if (forcedscale) {
        bspdata->bspx.entries.erase("LMSHIFT");
    } else if (light_options.lmshift.is_changed()) {
        // if we forcefully specified an lmshift lump, we have to generate one.
        bspdata->bspx.entries.erase("LMSHIFT");

        std::vector<uint8_t> shifts(bsp.dfaces.size());

        for (auto &shift : shifts) {
            shift = light_options.lmshift.value();
        }

        bspdata->bspx.transfer("LMSHIFT", shifts);
    }

    auto lmshift_lump = bspdata->bspx.entries.find("LMSHIFT");

    if (lmshift_lump == bspdata->bspx.entries.end() && !(light_options.write_litfile & lightfile_t::lit2) &&
        light_options.facestyles.value() <= 4) {
        g_ctx->faces_sup.clear(); // no scales, no lit2
    } else { // we have scales or lit2 output. yay...
        g_ctx->faces_sup.resize(bsp.dfaces.size());

        if (lmshift_lump != bspdata->bspx.entries.end()) {
            for (int i = 0; i < bsp.dfaces.size(); i++) {
                g_ctx->faces_sup[i].lmscale = nth_bit(reinterpret_cast<const char *>(lmshift_lump->second.data())[i]);
            }
        } else {
            for (int i = 0; i < bsp.dfaces.size(); i++) {
                g_ctx->faces_sup[i].lmscale = g_ctx->modelinfo.at(0)->lightmapscale;
            }
        }
    }

    // decoupled lightmaps
    g_ctx->facesup_decoupled_global.clear();
    if (light_options.world_units_per_luxel.is_changed()) {
        g_ctx->facesup_decoupled_global.resize(bsp.dfaces.size());
    }

    CalculateVertexNormals(&bsp);

    // create lightmap surfaces
    CreateLightmapSurfaces(&bsp);

    const bool bouncerequired =
        light_options.bounce.value() &&
        (light_options.debugmode == debugmodes::none || light_options.debugmode == debugmodes::bounce ||
            light_options.debugmode == debugmodes::bouncelights); // mxd

    MakeRadiositySurfaceLights(light_options, &bsp);
    UpdateEmissiveLightSurfacesList();

    logging::header("Direct Lighting"); // mxd
    auto &light_surfaces = g_ctx->light_surfaces_span;

    logging::parallel_for(static_cast<size_t>(0), bsp.dfaces.size(), [&bsp, &light_surfaces](size_t i) {
        if (Face_IsLightmapped(&bsp, &bsp.dfaces[i])) {
#if defined(HAVE_EMBREE) && defined(__SSE2__)
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
            DirectLightFace(&bsp, light_surfaces[i], light_options);
        }
    });

    if (bouncerequired && !light_options.nolighting.value()) {

        for (size_t i = 0; i < light_options.bounce.value(); i++) {

            const bool made_bounce_lights = MakeBounceLights(light_options, &bsp, i);

            // Bounce debugging still needs the direct pass to seed virtual
            // lights, but the direct samples must not leak into its output.
            if (i == 0 && (light_options.debugmode == debugmodes::bounce ||
                              light_options.debugmode == debugmodes::bouncelights)) {
                for (lightsurf_t &surface : light_surfaces) {
                    for (lightmap_t &lightmap : surface.lightmapsByStyle) {
                        std::fill(lightmap.samples.begin(), lightmap.samples.end(), lightsample_t{});
                        lightmap.bounce_color = {};
                    }
                }
            }

            if (!made_bounce_lights) {
                logging::header("No bounces; indirect lighting halted");
                break;
            }
            UpdateEmissiveLightSurfacesList();

            logging::header(fmt::format("Indirect Lighting (pass {0})", i).c_str()); // mxd

            logging::parallel_for(static_cast<size_t>(0), bsp.dfaces.size(), [i, &bsp, &light_surfaces](size_t f) {
                if (Face_IsLightmapped(&bsp, &bsp.dfaces[f])) {
#if defined(HAVE_EMBREE) && defined(__SSE2__)
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

                    IndirectLightFace(&bsp, light_surfaces[f], light_options, i);
                }
            });
        }
    }

    if (!light_options.nolighting.value()) {
        logging::header("Post-Processing"); // mxd
        logging::parallel_for(static_cast<size_t>(0), bsp.dfaces.size(), [&bsp, &light_surfaces](size_t i) {
            if (Face_IsLightmapped(&bsp, &bsp.dfaces[i])) {
#if defined(HAVE_EMBREE) && defined(__SSE2__)
                _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

                PostProcessLightFace(&bsp, light_surfaces[i], light_options);
            }
        });
    }

    // Final save
    SaveLightmapSurfaces(bspdata, source);

    // kill this stuff if its somehow found.
    bspdata->bspx.entries.erase("LMSTYLE16");
    bspdata->bspx.entries.erase("LMSTYLE");
    bspdata->bspx.entries.erase("LMOFFSET");
    bspdata->bspx.entries.erase("DECOUPLED_LM");

    if (!g_ctx->faces_sup.empty()) {
        bool needoffsets = false;
        bool needstyles = false;
        int maxstyle = 0;
        int stylesperface = 0;

        for (int i = 0; i < bsp.dfaces.size(); i++) {
            if (bsp.dfaces[i].lightofs != g_ctx->faces_sup[i].lightofs)
                needoffsets = true;
            int j = 0;
            for (; j < MAXLIGHTMAPSSUP; j++) {
                if (g_ctx->faces_sup[i].styles[j] == INVALID_LIGHTSTYLE)
                    break;
                if (j < MAXLIGHTMAPS && bsp.dfaces[i].styles[j] != g_ctx->faces_sup[i].styles[j]) {
                    needstyles = true;
                }
                if (maxstyle < g_ctx->faces_sup[i].styles[j])
                    maxstyle = g_ctx->faces_sup[i].styles[j];
            }
            if (stylesperface < j)
                stylesperface = j;
        }

        if (stylesperface >= light_options.facestyles.value()) {
            logging::print(
                "WARNING: styles per face {} exceeds compiler-set max styles {}; use `-facestyles` if you need more.\n",
                stylesperface, light_options.facestyles.value());
            stylesperface = light_options.facestyles.value();
        }

        needstyles |= (stylesperface > 4);

        logging::print("max {} styles per face, {} used{}\n", light_options.facestyles.value(), stylesperface,
            maxstyle >= INVALID_LIGHTSTYLE_OLD ? ", 16bit lightstyles" : "");

        if (needstyles) {
            if (maxstyle >= INVALID_LIGHTSTYLE_OLD) {
                /*needs bigger datatype*/
                std::vector<uint8_t> styles_mem(sizeof(uint16_t) * stylesperface * bsp.dfaces.size());

                omemstream styles(styles_mem.data(), styles_mem.size(), std::ios_base::out | std::ios_base::binary);
                styles << endianness<std::endian::little>;

                for (size_t i = 0; i < bsp.dfaces.size(); i++) {
                    for (size_t j = 0; j < stylesperface; j++) {
                        styles <= g_ctx->faces_sup[i].styles[j];
                    }
                }

                logging::print("LMSTYLE16 BSPX lump written\n");
                bspdata->bspx.transfer("LMSTYLE16", styles_mem);
            } else {
                /*original LMSTYLE lump was just for different lmshift info*/
                std::vector<uint8_t> styles_mem(stylesperface * bsp.dfaces.size());

                for (size_t i = 0, k = 0; i < bsp.dfaces.size(); i++) {
                    for (size_t j = 0; j < stylesperface; j++, k++) {
                        styles_mem[k] = g_ctx->faces_sup[i].styles[j] == INVALID_LIGHTSTYLE
                                            ? INVALID_LIGHTSTYLE_OLD
                                            : g_ctx->faces_sup[i].styles[j];
                    }
                }

                logging::print("LMSTYLE BSPX lump written\n");
                bspdata->bspx.transfer("LMSTYLE", styles_mem);
            }
        }

        if (needoffsets) {
            std::vector<uint8_t> offsets_mem(bsp.dfaces.size() * sizeof(int32_t));

            omemstream offsets(offsets_mem.data(), offsets_mem.size(), std::ios_base::out | std::ios_base::binary);
            offsets << endianness<std::endian::little>;

            for (size_t i = 0; i < bsp.dfaces.size(); i++) {
                offsets <= g_ctx->faces_sup[i].lightofs;
            }

            logging::print("LMOFFSET BSPX lump written\n");
            bspdata->bspx.transfer("LMOFFSET", offsets_mem);
        }
    }

    if (!g_ctx->facesup_decoupled_global.empty()) {
        std::vector<uint8_t> mem(sizeof(bspx_decoupled_lm_perface) * bsp.dfaces.size());

        omemstream stream(mem.data(), mem.size(), std::ios_base::out | std::ios_base::binary);
        stream << endianness<std::endian::little>;

        for (size_t i = 0; i < bsp.dfaces.size(); i++) {
            stream <= g_ctx->facesup_decoupled_global[i];
        }

        logging::print("DECOUPLED_LM BSPX lump written\n");
        bspdata->bspx.transfer("DECOUPLED_LM", mem);
    }
}

// obj

static size_t ObjFaceVertexIndex(const mbsp_t &bsp, const mface_t &face, size_t edge_offset, size_t facenum)
{
    const int32_t signed_edge = bsp.dsurfedges[static_cast<size_t>(face.firstedge) + edge_offset];
    if (signed_edge == std::numeric_limits<int32_t>::min()) {
        FError("can't export OBJ: face {} references an invalid edge", facenum);
    }
    const size_t edge_index =
        static_cast<size_t>(signed_edge < 0 ? -static_cast<int64_t>(signed_edge) : static_cast<int64_t>(signed_edge));
    if (edge_index >= bsp.dedges.size()) {
        FError("can't export OBJ: face {} references out-of-range edge {}", facenum, edge_index);
    }
    const size_t vertex_index = bsp.dedges[edge_index][signed_edge < 0 ? 1 : 0];
    if (vertex_index >= bsp.dvertexes.size()) {
        FError("can't export OBJ: edge {} references out-of-range vertex {}", edge_index, vertex_index);
    }
    return vertex_index;
}

static void ValidateObjFace(const mbsp_t &bsp, const mface_t &face, size_t facenum)
{
    if (face.firstedge < 0 || face.numedges < 3 || static_cast<size_t>(face.firstedge) > bsp.dsurfedges.size() ||
        static_cast<size_t>(face.numedges) > bsp.dsurfedges.size() - static_cast<size_t>(face.firstedge)) {
        FError("can't export OBJ: face {} has an invalid surface-edge range", facenum);
    }

    for (size_t edge_offset = 0; edge_offset < static_cast<size_t>(face.numedges); ++edge_offset) {
        const size_t vertex_index = ObjFaceVertexIndex(bsp, face, edge_offset, facenum);
        const qvec3f &position = bsp.dvertexes[vertex_index];
        const qvec3f &normal = GetSurfaceVertexNormal(&bsp, &face, static_cast<int>(edge_offset)).normal;
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(position[axis]) || !std::isfinite(normal[axis])) {
                FError("can't export OBJ: face {} has non-finite vertex data", facenum);
            }
        }
    }
}

static void ExportObjFace(std::ofstream &f, const mbsp_t *bsp, const mface_t *face, size_t facenum, size_t *vertcount)
{
    const size_t edge_count = static_cast<size_t>(face->numedges);
    if (*vertcount > std::numeric_limits<size_t>::max() - edge_count) {
        FError("can't export OBJ: vertex count overflows this platform");
    }

    // export the vertices and uvs
    for (size_t i = 0; i < edge_count; i++) {
        const size_t vertnum = ObjFaceVertexIndex(*bsp, *face, i, facenum);
        const qvec3f normal = GetSurfaceVertexNormal(bsp, face, static_cast<int>(i)).normal;
        const qvec3f &pos = bsp->dvertexes[vertnum];
        ewt::print(f, "v {:.9} {:.9} {:.9}\n", pos[0], pos[1], pos[2]);
        ewt::print(f, "vn {:.9} {:.9} {:.9}\n", normal[0], normal[1], normal[2]);
    }

    f << "f";
    for (size_t i = 0; i < edge_count; i++) {
        // .obj vertexes start from 1
        // .obj faces are CCW, quake is CW, so reverse the order
        const size_t vertindex = *vertcount + (edge_count - 1 - i) + 1;
        ewt::print(f, " {}//{}", vertindex, vertindex);
    }
    f << '\n';

    *vertcount += edge_count;
}

static void ExportObj(const fs::path &filename, const mbsp_t *bsp)
{
    if (!bsp) {
        FError("can't export OBJ for a null BSP");
    }
    if (bsp->dmodels.empty()) {
        FError("can't export OBJ: BSP has no world model");
    }

    const dmodelh2_t &world = bsp->dmodels[0];
    if (world.firstface < 0 || world.numfaces < 0 || static_cast<size_t>(world.firstface) > bsp->dfaces.size() ||
        static_cast<size_t>(world.numfaces) > bsp->dfaces.size() - static_cast<size_t>(world.firstface)) {
        FError("can't export OBJ: world model has an invalid face range");
    }
    const size_t start = static_cast<size_t>(world.firstface);
    const size_t end = start + static_cast<size_t>(world.numfaces);

    // Validate the complete export before truncating an existing destination.
    for (size_t i = start; i < end; ++i) {
        ValidateObjFace(*bsp, bsp->dfaces[i], i);
    }

    std::ofstream objfile(filename);
    if (!objfile) {
        FError("couldn't open {} for writing", filename);
    }
    size_t vertcount = 0;

    for (size_t i = start; i < end; i++) {
        ExportObjFace(objfile, bsp, &bsp->dfaces[i], i, &vertcount);
    }

    objfile.flush();
    if (!objfile) {
        objfile.close();
        FError("error writing {}", filename);
    }
    objfile.close();
    if (!objfile) {
        FError("error closing {}", filename);
    }

    logging::print("Wrote {}\n", filename);
}

// returns the face with a centroid nearest the given point.
static const mface_t *Face_NearestCentroid(const mbsp_t *bsp, const qvec3f &point)
{
    const mface_t *nearest_face = NULL;
    float nearest_dist = FLT_MAX;

    for (int i = 0; i < bsp->dfaces.size(); i++) {
        const mface_t *f = BSP_GetFace(bsp, i);

        const qvec3f fc = Face_Centroid(bsp, f);

        const qvec3f distvec = fc - point;
        const float dist = qv::length(distvec);

        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest_face = f;
        }
    }

    return nearest_face;
}

static void FindDebugFace(const mbsp_t *bsp)
{
    if (!light_options.debugface.is_changed())
        return;

    const mface_t *f = Face_NearestCentroid(bsp, light_options.debugface.value());
    if (f == NULL)
        FError("f == NULL\n");

    const int facenum = f - bsp->dfaces.data();

    g_ctx->dump_facenum = facenum;

    const modelinfo_t *mi = ModelInfoForFace(bsp, facenum);
    const int modelnum = mi ? (mi->model - bsp->dmodels.data()) : -1;

    const char *texname = Face_TextureName(bsp, f);
    logging::funcprint("dumping face {} (texture '{}' model {})\n", facenum, texname, modelnum);
}

// returns the vert nearest the given point
static int Vertex_NearestPoint(const mbsp_t *bsp, const qvec3f &point)
{
    int nearest_vert = -1;
    float nearest_dist = std::numeric_limits<float>::infinity();

    for (int i = 0; i < bsp->dvertexes.size(); i++) {
        const qvec3f &vertex = bsp->dvertexes[i];

        float dist = qv::distance(vertex, point);

        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest_vert = i;
        }
    }

    return nearest_vert;
}

static void FindDebugVert(const mbsp_t *bsp)
{
    if (!light_options.debugvert.is_changed())
        return;

    int v = Vertex_NearestPoint(bsp, light_options.debugvert.value());

    logging::funcprint("dumping vert {} at {}\n", v, bsp->dvertexes[v]);

    g_ctx->dump_vertnum = v;
}

void SetLitNeeded(const bspdata_t &bspdata)
{
    if (bspdata.loadversion->game->has_rgb_lightmap) {
        return;
    }

    if (!light_options.write_litfile) {
        if (light_options.novanilla.value()) {
            light_options.write_litfile = lightfile_t::bspx;
            logging::print("Colored light entities/settings detected: "
                           "bspxlit output enabled.\n");
        } else {
            light_options.write_litfile = lightfile_t::lit;
            logging::print("Colored light entities/settings detected: "
                           ".lit output enabled.\n");
        }
    }
}

#if 0
static void PrintLight(const light_t &light)
{
    bool first = true;

    auto settings = const_cast<light_t &>(light).settings();
    for (const auto &setting : settings.allSettings()) {
        if (!setting->isChanged())
            continue; // don't spam default values

        // print separator
        if (!first) {
            logging::print("; ");
        } else {
            first = false;
        }

        logging::print("{}={}", setting->primaryName(), setting->stringValue());
    }
    logging::print("\n");
}

static void PrintLights()
{
    logging::print("===PrintLights===\n");

    for (const auto &light : GetLights()) {
        PrintLight(light);
    }
}
#endif

static inline void WriteNormals(const mbsp_t &bsp, bspdata_t &bspdata)
{
    std::set<qvec3f> unique_normals;
    size_t num_normals = 0;

    for (auto &face : bsp.dfaces) {
        auto &cache = FaceCacheForFNum(&face - bsp.dfaces.data());
        for (auto &normals : cache.normals()) {
            unique_normals.insert(qv::Snap(normals.normal));
            unique_normals.insert(qv::Snap(normals.tangent));
            unique_normals.insert(qv::Snap(normals.bitangent));
            num_normals += 3;
        }
    }

    size_t data_size = sizeof(uint32_t) + (sizeof(qvec3f) * unique_normals.size()) + (sizeof(uint32_t) * num_normals);
    std::vector<uint8_t> data(data_size);
    omemstream stream(data.data(), data_size);

    stream << endianness<std::endian::little>;
    stream <= numeric_cast<uint32_t>(unique_normals.size());

    std::map<qvec3f, size_t> mapped_normals;

    for (auto &n : unique_normals) {
        stream <= std::tie(n[0], n[1], n[2]);
        mapped_normals.emplace(n, mapped_normals.size());
    }

    for (auto &face : bsp.dfaces) {
        auto &cache = FaceCacheForFNum(&face - bsp.dfaces.data());

        for (auto &n : cache.normals()) {
            stream <= numeric_cast<uint32_t>(mapped_normals[qv::Snap(n.normal)]);
            stream <= numeric_cast<uint32_t>(mapped_normals[qv::Snap(n.tangent)]);
            stream <= numeric_cast<uint32_t>(mapped_normals[qv::Snap(n.bitangent)]);
        }
    }

    Q_assert(stream.tellp() == data_size);

    logging::print(logging::flag::VERBOSE, "Compressed {} normals down to {}\n", num_normals, unique_normals.size());

    bspdata.bspx.transfer("FACENORMALS", data);
}

/**
 * Resets globals in this file
 */
static void ResetLight()
{
    dirt_in_use = false;
    if (g_ctx) {
        ClearLightmapSurfaces();
        g_ctx->faces_sup.clear();
        g_ctx->facesup_decoupled_global.clear();
        g_ctx->uncompressed_vis.clear(); // uncompressed_vis is unordered_map
        g_ctx->modelinfo.clear();
        g_ctx->tracelist.clear();
        g_ctx->selfshadowlist.clear();
        g_ctx->shadowworldonlylist.clear();
        g_ctx->switchableshadowlist.clear();
        g_ctx->owned_modelinfo.clear();
        g_ctx->extended_texinfo_flags.clear();
        g_ctx->extended_content_flags.clear();
        g_ctx->dump_facenum = -1;
        g_ctx->dump_vertnum = -1;
    }
}

void light_reset()
{
    ResetLightEntities();
    ResetLight();
    ResetLtFace();
    ResetPhong();
    ResetSurflight();
    ResetEmbree();

    light_options.reset();
}

/*
 * ==================
 * main
 * light modelfile
 * ==================
 */
static int light_main_impl(int argc, const char **argv)
{
    // light's remaining process-global state cannot safely serve two callers. A
    // thread-local sentinel catches recursion before try_locking a mutex already
    // owned by this thread, which is undefined for std::mutex.
    if (light_main_active_on_this_thread) {
        FError("light compiler is already active; concurrent or recursive invocation is not supported");
    }

    std::unique_lock<std::mutex> invocation_lock(light_main_mutex, std::try_to_lock);
    if (!invocation_lock.owns_lock()) {
        FError("light compiler is already active; concurrent or recursive invocation is not supported");
    }

    light_main_active_on_this_thread = true;
    struct invocation_state_reset_t
    {
        ~invocation_state_reset_t() { light_main_active_on_this_thread = false; }
    } invocation_state_reset;

    // This should be impossible while holding the invocation lock; retain the
    // check to diagnose a stale context left by code outside light_main.
    if (g_ctx) {
        FError("light compiler context was already initialized before invocation");
    }
    light_reset();

    bspdata_t bspdata;

    light_options.preinitialize(argc, argv);
    light_options.initialize(argc, argv);
    light_options.postinitialize(argc, argv);

    auto start = I_FloatTime();
    fs::path source = light_options.sourceMap;

    logging::init(
        fs::path(source).replace_filename(source.stem().string() + "-light").replace_extension("log"), light_options);

    source.replace_extension("rad");
    if (source != "lights.rad")
        ParseLightsFile("lights.rad"); // generic/default name
    ParseLightsFile(source); // map-specific file name

    source.replace_extension("bsp");
    LoadBSPFile(source, &bspdata);

    bspdata.version->game->init_filesystem(source, light_options);

    if (!ConvertBSPFormat(&bspdata, &bspver_generic)) {
        FError("couldn't convert {} to the generic BSP representation", source);
    }

    mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);
    if (bsp.dmodels.empty()) {
        FError("BSP contains no models");
    }

    // mxd. Use 1.0 rangescale as a default to better match with qrad3/arghrad
    if (bspdata.loadversion->game->id == GAME_QUAKE_II) {
        if (!light_options.rangescale.is_changed()) {
            light_options.rangescale.set_value(1.0, settings::source::GAME_TARGET);
        }
        if (!light_options.bouncecolorscale.is_changed()) {
            light_options.bouncecolorscale.set_value(0.5, settings::source::GAME_TARGET);
        }
        if (!light_options.surflightscale.is_changed()) {
            light_options.surflightscale.set_value(0.65f, settings::source::GAME_TARGET);
        }
        if (!light_options.surflightskyscale.is_changed()) {
            light_options.surflightskyscale.set_value(0.65f, settings::source::GAME_TARGET);
        }
        if (!light_options.bouncescale.is_changed()) {
            light_options.bouncescale.set_value(0.85f, settings::source::GAME_TARGET);
        }
        if (!light_options.bounce.is_changed()) {
            light_options.bounce.set_value(true, settings::source::GAME_TARGET);
        }
        if (!light_options.surflight_radiosity.is_changed()) {
            light_options.surflight_radiosity.set_value(SURFLIGHT_RAD, settings::source::GAME_TARGET);
        }
        if (!light_options.bouncestyled.is_changed()) {
            light_options.bouncestyled.set_value(true, settings::source::GAME_TARGET);
        }
    }

    // check vis approx type
    if (light_options.visapprox.value() == visapprox_t::AUTO) {
        if (!bsp.dvis.bits.empty()) {
            light_options.visapprox.set_value(visapprox_t::VIS, settings::source::DEFAULT);
        } else {
            light_options.visapprox.set_value(visapprox_t::RAYS, settings::source::DEFAULT);
        }
    }

    img::load_textures(&bsp, light_options);

    // Create Context
    // Note: We use the pointer to bsp inside bspdata.
    // WARNING: ConvertBSPFormat might re-allocate if it changes variant?
    // But we are past that.
    vibey::light::LightContext ctx(&light_options, &bsp);
    g_ctx = &ctx;
    struct context_reset_t
    {
        vibey::light::LightContext *context;
        ~context_reset_t()
        {
            if (g_ctx == context) {
                g_ctx = nullptr;
            }
        }
    } context_reset{&ctx};

    g_ctx->extended_texinfo_flags = LoadExtendedTexinfoFlags(source, &bsp);
    g_ctx->extended_content_flags = LoadExtendedContentFlags(source, &bsp);

    LoadEntities(light_options, &bsp);

    CacheTextures(bsp);

    light_options.light_postinitialize(argc, argv);
    light_options.print_summary();

    g_ctx->uncompressed_vis = DecompressAllVis(&bsp, true);
    // FindModelInfo now uses g_ctx
    FindModelInfo(&bsp);

    FindDebugFace(&bsp);
    FindDebugVert(&bsp);

    Embree_TraceInit(&bsp);

    if (light_options.debugmode == debugmodes::phong_obj) {
        CalculateVertexNormals(&bsp);
        source.replace_extension("obj");
        ExportObj(source, &bsp);

        logging::close();
        return 0;
    }

    SetupLights(light_options, &bsp);

    // PrintLights();

    if (!light_options.onlyents.value()) {
        SetupDirt(light_options);

        LightWorld(&bspdata, source, light_options.lightmap_scale.is_changed());

        LightGrid(&bspdata);

        ClearLightmapSurfaces();

        // invalidate normals
        bspdata.bspx.entries.erase("FACENORMALS");

        if (light_options.write_normals.value()) {
            WriteNormals(bsp, bspdata);
        }

        if (light_options.write_litfile & lightfile_t::lit2) {
            logging::close();
            return 0; // run away before any files are written
        }
    }

    /* -novanilla + internal lighting = no grey lightmap */
    if (light_options.novanilla.value() && (light_options.write_litfile & lightfile_t::bspx)) {
        bsp.dlightdata.clear();
    }

    if (light_options.exportobj.value()) {
        ExportObj(fs::path{source}.replace_extension(".obj"), &bsp);
    }

    const bool writes_external_lit =
        (light_options.write_litfile & lightfile_t::hdr) || (light_options.write_litfile & lightfile_t::lit2) ||
        ((light_options.write_litfile & lightfile_t::lit) && !bsp.loadversion->game->has_rgb_lightmap);

    WriteEntitiesToString(light_options, &bsp);
    UpdateEmbeddedLightingSettings(bspdata, light_options);
    /* Convert data format back if necessary */
    if (!ConvertBSPFormat(&bspdata, bspdata.loadversion)) {
        FError("couldn't convert {} back to {}", source, bspdata.loadversion->short_name);
    }

    if (!light_options.litonly.value()) {
        WriteBSPFile(source, &bspdata);
    }

    // A successful run with no replacement external lighting file makes any
    // previous .lit stale. Defer removal until all processing and BSP output
    // have succeeded so a failed compile never destroys the last valid file.
    if (!light_options.onlyents.value() && !writes_external_lit) {
        fs::path stale_lit = source;
        stale_lit.replace_extension("lit");
        std::error_code error;
        fs::remove(stale_lit, error);
        if (error) {
            FError("couldn't remove stale {}: {}", stale_lit, error.message());
        }
    }

    auto end = I_FloatTime();
    logging::print("{:.3} seconds elapsed\n", (end - start));
#if 0
    logging::print("\n");
    logging::print("stats:\n");
    logging::print("{} lights tested, {} hits per sample point\n",
        static_cast<float>(total_light_rays) / static_cast<float>(total_samplepoints),
        static_cast<float>(total_light_ray_hits) / static_cast<float>(total_samplepoints));
    logging::print("{} surface lights tested, {} hits per sample point\n",
        static_cast<float>(total_surflight_rays) / static_cast<float>(total_samplepoints),
        static_cast<float>(total_surflight_ray_hits) / static_cast<float>(total_samplepoints)); // mxd
    logging::print("{} bounce lights tested, {} hits per sample point\n",
        static_cast<float>(total_bounce_rays) / static_cast<float>(total_samplepoints),
        static_cast<float>(total_bounce_ray_hits) / static_cast<float>(total_samplepoints));
#endif
    logging::print("{} empty lightmaps\n", static_cast<int>(fully_transparent_lightmaps));
    logging::close();

    return 0;
}

int light_main(int argc, const char **argv)
{
    const int result = light_main_impl(argc, argv);
    if (result == 0) {
        // The implementation's invocation lock, context, workers, and local
        // diagnostic objects are all gone before warning promotion can throw.
        logging::fail_if_warnings();
    }
    return result;
}

int light_main(const std::vector<std::string> &args)
{
    std::vector<const char *> argPtrs;
    for (const std::string &arg : args) {
        argPtrs.push_back(arg.data());
    }

    return light_main(argPtrs.size(), argPtrs.data());
}
