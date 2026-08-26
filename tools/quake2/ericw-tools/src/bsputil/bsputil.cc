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

#include <bsputil/bsputil.hh>

#include <cstdint>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "common/imglib.hh"

#include <common/cmdlib.hh>
#include <common/bspfile.hh>
#include <common/bsputils.hh>
#include <common/decompile.hh>
#include <common/mathlib.hh>
#include <common/fs.hh>
#include <common/settings.hh>
#include <common/ostream.hh>

#include <set>
#include <algorithm> // std::sort
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <fmt/ostream.h>

// bsputil_settings

bool bsputil_settings::load_setting(const std::string &name, settings::source src)
{
    auto setting = std::make_unique<settings::setting_func>(nullptr, name, nullptr);
    operations.push_back(std::move(setting));
    return true;
}

bsputil_settings::bsputil_settings()
    : scale{this, "scale",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_vec3>(name, parser, src, 0.f, 0.f, 0.f);
          },
          nullptr, "Scale the BSP by the given scalar vectors (can be negative, too)"},
      replace_entities{this, "replace-entities",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_string>(name, parser, src, "");
          },
          nullptr, "Replace BSP entities with the given files' contents"},
      extract_entities{this, "extract-entities",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_bool>(name, parser, src, "");
          },
          nullptr, "Extract BSP entities to the given file name"},
      extract_textures{this, "extract-textures",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_bool>(name, parser, src, "");
          },
          nullptr, "Extract BSP texutres to the given wad file"},
      replace_textures{this, "replace-textures",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_string>(name, parser, src, "");
          },
          nullptr, "Replace BSP textures with the given wads' contents"},
      convert{this, "convert",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_string>(name, parser, src, "");
          },
          nullptr, "Convert the BSP file to a different BSP format"},
      check{this, "check",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting(name, src);
          },
          nullptr, "Check/verify BSP data"},
      modelinfo{this, "modelinfo",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting(name, src);
          },
          nullptr, "Print model info"},
      findfaces{this, "findfaces",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              auto pos = std::make_shared<settings::setting_vec3>(nullptr, name, 0.f, 0.f, 0.f);
              if (bool parsed = pos->parse(name, parser, src); !parsed)
                  return false;
              auto norm = std::make_shared<settings::setting_vec3>(nullptr, name, 0.f, 0.f, 0.f);
              if (bool parsed = norm->parse(name, parser, src); !parsed)
                  return false;
              operations.push_back(std::make_unique<setting_combined>(
                  nullptr, name, std::initializer_list<std::shared_ptr<settings::setting_base>>{pos, norm}));
              return true;
          },
          nullptr, "Find faces with specified pos/normal"},
      findleaf{this, "findleaf",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_vec3>(name, parser, src, 0.f, 0.f, 0.f);
          },
          nullptr, "Find closest leaf"},
      settexinfo{this, "settexinfo",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              auto faceNum = std::make_shared<settings::setting_int32>(nullptr, name, 0);
              if (bool parsed = faceNum->parse(name, parser, src); !parsed)
                  return false;
              auto texInfoNum = std::make_shared<settings::setting_int32>(nullptr, name, 0);
              if (bool parsed = texInfoNum->parse(name, parser, src); !parsed)
                  return false;
              operations.push_back(std::make_unique<setting_combined>(
                  nullptr, name, std::initializer_list<std::shared_ptr<settings::setting_base>>{faceNum, texInfoNum}));
              return true;
          },
          nullptr, "Set texinfo"},
      decompile{this, "decompile",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting(name, src);
          },
          nullptr, "Decompile to the given .map file"},
      decompile_geomonly{this, "decompile-geomonly",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting(name, src);
          },
          nullptr, "Decompile"},
      decompile_ignore_brushes{this, "decompile-ignore-brushes",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting(name, src);
          },
          nullptr, "Decompile entities only"},
      decompile_hull{this, "decompile-hull",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_int32>(name, parser, src, 0);
          },
          nullptr, "Decompile specific hull"},
      extract_bspx_lump{this, "extract-bspx-lump",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              auto lump = std::make_shared<settings::setting_string>(nullptr, name, "");
              if (bool parsed = lump->parse(name, parser, src); !parsed)
                  return false;
              auto output = std::make_shared<settings::setting_string>(nullptr, name, "");
              if (bool parsed = output->parse(name, parser, src); !parsed)
                  return false;
              operations.push_back(std::make_unique<setting_combined>(
                  nullptr, name, std::initializer_list<std::shared_ptr<settings::setting_base>>{lump, output}));
              return true;
          },
          nullptr, "Extract a BSPX lump"},
      insert_bspx_lump{this, "insert-bspx-lump",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              auto lump = std::make_shared<settings::setting_string>(nullptr, name, "");
              if (bool parsed = lump->parse(name, parser, src); !parsed)
                  return false;
              auto input = std::make_shared<settings::setting_string>(nullptr, name, "");
              if (bool parsed = input->parse(name, parser, src); !parsed)
                  return false;
              operations.push_back(std::make_unique<setting_combined>(
                  nullptr, name, std::initializer_list<std::shared_ptr<settings::setting_base>>{lump, input}));
              return true;
          },
          nullptr, "Insert a BSPX lump"},
      remove_bspx_lump{this, "remove-bspx-lump",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_string>(name, parser, src, "");
          },
          nullptr, "Remove a BSPX lump"},
      svg{this, "svg",
          [&](const std::string &name, parser_base_t &parser, settings::source src) {
              return this->load_setting<settings::setting_bool>(name, parser, src, false);
          },
          nullptr, "Create an SVG view of the input BSP"}
{
}

void bsputil_settings::reset()
{
    settings::common_settings::reset();
    operations.clear();
}

bsputil_settings bsputil_options;

/* FIXME - share header with qbsp, etc. */
struct wadinfo_t
{
    std::array<char, 4> identification = {'W', 'A', 'D', '2'}; // should be WAD2
    int32_t numlumps;
    int32_t infotableofs = sizeof(wadinfo_t);

    auto stream_data() { return std::tie(identification, numlumps, infotableofs); }
};

struct lumpinfo_t
{
    int32_t filepos;
    int32_t disksize;
    int32_t size; // uncompressed
    char type;
    char compression;
    char pad1, pad2;
    std::array<char, 16> name; // fixed-width; 16-byte names need not be null terminated

    auto stream_data() { return std::tie(filepos, disksize, size, type, compression, pad1, pad2, name); }
};

void ExportWad(std::ofstream &wadfile, const mbsp_t *bsp)
{
    const auto &texdata = bsp->dtex;

    /* Count up the valid lumps */
    size_t numvalid = 0;
    for (auto &texture : texdata.textures) {
        if (texture.data.size() > sizeof(dmiptex_t)) {
            numvalid++;
        }
    }

    if (numvalid > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        FError("too many textures to export to WAD");
    }

    // Write out
    wadinfo_t header;
    header.numlumps = static_cast<int32_t>(numvalid);
    wadfile <= header;

    lumpinfo_t lump{};
    lump.type = 'D';

    /* Miptex data will follow the lump headers */
    if (numvalid > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - sizeof(header)) / sizeof(lump)) {
        FError("WAD directory is too large");
    }
    size_t filepos = sizeof(header) + numvalid * sizeof(lump);
    for (auto &miptex : texdata.textures) {
        if (miptex.data.size() <= sizeof(dmiptex_t))
            continue;

        if (filepos > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            miptex.data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            miptex.data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) - filepos) {
            FError("texture data is too large to export to WAD");
        }

        lump.filepos = static_cast<int32_t>(filepos);
        lump.size = static_cast<int32_t>(miptex.data.size());
        lump.disksize = lump.size;
        lump.name = {};
        if (miptex.name.size() > lump.name.size()) {
            logging::print(
                "WARNING: truncating texture name '{}' to {} bytes for WAD export\n", miptex.name, lump.name.size());
        }
        std::copy_n(miptex.name.begin(), std::min(miptex.name.size(), lump.name.size()), lump.name.begin());

        filepos += lump.disksize;

        // Write it out
        wadfile <= lump;
    }
    for (auto &miptex : texdata.textures) {
        if (miptex.data.size() > sizeof(dmiptex_t)) {
            miptex.stream_write(wadfile);
        }
    }

    wadfile.flush();
    if (!wadfile) {
        FError("error writing WAD data");
    }
}

static void ReplaceTexturesFromWad(mbsp_t &bsp)
{
    auto &texdata = bsp.dtex;

    for (miptex_t &tex : texdata.textures) {
        logging::print("bsp texture: {}\n", tex.name);

        // see if this texture in the .bsp is in the wad?
        if (auto [wadtex_opt, _0, mipdata] =
                img::load_texture(tex.name, false, bsp.loadversion->game, bsputil_options, false, true);
            wadtex_opt) {
            const img::texture &wadtex = *wadtex_opt;

            if (tex.width != wadtex.width || tex.height != wadtex.height) {
                logging::print("    size {}x{} in bsp does not match replacement texture {}x{}\n", tex.width,
                    tex.height, wadtex.width, wadtex.height);
                continue;
            }

            // update the bsp miptex
            tex.null_texture = false;
            tex.data = *mipdata;
            logging::print("    replaced with {} from wad\n", wadtex.meta.name);
        }
    }
}

static void PrintModelInfo(const mbsp_t *bsp)
{
    // TODO: remove, bspinfo .json export is more useful
    for (size_t i = 0; i < bsp->dmodels.size(); i++) {
        const dmodelh2_t *dmodel = &bsp->dmodels[i];
        logging::print("model {:3}: {:5} faces (firstface = {})\n", i, dmodel->numfaces, dmodel->firstface);
    }
}

/*
 * Check vertices of faces lie on the correct plane. Keep this tolerant of
 * malformed input: --check should
 * diagnose bad references, not follow them.
 */
constexpr double PLANE_ON_EPSILON = 0.01;

static bool ValidSignedRange(const int64_t first, const int64_t count, const size_t size)
{
    if (first < 0 || count < 0) {
        return false;
    }

    const uint64_t unsigned_first = static_cast<uint64_t>(first);
    const uint64_t unsigned_count = static_cast<uint64_t>(count);
    return unsigned_first <= size && unsigned_count <= size - static_cast<size_t>(unsigned_first);
}

static bool ValidUnsignedRange(const uint64_t first, const uint64_t count, const size_t size)
{
    return first <= size && count <= size - static_cast<size_t>(first);
}

static uint64_t SurfedgeIndex(const int32_t surfedge)
{
    // Widen before negating so INT32_MIN is well-defined.
    return surfedge < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(surfedge)) : static_cast<uint64_t>(surfedge);
}

static uint64_t LeafIndexForChild(const int32_t child)
{
    // Negative children encode -(leaf + 1). Widen before arithmetic so the
    // INT32_MIN corruption case remains well-defined.
    return static_cast<uint64_t>(-static_cast<int64_t>(child) - 1);
}

template<typename T>
static bool VectorIsFinite(const qvec<T, 3> &value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

static void CheckBSPFacesPlanar(const mbsp_t *bsp)
{
    for (size_t i = 0; i < bsp->dfaces.size(); i++) {
        const mface_t &face = bsp->dfaces[i];
        if (face.planenum < 0 || static_cast<uint64_t>(face.planenum) >= bsp->dplanes.size() ||
            !ValidSignedRange(face.firstedge, face.numedges, bsp->dsurfedges.size())) {
            continue;
        }

        dplane_t plane = bsp->dplanes[static_cast<size_t>(face.planenum)];

        if (face.side) {
            plane = -plane;
        }

        for (int32_t j = 0; j < face.numedges; j++) {
            const size_t surfedge_index = static_cast<size_t>(face.firstedge) + static_cast<size_t>(j);
            const int32_t surfedge = bsp->dsurfedges[surfedge_index];
            const uint64_t edge_index = SurfedgeIndex(surfedge);
            if (edge_index >= bsp->dedges.size()) {
                continue;
            }

            const uint32_t vertex_index = bsp->dedges[static_cast<size_t>(edge_index)][surfedge >= 0 ? 0 : 1];
            if (vertex_index >= bsp->dvertexes.size()) {
                continue;
            }

            const qvec3f &point = bsp->dvertexes[vertex_index];
            const float dist = plane.distance_to(point);

            if (dist < -PLANE_ON_EPSILON || dist > PLANE_ON_EPSILON)
                logging::print("WARNING: face {}, point {} off plane by {}\n", i, j, dist);
        }
    }
}

static bool CalculateNodeHeights(const mbsp_t *bsp, const size_t root, std::vector<size_t> &heights)
{
    enum class visit_state : uint8_t
    {
        unvisited,
        visiting,
        complete
    };

    struct frame_t
    {
        size_t node;
        uint8_t next_child;
    };

    heights.assign(bsp->dnodes.size(), 0);
    std::vector<visit_state> states(bsp->dnodes.size(), visit_state::unvisited);
    std::vector<frame_t> stack;
    stack.reserve(bsp->dnodes.size());
    stack.push_back({root, 0});

    while (!stack.empty()) {
        frame_t &frame = stack.back();
        if (states[frame.node] == visit_state::unvisited) {
            states[frame.node] = visit_state::visiting;
        }

        if (frame.next_child < 2) {
            const uint8_t child_side = frame.next_child++;
            const int32_t child = bsp->dnodes[frame.node].children[child_side];
            if (child < 0) {
                if (LeafIndexForChild(child) >= bsp->dleafs.size()) {
                    logging::print(
                        "warning: can't calculate node heights: node {} child {} references invalid leaf {}\n",
                        frame.node, child_side, LeafIndexForChild(child));
                    return false;
                }
                continue;
            }

            const size_t child_index = static_cast<size_t>(child);
            if (child_index >= bsp->dnodes.size()) {
                logging::print("warning: can't calculate node heights: node {} child {} references invalid node {}\n",
                    frame.node, child_side, child_index);
                return false;
            }
            if (states[child_index] == visit_state::visiting) {
                logging::print("warning: node tree contains a cycle from node {} to node {}; skipping height report\n",
                    frame.node, child_index);
                return false;
            }
            if (states[child_index] == visit_state::unvisited) {
                stack.push_back({child_index, 0});
            }
            continue;
        }

        size_t height = 1;
        for (const int32_t child : bsp->dnodes[frame.node].children) {
            if (child >= 0) {
                height = std::max(height, heights[static_cast<size_t>(child)] + 1);
            }
        }
        heights[frame.node] = height;
        states[frame.node] = visit_state::complete;
        stack.pop_back();
    }

    return true;
}

static void PrintNodeHeights(const mbsp_t *bsp)
{
    if (bsp->dmodels.empty()) {
        return;
    }

    const int32_t root = bsp->dmodels[0].headnode[0];
    if (root < 0) {
        if (LeafIndexForChild(root) >= bsp->dleafs.size()) {
            logging::print("warning: world model references invalid root leaf {}\n", LeafIndexForChild(root));
        }
        return;
    }
    if (static_cast<size_t>(root) >= bsp->dnodes.size()) {
        logging::print("warning: world model references invalid root node {}\n", root);
        return;
    }

    std::vector<size_t> heights;
    if (!CalculateNodeHeights(bsp, static_cast<size_t>(root), heights)) {
        return;
    }

    const int maxlevel = 3;

    using visit_t = std::pair<size_t, int>;

    int current_level = -1;

    std::vector<visit_t> tovisit{{static_cast<size_t>(root), 0}};
    size_t next_visit = 0;
    while (next_visit < tovisit.size()) {
        const auto [node_index, level] = tovisit[next_visit++];
        const bsp2_dnode_t &node = bsp->dnodes[node_index];

        Q_assert(level <= maxlevel);

        // handle this node
        if (level != current_level) {
            current_level = level;
            logging::print("\nNode heights at level {}: ", level);
        }

        // print the level of this node
        logging::print("{}, ", heights[node_index]);

        // add child nodes to the bfs
        if (level < maxlevel) {
            for (int i = 0; i < 2; i++) {
                const int32_t child = node.children[i];
                if (child >= 0) {
                    tovisit.emplace_back(static_cast<size_t>(child), level + 1);
                }
            }
        }
    }
    logging::print("\n");
}

void CheckBSPFile(const mbsp_t *bsp)
{
    if (bsp == nullptr) {
        FError("can't check a null BSP");
    }

    // FIXME: Should do a better reachability check where we traverse the
    // nodes/leafs to find reachable faces.
    std::vector<bool> referenced_texinfos(bsp->texinfo.size());
    std::vector<bool> referenced_planenums(bsp->dplanes.size());
    std::vector<bool> referenced_vertexes(bsp->dvertexes.size());
    std::array<bool, 256> used_lightstyles{};

    /* models */
    for (size_t i = 0; i < bsp->dmodels.size(); ++i) {
        const dmodelh2_t &model = bsp->dmodels[i];
        if (!VectorIsFinite(model.mins) || !VectorIsFinite(model.maxs) || !VectorIsFinite(model.origin)) {
            logging::print("warning: model {} has non-finite bounds or origin\n", i);
        } else if (model.mins[0] > model.maxs[0] || model.mins[1] > model.maxs[1] || model.mins[2] > model.maxs[2]) {
            logging::print("warning: model {} has inverted bounds ({} to {})\n", i, model.mins, model.maxs);
        }
        if (!ValidSignedRange(model.firstface, model.numfaces, bsp->dfaces.size())) {
            const int64_t endface = static_cast<int64_t>(model.firstface) + model.numfaces;
            logging::print("warning: model {} has faces out of range ([{}, {}) vs {})\n", i, model.firstface, endface,
                bsp->dfaces.size());
        }

        const int32_t root = model.headnode[0];
        if (root >= 0 && static_cast<size_t>(root) >= bsp->dnodes.size()) {
            logging::print("warning: model {} references invalid root node {}\n", i, root);
        } else if (root < 0 && LeafIndexForChild(root) >= bsp->dleafs.size()) {
            logging::print("warning: model {} references invalid root leaf {}\n", i, LeafIndexForChild(root));
        }
    }

    /* planes and vertices */
    for (size_t i = 0; i < bsp->dplanes.size(); ++i) {
        const dplane_t &plane = bsp->dplanes[i];
        if (!VectorIsFinite(plane.normal) || !std::isfinite(plane.dist) || qv::length2(plane.normal) == 0.0f) {
            logging::print("warning: plane {} has an invalid normal or distance\n", i);
        }
    }
    for (size_t i = 0; i < bsp->dvertexes.size(); ++i) {
        if (!VectorIsFinite(bsp->dvertexes[i])) {
            logging::print("warning: vertex {} has non-finite coordinates\n", i);
        }
    }

    /* faces */
    for (size_t i = 0; i < bsp->dfaces.size(); i++) {
        const mface_t &face = bsp->dfaces[i];

        /* texinfo bounds check */
        if (face.texinfo < 0) {
            logging::print("warning: face {} has negative texinfo ({})\n", i, face.texinfo);
        } else if (static_cast<size_t>(face.texinfo) >= bsp->texinfo.size()) {
            logging::print(
                "warning: face {} has texinfo out of range ({} >= {})\n", i, face.texinfo, bsp->texinfo.size());
        } else {
            referenced_texinfos[static_cast<size_t>(face.texinfo)] = true;
        }

        /* planenum bounds check */
        if (face.planenum < 0) {
            logging::print("warning: face {} has negative planenum ({})\n", i, face.planenum);
        } else if (static_cast<uint64_t>(face.planenum) >= bsp->dplanes.size()) {
            logging::print(
                "warning: face {} has planenum out of range ({} >= {})\n", i, face.planenum, bsp->dplanes.size());
        } else {
            referenced_planenums[static_cast<size_t>(face.planenum)] = true;
        }

        /* lightofs check */
        if (face.lightofs < -1) {
            logging::print("warning: face {} has negative light offset ({})\n", i, face.lightofs);
        } else if (face.lightofs >= 0 && static_cast<size_t>(face.lightofs) >= bsp->dlightdata.size()) {
            logging::print("warning: face {} has light offset out of range "
                           "({} >= {})\n",
                i, face.lightofs, bsp->dlightdata.size());
        }

        /* edge check */
        if (face.firstedge < 0)
            logging::print("warning: face {} has negative firstedge ({})\n", i, face.firstedge);
        if (face.numedges < 3)
            logging::print("warning: face {} has < 3 edges ({})\n", i, face.numedges);
        if (!ValidSignedRange(face.firstedge, face.numedges, bsp->dsurfedges.size())) {
            const int64_t end = static_cast<int64_t>(face.firstedge) + static_cast<int64_t>(face.numedges);
            logging::print("warning: face {} has edges out of range ([{}, {}) vs {})\n", i, face.firstedge, end,
                bsp->dsurfedges.size());
        }

        bool saw_style_terminator = false;
        std::array<bool, 256> face_styles_seen{};
        size_t face_style_count = 0;
        for (size_t style_slot = 0; style_slot < face.styles.size(); ++style_slot) {
            const uint8_t style = face.styles[style_slot];
            if (style == INVALID_LIGHTSTYLE_OLD) {
                saw_style_terminator = true;
                continue;
            }

            ++face_style_count;
            used_lightstyles[style] = true;
            if (saw_style_terminator) {
                logging::print(
                    "warning: face {} has light style {} after a terminator in slot {}\n", i, style, style_slot);
            }
            if (face_styles_seen[style]) {
                logging::print("warning: face {} repeats light style {}\n", i, style);
            }
            face_styles_seen[style] = true;
        }
        if (face.lightofs >= 0 && face_style_count == 0) {
            logging::print("warning: face {} has light data offset {} but no light styles\n", i, face.lightofs);
        }
    }

    /* edges */
    for (size_t i = 0; i < bsp->dedges.size(); i++) {
        const bsp2_dedge_t &edge = bsp->dedges[i];

        for (size_t j = 0; j < 2; j++) {
            const uint32_t vertex = edge[j];
            if (vertex >= bsp->dvertexes.size()) {
                logging::print("warning: edge {} has vertex {} out range "
                               "({} >= {})\n",
                    i, j, vertex, bsp->dvertexes.size());
            } else {
                referenced_vertexes[vertex] = true;
            }
        }
    }

    /* surfedges */
    for (size_t i = 0; i < bsp->dsurfedges.size(); i++) {
        const int32_t edgenum = bsp->dsurfedges[i];
        if (!edgenum)
            logging::print("warning: surfedge {} has zero value!\n", i);
        if (SurfedgeIndex(edgenum) >= bsp->dedges.size())
            logging::print("warning: surfedge {} is out of range (abs({}) >= {})\n", i, edgenum, bsp->dedges.size());
    }

    /* marksurfaces */
    for (size_t i = 0; i < bsp->dleaffaces.size(); i++) {
        const uint32_t surfnum = bsp->dleaffaces[i];
        if (surfnum >= bsp->dfaces.size())
            logging::print("warning: marksurface {} is out of range ({} >= {})\n", i, surfnum, bsp->dfaces.size());
    }

    /* leafs */
    for (size_t i = 0; i < bsp->dleafs.size(); i++) {
        const mleaf_t &leaf = bsp->dleafs[i];
        const uint64_t endmarksurface = static_cast<uint64_t>(leaf.firstmarksurface) + leaf.nummarksurfaces;
        if (!ValidUnsignedRange(leaf.firstmarksurface, leaf.nummarksurfaces, bsp->dleaffaces.size()))
            logging::print("warning: leaf {} has marksurfaces out of range "
                           "([{}, {}) vs {})\n",
                i, leaf.firstmarksurface, endmarksurface, bsp->dleaffaces.size());
        if (leaf.visofs < -1) {
            logging::print("warning: leaf {} has negative visdata offset ({})\n", i, leaf.visofs);
        } else if (leaf.visofs >= 0 && static_cast<size_t>(leaf.visofs) >= bsp->dvis.bits.size()) {
            logging::print("warning: leaf {} has visdata offset out of range "
                           "({} >= {})\n",
                i, leaf.visofs, bsp->dvis.bits.size());
        }
    }

    /* nodes */
    for (size_t i = 0; i < bsp->dnodes.size(); i++) {
        const bsp2_dnode_t &node = bsp->dnodes[i];

        for (size_t j = 0; j < 2; j++) {
            const int32_t child = node.children[j];
            if (child >= 0 && static_cast<size_t>(child) >= bsp->dnodes.size())
                logging::print("warning: node {} has child {} (node) out of range "
                               "({} >= {})\n",
                    i, j, child, bsp->dnodes.size());
            if (child < 0 && LeafIndexForChild(child) >= bsp->dleafs.size())
                logging::print("warning: node {} has child {} (leaf) out of range "
                               "({} >= {})\n",
                    i, j, LeafIndexForChild(child), bsp->dleafs.size());
        }

        if (node.children[0] == node.children[1]) {
            logging::print("warning: node {} has both children {}\n", i, node.children[0]);
        }

        if (node.planenum < 0 || static_cast<size_t>(node.planenum) >= bsp->dplanes.size()) {
            logging::print(
                "warning: node {} has planenum out of range ({} vs {})\n", i, node.planenum, bsp->dplanes.size());
        } else {
            referenced_planenums[static_cast<size_t>(node.planenum)] = true;
        }

        if (!ValidUnsignedRange(node.firstface, node.numfaces, bsp->dfaces.size())) {
            const uint64_t endface = static_cast<uint64_t>(node.firstface) + node.numfaces;
            logging::print("warning: node {} has faces out of range ([{}, {}) vs {})\n", i, node.firstface, endface,
                bsp->dfaces.size());
        }
    }

    /* clipnodes */
    for (size_t i = 0; i < bsp->dclipnodes.size(); i++) {
        const bsp2_dclipnode_t &clipnode = bsp->dclipnodes[i];

        for (size_t j = 0; j < 2; j++) {
            const int32_t child = clipnode.children[j];
            if (child >= 0 && static_cast<size_t>(child) >= bsp->dclipnodes.size())
                logging::print("warning: clipnode {} has child {} (clipnode) out of range "
                               "({} >= {})\n",
                    i, j, child, bsp->dclipnodes.size());
            if (child < 0 && child < CONTENTS_MIN)
                logging::print("warning: clipnode {} has invalid contents ({}) for child {}\n", i, child, j);
        }

        if (clipnode.children[0] == clipnode.children[1]) {
            logging::print("warning: clipnode {} has both children {}\n", i, clipnode.children[0]);
        }

        if (clipnode.planenum < 0 || static_cast<size_t>(clipnode.planenum) >= bsp->dplanes.size()) {
            logging::print("warning: clipnode {} has planenum out of range ({} vs {})\n", i, clipnode.planenum,
                bsp->dplanes.size());
        } else {
            referenced_planenums[static_cast<size_t>(clipnode.planenum)] = true;
        }
    }

    /* TODO: finish range checks, add "unreferenced" checks... */

    /* unreferenced texinfo */
    {
        const size_t num_unreferenced_texinfo =
            std::count(referenced_texinfos.begin(), referenced_texinfos.end(), false);
        if (num_unreferenced_texinfo)
            logging::print("warning: {} texinfos are unreferenced\n", num_unreferenced_texinfo);
    }

    /* unreferenced planes */
    {
        const size_t num_unreferenced_planes =
            std::count(referenced_planenums.begin(), referenced_planenums.end(), false);
        if (num_unreferenced_planes)
            logging::print("warning: {} planes are unreferenced\n", num_unreferenced_planes);
    }

    /* unreferenced vertices */
    {
        const size_t num_unreferenced_vertexes =
            std::count(referenced_vertexes.begin(), referenced_vertexes.end(), false);
        if (num_unreferenced_vertexes)
            logging::print("warning: {} vertexes are unreferenced\n", num_unreferenced_vertexes);
    }

    /* tree balance */
    PrintNodeHeights(bsp);

    /* unique visofs's */
    std::set<int32_t> visofs_set;
    for (const mleaf_t &leaf : bsp->dleafs) {
        if (leaf.visofs >= 0 && static_cast<size_t>(leaf.visofs) < bsp->dvis.bits.size()) {
            visofs_set.insert(leaf.visofs);
        }
    }
    logging::print("{} unique visdata offsets for {} leafs\n", visofs_set.size(), bsp->dleafs.size());
    if (!bsp->dmodels.empty()) {
        logging::print("{} visleafs in world model\n", bsp->dmodels[0].visleafs);
    } else {
        logging::print("warning: BSP contains no models\n");
    }

    /* unique lightstyles */
    logging::print("{} lightstyles used:\n", std::count(used_lightstyles.begin(), used_lightstyles.end(), true));
    for (size_t style = 0; style < used_lightstyles.size(); ++style) {
        if (used_lightstyles[style]) {
            logging::print("\t{}\n", style);
        }
    }

    if (!bsp->dmodels.empty()) {
        logging::print("world mins: {} maxs: {}\n", bsp->dmodels[0].mins, bsp->dmodels[0].maxs);
    }

    CheckBSPFacesPlanar(bsp);
}

static void FindFaces(const mbsp_t *bsp, const qvec3d &pos, const qvec3d &normal)
{
    for (int i = 0; i < bsp->dmodels.size(); ++i) {
        const dmodelh2_t *model = &bsp->dmodels[i];
        const mface_t *face = BSP_FindFaceAtPoint(bsp, model, pos, normal);

        if (face != nullptr) {
            logging::print("model {} face {}: texture '{}' texinfo {}\n", i, Face_GetNum(bsp, face),
                Face_TextureName(bsp, face), face->texinfo);
        }
    }
}

static void FindLeaf(const mbsp_t *bsp, const qvec3d &pos)
{
    const mleaf_t *leaf = BSP_FindLeafAtPoint(bsp, &bsp->dmodels[0], pos);

    logging::print("leaf {}: contents {} ({})\n", (leaf - bsp->dleafs.data()), leaf->contents,
        bsp->loadversion->game->create_contents_from_native(leaf->contents).to_string());
}

// map file stuff
struct map_entity_t
{
    entdict_t epairs;
    parser_source_location location;
    std::string map_brushes; // raw brush data
};

struct map_file_t
{
    std::vector<map_entity_t> entities;
};

static void ParseEpair(parser_t &parser, map_entity_t &entity)
{
    std::string key = parser.token;

    // trim whitespace from start/end
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) {
        key.erase(key.begin());
    }
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
        key.erase(key.end() - 1);
    }

    if (key.empty()) {
        FError("{}: Entity key is empty or contains only whitespace", parser.location);
    }

    parser.parse_token(PARSE_SAMELINE);

    entity.epairs.set(key, parser.token);
}

bool ParseEntity(parser_t &parser, map_entity_t &entity)
{
    entity.location = parser.location;

    if (!parser.parse_token()) {
        return false;
    }

    if (parser.token != "{") {
        FError("{}: Invalid entity format, {{ not found", parser.location);
    }

    do {
        if (!parser.parse_token())
            FError("Unexpected EOF (no closing brace)");
        if (parser.token == "}")
            break;
        else if (parser.token == "{") {
            auto start = parser.pos - 1;

            // skip until a }
            do {
                if (!parser.parse_token()) {
                    FError("Unexpected EOF (no closing brace)");
                }
            } while (parser.token != "}");

            auto end = parser.pos;
            entity.map_brushes += std::string(start, end) + "\n";
        } else {
            ParseEpair(parser, entity);
        }
    } while (1);

    return true;
}

map_file_t LoadMapOrEntFile(const fs::path &source)
{
    logging::funcheader();

    auto file = fs::load(source);
    map_file_t map;

    if (!file) {
        FError("Couldn't load map/entity file \"{}\".\n", source);
        return map;
    }

    parser_t parser(file, {source.string()});

    for (;;) {
        map_entity_t &entity = map.entities.emplace_back();

        if (!ParseEntity(parser, entity)) {
            break;
        }
    }

    // Remove dummy entity inserted above
    assert(!map.entities.back().epairs.size());
    map.entities.pop_back();

    return map;
}

struct planepoints : std::array<qvec3d, 3>
{
    qplane3d plane() const
    {
        /* calculate the normal/dist plane equation */
        qvec3d ab = at(0) - at(1);
        qvec3d cb = at(2) - at(1);
        qvec3d normal = qv::normalize(qv::cross(ab, cb));
        return {normal, qv::dot(at(1), normal)};
    }
};

template<typename T>
static planepoints NormalDistanceToThreePoints(const qplane3<T> &plane)
{
    std::tuple<qvec3d, qvec3d> tanBitan = qv::MakeTangentAndBitangentUnnormalized(plane.normal);

    qvec3d point0 = plane.normal * plane.dist;

    return {point0, point0 + std::get<1>(tanBitan), point0 + std::get<0>(tanBitan)};
}

#include <pareto/spatial_map.h>

struct planelist_t
{
    // planes indices (into the `planes` vector)
    pareto::spatial_map<double, 4, size_t> plane_hash;
    std::vector<dplane_t> planes;

    // add the specified plane to the list
    size_t add_plane(const dplane_t &plane)
    {
        planes.emplace_back(plane);
        planes.emplace_back(-plane);

        size_t positive_index = planes.size() - 2;
        size_t negative_index = planes.size() - 1;

        auto &positive = planes[positive_index];
        auto &negative = planes[negative_index];

        size_t result;

        if (positive.normal[static_cast<int32_t>(positive.type) % 3] < 0.0) {
            std::swap(positive, negative);
            result = negative_index;
        } else {
            result = positive_index;
        }

        plane_hash.emplace(
            pareto::point<double, 4>{positive.normal[0], positive.normal[1], positive.normal[2], positive.dist},
            positive_index);
        plane_hash.emplace(
            pareto::point<double, 4>{negative.normal[0], negative.normal[1], negative.normal[2], negative.dist},
            negative_index);

        return result;
    }

    std::optional<size_t> find_plane_nonfatal(const dplane_t &plane)
    {
        constexpr double HALF_NORMAL_EPSILON = NORMAL_EPSILON * 0.5;
        constexpr double HALF_DIST_EPSILON = DIST_EPSILON * 0.5;

        if (auto it = plane_hash.find_intersection(
                {plane.normal[0] - HALF_NORMAL_EPSILON, plane.normal[1] - HALF_NORMAL_EPSILON,
                    plane.normal[2] - HALF_NORMAL_EPSILON, plane.dist - HALF_DIST_EPSILON},
                {plane.normal[0] + HALF_NORMAL_EPSILON, plane.normal[1] + HALF_NORMAL_EPSILON,
                    plane.normal[2] + HALF_NORMAL_EPSILON, plane.dist + HALF_DIST_EPSILON});
            it != plane_hash.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    // find the specified plane in the list if it exists. throws
    // if not.
    size_t find_plane(const dplane_t &plane)
    {
        if (auto index = find_plane_nonfatal(plane)) {
            return *index;
        }

        throw std::bad_function_call();
    }

    // find the specified plane in the list if it exists, or
    // return a new one
    size_t add_or_find_plane(const dplane_t &plane)
    {
        if (auto index = find_plane_nonfatal(plane)) {
            return *index;
        }

        return add_plane(plane);
    }
};

static void WriteBSPCopy(const fs::path &path, const bspdata_t &bspdata, const bspversion_t *version)
{
    bspdata_t output = bspdata;
    if (!ConvertBSPFormat(&output, version)) {
        FError("failed to convert BSP to {}", version->short_name);
    }
    WriteBSPFile(path, &output);
}

static void ScaleLightgridOctreeHeader(std::vector<uint8_t> &lump_bytes, const qvec3d &scalar)
{
    constexpr size_t lightgrid_header_size =
        (3 * sizeof(float)) + (3 * sizeof(int32_t)) + (3 * sizeof(float)) + sizeof(uint8_t) + sizeof(uint32_t);
    if (lump_bytes.size() < lightgrid_header_size) {
        FError("LIGHTGRID_OCTREE lump is truncated");
    }

    auto istream = imemstream(lump_bytes.data(), lump_bytes.size());
    istream >> endianness<std::endian::little>;

    lightgrid_header_t header;
    istream >= header;
    if (!istream) {
        FError("LIGHTGRID_OCTREE lump has a truncated header");
    }

    auto checked_float = [](double value, std::string_view field, size_t axis) -> float {
        constexpr double float_max = std::numeric_limits<float>::max();
        if (!std::isfinite(value) || value < -float_max || value > float_max) {
            FError("LIGHTGRID_OCTREE {} component {} is outside the finite float range", field, axis);
        }
        return static_cast<float>(value);
    };

    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(header.grid_dist[axis]) || header.grid_dist[axis] <= 0.0f) {
            FError("LIGHTGRID_OCTREE grid_dist component {} must be finite and greater than zero", axis);
        }
        if (header.grid_size[axis] <= 0) {
            FError("LIGHTGRID_OCTREE grid_size component {} must be greater than zero", axis);
        }
        if (!std::isfinite(header.grid_mins[axis])) {
            FError("LIGHTGRID_OCTREE grid_mins component {} must be finite", axis);
        }

        // The positivity check above makes this subtraction safe even when a
        // corrupt lump contains INT32_MIN.
        const double grid_steps = static_cast<double>(header.grid_size[axis] - 1);
        const double original_grid_max =
            static_cast<double>(header.grid_mins[axis]) + static_cast<double>(header.grid_dist[axis]) * grid_steps;
        checked_float(original_grid_max, "implied grid maximum", axis);

        const float scaled_grid_dist =
            checked_float(static_cast<double>(header.grid_dist[axis]) * scalar[axis], "scaled grid_dist", axis);
        if (!std::isfinite(scaled_grid_dist) || scaled_grid_dist <= 0.0f) {
            FError("LIGHTGRID_OCTREE scaled grid_dist component {} must remain finite and greater than zero", axis);
        }
        header.grid_dist[axis] = scaled_grid_dist;
        header.grid_mins[axis] =
            checked_float(static_cast<double>(header.grid_mins[axis]) * scalar[axis], "scaled grid_mins", axis);

        const double scaled_grid_max =
            static_cast<double>(header.grid_mins[axis]) + static_cast<double>(header.grid_dist[axis]) * grid_steps;
        checked_float(scaled_grid_max, "scaled implied grid maximum", axis);
    }

    auto ostream = omemstream(lump_bytes.data(), lump_bytes.size());
    ostream << endianness<std::endian::little>;
    ostream <= header;
    if (!ostream) {
        FError("error updating LIGHTGRID_OCTREE header");
    }
}

static int bsputil_main_impl(int _argc, const char **_argv)
{
    logging::preinitialize();

    bsputil_options.reset();
    bsputil_options.preinitialize(_argc, _argv);
    bsputil_options.initialize(_argc - 1, _argv + 1);
    bsputil_options.postinitialize(_argc, _argv);

    logging::init(std::nullopt, bsputil_options);

    if (bsputil_options.remainder.size() != 1 || bsputil_options.operations.empty()) {
        bsputil_options.print_help(false);
        return 1;
    }

    bspdata_t bspdata;

    fs::path source = bsputil_options.remainder[0];

    if (!fs::exists(source)) {
        source = DefaultExtension(source, "bsp");
    }

    logging::print("---------------------\n");
    logging::print("{}\n", source);

    map_file_t map_file;

    const bool source_is_bsp = string_iequals(source.extension().string(), ".bsp");
    const bspversion_t *current_write_version = nullptr;

    if (source_is_bsp) {
        LoadBSPFile(source, &bspdata);

        bspdata.version->game->init_filesystem(source, bsputil_options);

        if (!ConvertBSPFormat(&bspdata, &bspver_generic)) {
            FError("couldn't convert {} to the generic BSP representation", source);
        }
        current_write_version = bspdata.loadversion;
    } else {
        map_file = LoadMapOrEntFile(source);
    }

    // Keep output naming anchored to the resolved input path, but advance the
    // mutation target as output-producing operations create derived BSPs.
    fs::path current_write_path = source;

    for (auto &operation : bsputil_options.operations) {
        if (!source_is_bsp && operation->primary_name() != "replace-entities") {
            FError("option -{} requires a BSP input file", operation->primary_name());
        }

        if (operation->primary_name() == "svg") {
            fs::path svg = fs::path(source).replace_extension(".svg");
            std::ofstream f(svg, std::ios_base::out);
            if (!f) {
                FError("couldn't open {} for writing", svg);
            }

            f << R"(<?xml version="1.0" encoding="UTF-8"?>)" << std::endl;
            f << R"(<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">)"
              << std::endl;

            auto &bsp = std::get<mbsp_t>(bspdata.bsp);

            img::load_textures(&bsp, {});

            struct rendered_faces_t
            {
                std::vector<const mface_t *> faces;
                qvec3f origin;
                aabb3f bounds;
            };

            std::vector<rendered_faces_t> faces;
            aabb3f total_bounds;
            size_t total_faces = 0;
            auto ents = EntData_Parse(bsp);

            auto addSubModel = [&bsp, &faces, &total_bounds, &total_faces](int32_t index, qvec3f origin) {
                if (index < 0 || static_cast<size_t>(index) >= bsp.dmodels.size()) {
                    logging::print("WARNING: ignoring entity with invalid BSP model index {}\n", index);
                    return;
                }

                auto &model = bsp.dmodels[index];
                rendered_faces_t f{{}, origin};

                if (model.firstface < 0 || model.numfaces < 0 ||
                    static_cast<size_t>(model.firstface) > bsp.dfaces.size() ||
                    static_cast<size_t>(model.numfaces) > bsp.dfaces.size() - static_cast<size_t>(model.firstface)) {
                    FError("model {} has an invalid face range", index);
                }

                std::vector<size_t> face_ids;
                face_ids.reserve(model.numfaces);

                for (size_t i = model.firstface; i < model.firstface + model.numfaces; i++) {
                    auto &face = bsp.dfaces[i];

                    if (face.texinfo == -1)
                        continue;

                    if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= bsp.texinfo.size()) {
                        FError("face {} has invalid texinfo {}", i, face.texinfo);
                    }

                    auto &texinfo = bsp.texinfo[face.texinfo];

                    if (texinfo.flags.is_nodraw())
                        continue;
                    // TODO
                    // else if (texinfo.flags.native & Q2_SURF_SKY)
                    //    continue;
                    else if (!Q_strcasecmp(Face_TextureName(&bsp, &face), "trigger"))
                        continue;

                    auto norm = Face_Normal(&bsp, &face);

                    if (qv::dot(qvec3d(0, 0, 1), norm) <= DEFAULT_ON_EPSILON)
                        continue;

                    face_ids.push_back(i);
                }

                std::sort(face_ids.begin(), face_ids.end(), [&bsp](size_t a, size_t b) {
                    float za = std::numeric_limits<float>::lowest();
                    float zb = za;
                    auto &facea = bsp.dfaces[a];
                    auto &faceb = bsp.dfaces[b];

                    for (size_t e = 0; e < facea.numedges; e++)
                        za = std::max(za, Face_PointAtIndex(&bsp, &facea, e)[2]);

                    for (size_t e = 0; e < faceb.numedges; e++)
                        zb = std::max(zb, Face_PointAtIndex(&bsp, &faceb, e)[2]);

                    return za < zb;
                });

                for (auto &face_index : face_ids) {
                    const auto &face = bsp.dfaces[face_index];
                    f.faces.push_back(&face);

                    for (auto pt : Face_Points(&bsp, &face))
                        f.bounds += f.origin + pt;
                }

                if (f.faces.empty())
                    return;

                total_bounds += f.bounds;
                total_faces += f.faces.size();
                faces.push_back(std::move(f));
            };

            addSubModel(0, {});

            for (auto &entity : ents) {
                if (!entity.has("model"))
                    continue;

                const std::string &model_value = entity.get("model");
                if (model_value.size() < 2 || model_value.front() != '*') {
                    logging::print("WARNING: ignoring invalid BSP model reference '{}'\n", model_value);
                    continue;
                }

                int32_t model = -1;
                const char *first = model_value.data() + 1;
                const char *last = model_value.data() + model_value.size();
                const auto [end, parse_error] = std::from_chars(first, last, model);
                if (parse_error != std::errc{} || end != last || model <= 0) {
                    logging::print("WARNING: ignoring invalid BSP model reference '{}'\n", model_value);
                    continue;
                }

                qvec3f origin{};
                if (entity.has("origin"))
                    entity.get_vector("origin", origin);

                addSubModel(model, origin);
            }

            if (faces.empty()) {
                FError("BSP has no drawable upward-facing surfaces for SVG output");
            }

            total_bounds = total_bounds.grow(32);

            float xo = total_bounds.mins()[0];
            float yo = total_bounds.mins()[1];
            // float zo = total_bounds.mins()[2];

            float xs = total_bounds.maxs()[0] - xo;
            float ys = total_bounds.maxs()[1] - yo;
            // float zs = total_bounds.maxs()[2] - zo;

            fmt::print(f, R"(<svg xmlns="http://www.w3.org/2000/svg" version="1.1" width="{}" height="{}">)", xs, ys);
            f << std::endl;

            f << R"(<defs><g id="bsp">)" << std::endl;

            struct face_id_t
            {
                size_t model;
                size_t face;
            };
            std::vector<face_id_t> face_ids;
            face_ids.reserve(total_faces);

            for (size_t i = 0; i < faces.size(); i++)
                for (size_t f = 0; f < faces[i].faces.size(); f++)
                    face_ids.push_back(face_id_t{i, f});

            std::sort(face_ids.begin(), face_ids.end(), [&bsp, &faces, yo](face_id_t a, face_id_t b) {
                float za = yo;
                float zb = yo;
                auto facea = faces[a.model].faces[a.face];
                auto faceb = faces[b.model].faces[b.face];

                for (size_t e = 0; e < facea->numedges; e++)
                    za = std::max(za, Face_PointAtIndex(&bsp, facea, e)[2] + faces[a.model].origin[2]);

                for (size_t e = 0; e < faceb->numedges; e++)
                    zb = std::max(zb, Face_PointAtIndex(&bsp, faceb, e)[2] + faces[b.model].origin[2]);

                return za < zb;
            });

            float low_z = total_bounds.maxs()[2], high_z = total_bounds.mins()[2];

            for (auto &face_index : face_ids) {
                auto face = faces[face_index.model].faces[face_index.face];

                for (auto &pt : Face_Points(&bsp, face)) {
                    low_z = std::min(low_z, pt[2] + faces[face_index.model].origin[2]);
                    high_z = std::max(high_z, pt[2] + faces[face_index.model].origin[2]);
                }
            }

            for (auto &face_index : face_ids) {
                auto face = faces[face_index.model].faces[face_index.face];
                auto pts = Face_Points(&bsp, face);
                std::string pts_str;
                float nz = xo;

                for (auto &pt : pts) {
                    fmt::format_to(std::back_inserter(pts_str), "{},{} ",
                        (pt[0] + faces[face_index.model].origin[0]) - xo,
                        ys - ((pt[1] + faces[face_index.model].origin[1]) - yo));
                    nz = std::max(nz, pt[2] + faces[face_index.model].origin[2]);
                }

                const float z_range = high_z - low_z;
                float z_scale = z_range > 0.0f ? (nz - low_z) / z_range : 0.0f;
                float d = (0.5 + (z_scale * 0.5));
                qvec3b color{255, 255, 255};

                const char *tex = Face_TextureName(&bsp, face);

                if (tex) {
                    if (auto texptr = img::find(tex))
                        color = texptr->averageColor;
                }

                fmt::print(f, R"svg(<polygon points="{}" fill="rgb({}, {}, {})" />)svg", pts_str, color[0] * d,
                    color[1] * d, color[2] * d);
                f << std::endl;
            }

            f << R"(</g></defs>)" << std::endl;

            f << R"(<use href="#bsp" fill="none" stroke="black" stroke-width="15" stroke-miterlimit="0" />)"
              << std::endl;
            f << R"(<use href="#bsp" fill="white" stroke="black" stroke-width="1" />)" << std::endl;

            f << R"(</svg>)" << std::endl;
            if (!f) {
                FError("error writing {}", svg);
            }
        } else if (operation->primary_name() == "scale") {
            qvec3d scalar = dynamic_cast<settings::setting_vec3 *>(operation.get())->value();
            for (double component : scalar) {
                if (!std::isfinite(component) || component == 0.0) {
                    FError("scale components must be finite and non-zero (got {})", scalar);
                }
            }
            logging::print("scaling by {}\n", scalar);

            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            // adjust entity origins
            {
                auto ents = EntData_Parse(bsp);

                for (auto &ent : ents) {
                    if (ent.has("origin")) {
                        qvec3f origin;
                        ent.get_vector("origin", origin);
                        origin *= scalar;
                        ent.set("origin", fmt::format("{} {} {}", origin[0], origin[1], origin[2]));
                    }

                    if (ent.has("lip")) {
                        float lip = ent.get_float("lip");
                        lip -= 2.0f;
                        lip *= scalar[2];
                        lip += 2.0f;
                        ent.set("lip", fmt::format("{}", lip));
                    }

                    if (ent.has("height")) {
                        // FIXME: check this
                        float height = ent.get_float("height");
                        height *= scalar[2];
                        ent.set("height", fmt::format("{}", height));
                    }
                }

                bsp.dentdata = EntData_Write(ents);
            }

            // adjust vertices
            for (auto &v : bsp.dvertexes) {
                v *= scalar;
            }

            // flip edge lists if we need to
            int32_t flip_faces = !!(scalar[0] < 0) + !!(scalar[1] < 0) + !!(scalar[2] < 0);
            const bool flips_orientation = (flip_faces & 1) != 0;

            if (flips_orientation) {
                for (auto &s : bsp.dfaces) {
                    if (s.firstedge < 0 || s.numedges < 0 || static_cast<size_t>(s.firstedge) > bsp.dsurfedges.size() ||
                        static_cast<size_t>(s.numedges) > bsp.dsurfedges.size() - static_cast<size_t>(s.firstedge)) {
                        FError("BSP face has an invalid surface-edge range");
                    }
                    std::reverse(
                        bsp.dsurfedges.data() + s.firstedge, bsp.dsurfedges.data() + (s.firstedge + s.numedges));
                }
            }

            std::unordered_map<size_t, size_t> plane_remap;

            // rebuild planes
            {
                size_t i = 0;
                planelist_t new_planes;

                for (auto &p : bsp.dplanes) {
                    auto pts = NormalDistanceToThreePoints(p);

                    for (auto &pt : pts) {
                        pt *= scalar;
                    }

                    if (flips_orientation) {
                        std::reverse(pts.begin(), pts.end());
                    }

                    dplane_t scaled{qplane3f(pts.plane()), p.type};

                    plane_remap[i] = new_planes.add_or_find_plane(scaled);
                    i++;
                }

                // remap plane list
                bsp.dplanes = std::move(new_planes.planes);
            }

            auto remap_plane = [&plane_remap](auto planenum) -> size_t {
                using planenum_type = decltype(planenum);
                if constexpr (std::is_signed_v<planenum_type>) {
                    if (planenum < 0) {
                        FError("BSP contains a negative plane index ({})", planenum);
                    }
                }
                const size_t index = static_cast<size_t>(planenum);
                const auto it = plane_remap.find(index);
                if (it == plane_remap.end()) {
                    FError("BSP plane index {} is out of range", index);
                }
                return it->second;
            };

            // adjust node/leaf/model bounds
            for (auto &m : bsp.dmodels) {
                m.origin *= scalar;

                qvec3f scaled_mins = m.mins * scalar;
                qvec3f scaled_maxs = m.maxs * scalar;

                m.mins = qv::min(scaled_mins, scaled_maxs);
                m.maxs = qv::max(scaled_mins, scaled_maxs);
            }

            for (auto &l : bsp.dleafs) {
                qvec3f scaled_mins = l.mins * scalar;
                qvec3f scaled_maxs = l.maxs * scalar;

                l.mins = qv::min(scaled_mins, scaled_maxs);
                l.maxs = qv::max(scaled_mins, scaled_maxs);

                for (auto &v : l.mins) {
                    v = floor(v);
                }
                for (auto &v : l.maxs) {
                    v = ceil(v);
                }
            }

            for (auto &m : bsp.dnodes) {
                qvec3f scaled_mins = m.mins * scalar;
                qvec3f scaled_maxs = m.maxs * scalar;

                m.mins = qv::min(scaled_mins, scaled_maxs);
                m.maxs = qv::max(scaled_mins, scaled_maxs);

                for (auto &v : m.mins) {
                    v = floor(v);
                }
                for (auto &v : m.maxs) {
                    v = ceil(v);
                }

                m.planenum = remap_plane(m.planenum);

                if (m.planenum & 1) {
                    std::reverse(m.children.begin(), m.children.end());
                    m.planenum &= ~1;
                }
            }

            for (auto &clipnode : bsp.dclipnodes) {
                clipnode.planenum = remap_plane(clipnode.planenum);

                if (clipnode.planenum & 1) {
                    std::reverse(clipnode.children.begin(), clipnode.children.end());
                    clipnode.planenum &= ~1;
                }
            }

            // remap planes on stuff
            for (auto &v : bsp.dbrushsides) {
                v.planenum = remap_plane(v.planenum);
            }

            for (auto &v : bsp.dfaces) {
                v.planenum = remap_plane(v.planenum);
            }

            auto scaleTexInfo = [&](mtexinfo_t &t) {
                // update texinfo

                const qmat3x3d inversescaleM{// column-major...
                    1 / scalar[0], 0.0, 0.0, 0.0, 1 / scalar[1], 0.0, 0.0, 0.0, 1 / scalar[2]};

                auto &texvecs = t.vecs;
                texvecf newtexvecs;

                for (int i = 0; i < 2; i++) {
                    const qvec4f in = texvecs.row(i);
                    const qvec3f in_first3(in);

                    const qvec3f out_first3 = inversescaleM * in_first3;
                    newtexvecs.set_row(i, {out_first3[0], out_first3[1], out_first3[2], in[3]});
                }

                texvecs = newtexvecs;
            };

            // adjust texinfo
            for (auto &t : bsp.texinfo) {
                scaleTexInfo(t);
            }

            // adjust decoupled LM
            if (bspdata.bspx.entries.contains("DECOUPLED_LM")) {

                auto &lump_bytes = bspdata.bspx.entries.at("DECOUPLED_LM");

                constexpr size_t decoupled_lm_record_size = 2 + 2 + 4 + (2 * 4 * sizeof(float));
                if (bsp.dfaces.size() > std::numeric_limits<size_t>::max() / decoupled_lm_record_size ||
                    lump_bytes.size() != bsp.dfaces.size() * decoupled_lm_record_size) {
                    FError("DECOUPLED_LM lump size does not match the BSP face count");
                }

                auto istream = imemstream(lump_bytes.data(), lump_bytes.size());
                auto ostream = omemstream(lump_bytes.data(), lump_bytes.size());

                istream >> endianness<std::endian::little>;
                ostream << endianness<std::endian::little>;

                bspx_decoupled_lm_perface result;

                for ([[maybe_unused]] auto &face : bsp.dfaces) {
                    istream >= result;

                    const qmat3x3d inversescaleM{// column-major...
                        1 / scalar[0], 0.0, 0.0, 0.0, 1 / scalar[1], 0.0, 0.0, 0.0, 1 / scalar[2]};

                    auto &texvecs = result.world_to_lm_space;
                    texvecf newtexvecs;

                    for (int i = 0; i < 2; i++) {
                        const qvec4f in = texvecs.row(i);
                        const qvec3f in_first3(in);

                        const qvec3f out_first3 = inversescaleM * in_first3;
                        newtexvecs.set_row(i, {out_first3[0], out_first3[1], out_first3[2], in[3]});
                    }

                    texvecs = newtexvecs;

                    ostream <= result;
                }
            }

            // Adjust the direct LIGHTGRID_OCTREE header. Mirroring also
            // requires reordering its samples, so drop it rather than leaving
            // subtly invalid data behind.
            if (bspdata.bspx.entries.contains("LIGHTGRID_OCTREE")) {
                if (scalar[0] < 0 || scalar[1] < 0 || scalar[2] < 0) {
                    logging::print(
                        "WARNING: removing LIGHTGRID_OCTREE after mirrored scaling; re-run vmt-light to rebuild it\n");
                    bspdata.bspx.entries.erase("LIGHTGRID_OCTREE");
                } else {
                    auto &lump_bytes = bspdata.bspx.entries.at("LIGHTGRID_OCTREE");
                    ScaleLightgridOctreeHeader(lump_bytes, scalar);
                }
            }

            // LIGHTGRIDS contains a sequence of size-prefixed subgrids, not a
            // direct header. Rebuilding it correctly requires transforming and
            // possibly reordering every subgrid, so do not preserve stale data.
            if (bspdata.bspx.entries.erase("LIGHTGRIDS")) {
                logging::print("WARNING: removing LIGHTGRIDS after scaling; re-run vmt-light to rebuild it\n");
            }

            const fs::path output =
                current_write_path.parent_path() / (current_write_path.stem().string() + "-scaled.bsp");
            WriteBSPCopy(output, bspdata, current_write_version);
            current_write_path = output;

        } else if (operation->primary_name() == "replace-entities") {
            fs::path dest = operation->string_value();
            logging::print("updating with {}\n", dest);

            // Load the .ent
            if (std::holds_alternative<mbsp_t>(bspdata.bsp)) {
                fs::data ent = fs::load(dest);

                if (!ent) {
                    Error("couldn't load ent file {}", dest);
                }

                mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

                bsp.dentdata = std::string(reinterpret_cast<char *>(ent->data()), ent->size());

                WriteBSPCopy(current_write_path, bspdata, current_write_version);
            } else {
                map_file_t ents = LoadMapOrEntFile(dest);

                if (map_file.entities.empty()) {
                    FError("source map {} contains no entities", source);
                }
                if (ents.entities.empty()) {
                    FError("replacement entity file {} contains no entities", dest);
                }

                ents.entities[0].map_brushes = std::move(map_file.entities[0].map_brushes);

                // move brushes over from .map into the .ent
                for (int32_t i1 = 0, b = 1; i1 < map_file.entities.size(); i1++) {

                    // skip worldspawn though
                    if (map_file.entities[i1].map_brushes.empty() || i1 == 0) {
                        continue;
                    }

                    for (int32_t i2 = 0, b2 = 1; i2 < ents.entities.size(); i2++) {
                        if (ents.entities[i2].epairs.get("model").empty() &&
                            ents.entities[i2].epairs.get("classname") != "func_areaportal") {
                            continue;
                        }

                        if (b2 == b) {
                            ents.entities[i2].map_brushes = std::move(map_file.entities[i1].map_brushes);
                            b++;
                            break;
                        }

                        b2++;
                    }

                    if (!map_file.entities[i1].map_brushes.empty()) {
                        Error("ent files' map brushes don't match\n");
                    }
                }

                for (auto &ent : ents.entities) {
                    // remove origin key from brushed entities
                    if (!ent.map_brushes.empty() && ent.epairs.find("origin") != ent.epairs.end()) {
                        ent.epairs.remove("origin");
                    }

                    // remove style keys from areaportals and lights that
                    // have targetnames
                    if (ent.epairs.find("style") != ent.epairs.end()) {
                        if (ent.epairs.get("classname") == "light") {
                            if (ent.epairs.find("targetname") != ent.epairs.end()) {
                                ent.epairs.remove("style");
                            }
                        } else if (ent.epairs.get("classname") == "func_areaportal") {
                            ent.epairs.remove("style");
                        }
                    }
                }

                // write out .replaced.map
                fs::path output = fs::path(source).replace_extension(".replaced.map");
                std::ofstream strm(output, std::ios::binary);
                if (!strm) {
                    FError("couldn't open {} for writing", output);
                }

                for (const auto &ent : ents.entities) {
                    strm << "{\n";
                    for (const auto &epair : ent.epairs) {
                        ewt::print(strm, "\"{}\" \"{}\"\n", epair.first, epair.second);
                    }
                    if (!ent.map_brushes.empty()) {
                        strm << ent.map_brushes;
                    }
                    strm << "}\n";
                }
                if (!strm) {
                    FError("error writing {}", output);
                }
            }
        } else if (operation->primary_name() == "convert") {
            std::string format = operation->string_value();
            const bspversion_t *fmt = nullptr;

            for (const bspversion_t *bspver : bspversions) {
                if (string_iequals(format, bspver->short_name)) {
                    fmt = bspver;
                    break;
                }
            }

            if (!fmt) {
                Error("Unsupported format {}", format);
            }

            const fs::path output =
                current_write_path.parent_path() / (current_write_path.stem().string() + "-" + fmt->short_name);
            WriteBSPCopy(output, bspdata, fmt);
            current_write_path = output;
            current_write_version = fmt;
        } else if (operation->primary_name() == "extract-entities") {
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            size_t entity_data_size = bsp.dentdata.size();
            if (entity_data_size && bsp.dentdata.back() == '\0') {
                entity_data_size--;
            }
            if (entity_data_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
                FError("entity lump is too large to calculate its CRC");
            }
            uint32_t crc = CRC_Block(
                reinterpret_cast<const unsigned char *>(bsp.dentdata.data()), static_cast<int>(entity_data_size));

            fs::path output = fs::path(source).replace_extension(".ent");
            logging::print("-> writing {} [CRC: {:04x}]... ", output, crc);

            std::ofstream f(output, std::ios_base::out | std::ios_base::binary);
            if (!f)
                Error("couldn't open {} for writing\n", output);

            f << bsp.dentdata;

            if (!f)
                Error("{}", strerror(errno));

            f.close();
        } else if (operation->primary_name() == "extract-textures") {
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            fs::path output = fs::path(source).replace_extension(".wad");
            logging::print("-> writing {}... ", output);

            std::ofstream f(output, std::ios_base::binary);

            if (!f)
                Error("couldn't open {} for writing\n", output);

            ExportWad(f, &bsp);
            f.close();
            if (!f)
                Error("error writing {}\n", output);
        } else if (operation->primary_name() == "replace-textures") {
            fs::path wad_source = operation->string_value();

            if (auto wad = fs::addArchive(wad_source, false)) {
                logging::print("loaded wad file: {}\n", wad_source);

                mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);
                ReplaceTexturesFromWad(bsp);
                WriteBSPCopy(current_write_path, bspdata, current_write_version);
            } else {
                Error("couldn't load .wad file {}\n", wad_source);
            }
        } else if (operation->primary_name() == "check") {
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);
            CheckBSPFile(&bsp);
        } else if (operation->primary_name() == "modelinfo") {
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);
            PrintModelInfo(&bsp);
        } else if (operation->primary_name() == "findfaces") {
            auto setting = dynamic_cast<setting_combined *>(operation.get());
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            try {
                const qvec3d pos = setting->get<settings::setting_vec3>(0)->value();
                const qvec3d normal = setting->get<settings::setting_vec3>(1)->value();
                FindFaces(&bsp, pos, normal);
            } catch (const std::exception &) {
                Error("Error reading position/normal\n");
            }
        } else if (operation->primary_name() == "findleaf") {
            qvec3f pos = dynamic_cast<settings::setting_vec3 *>(operation.get())->value();
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            try {
                FindLeaf(&bsp, pos);
            } catch (const std::exception &) {
                Error("Error reading position/normal\n");
            }
        } else if (operation->primary_name() == "settexinfo") {
            auto setting = dynamic_cast<setting_combined *>(operation.get());
            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            const int fnum = setting->get<settings::setting_int32>(0)->value();
            const int texinfonum = setting->get<settings::setting_int32>(1)->value();

            if (fnum < 0 || static_cast<size_t>(fnum) >= bsp.dfaces.size()) {
                FError("face index {} is out of range (BSP has {} faces)", fnum, bsp.dfaces.size());
            }
            if (texinfonum < -1 || (texinfonum >= 0 && static_cast<size_t>(texinfonum) >= bsp.texinfo.size())) {
                FError("texinfo index {} is out of range (BSP has {} texinfos)", texinfonum, bsp.texinfo.size());
            }

            mface_t *face = &bsp.dfaces[static_cast<size_t>(fnum)];
            face->texinfo = texinfonum;

            // Overwrite the current mutation target. After an output-producing
            // operation such as scale/convert, this is the derived BSP rather
            // than the original input.
            WriteBSPCopy(current_write_path, bspdata, current_write_version);
        } else if (operation->primary_name().starts_with("decompile")) {
            const bool geomOnly = operation->primary_name() == "decompile-geomonly";
            const bool ignoreBrushes = operation->primary_name() == "decompile-ignore-brushes";
            const bool hull = operation->primary_name() == "decompile-hull";

            int hullnum = 0;
            if (hull) {
                hullnum = dynamic_cast<settings::setting_int32 *>(operation.get())->value();
            }

            if (hullnum < 0) {
                FError("hull index must not be negative");
            }

            // generate output filename
            fs::path output = source;
            if (hull) {
                output.replace_extension(fmt::format(".decompile.hull{}.map", hullnum));
            } else {
                output.replace_extension(".decompile.map");
            }

            logging::print("-> writing {}...\n", output);

            std::ofstream f(output);

            if (!f)
                Error("couldn't open {} for writing\n", output);

            mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

            decomp_options options;
            options.geometryOnly = geomOnly;
            options.ignoreBrushes = ignoreBrushes;
            options.hullnum = hullnum;

            DecompileBSP(&bsp, options, f);

            f.close();

            if (!f)
                Error("{}", strerror(errno));
        } else if (operation->primary_name() == "extract-bspx-lump") {
            auto setting = dynamic_cast<setting_combined *>(operation.get());
            std::string lump_name = setting->get<settings::setting_string>(0)->value();
            fs::path output_file_name = setting->get<settings::setting_string>(1)->value();

            if (lump_name.empty() || lump_name.size() >= bspx_lump_t{}.lumpname.size()) {
                FError("BSPX lump names must contain between 1 and 23 characters");
            }

            const auto &entries = bspdata.bspx.entries;
            if (entries.find(lump_name) == entries.end()) {
                FError("couldn't find bspx lump {}", lump_name);
            }

            const std::vector<uint8_t> &entry = entries.at(lump_name);

            logging::print("-> writing {} BSPX lump data to {}... ", lump_name, output_file_name);
            std::ofstream f(output_file_name, std::ios_base::out | std::ios_base::binary);
            if (!f)
                FError("couldn't open {} for writing\n", output_file_name);

            if (entry.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
                FError("BSPX lump {} is too large to extract", lump_name);
            }
            f.write(reinterpret_cast<const char *>(entry.data()), static_cast<std::streamsize>(entry.size()));

            if (!f)
                FError("{}", strerror(errno));
            f.close();

            logging::print("done.\n");
        } else if (operation->primary_name() == "insert-bspx-lump") {
            auto setting = dynamic_cast<setting_combined *>(operation.get());
            std::string lump_name = setting->get<settings::setting_string>(0)->value();
            fs::path input_file_name = setting->get<settings::setting_string>(1)->value();

            if (lump_name.empty() || lump_name.size() >= bspx_lump_t{}.lumpname.size()) {
                FError("BSPX lump names must contain between 1 and 23 characters");
            }

            // read entire input
            auto data = fs::load(input_file_name);
            if (!data)
                FError("couldn't open {} for reading\n", input_file_name);

            // put bspx lump
            logging::print("-> inserting BSPX lump {} from {} ({} bytes)...", lump_name, input_file_name, data->size());
            auto &entries = bspdata.bspx.entries;
            entries[lump_name] = std::move(*data);

            WriteBSPCopy(current_write_path, bspdata, current_write_version);

            logging::print("done.\n");
        } else if (operation->primary_name() == "remove-bspx-lump") {
            std::string lump_name = operation->string_value();

            if (lump_name.empty() || lump_name.size() >= bspx_lump_t{}.lumpname.size()) {
                FError("BSPX lump names must contain between 1 and 23 characters");
            }

            // remove bspx lump
            logging::print("-> removing bspx lump {}\n", lump_name);

            auto &entries = bspdata.bspx.entries;
            auto it = entries.find(lump_name);
            if (it == entries.end()) {
                FError("couldn't find bspx lump {}", lump_name);
            }
            entries.erase(it);

            WriteBSPCopy(current_write_path, bspdata, current_write_version);

            logging::print("done.\n");
        } else {
            Error("option not implemented: {}", operation->primary_name());
        }
    }

    return 0;
}

int bsputil_main(int argc, const char **argv)
{
    try {
        const int result = bsputil_main_impl(argc, argv);
        logging::close();
        if (result == 0) {
            logging::fail_if_warnings();
        }
        return result;
    } catch (...) {
        logging::close();
        throw;
    }
}
