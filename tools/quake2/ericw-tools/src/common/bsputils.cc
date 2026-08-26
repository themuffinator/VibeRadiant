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

#include <common/bsputils.hh>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <common/log.hh>
#include <common/qvec.hh>

const dmodelh2_t *BSP_GetWorldModel(const mbsp_t *bsp)
{
    // We only support .bsp's that have a world model
    if (!bsp || bsp->dmodels.empty()) {
        FError("BSP has no models");
    }
    return &bsp->dmodels[0];
}

int Face_GetNum(const mbsp_t *bsp, const mface_t *f)
{
    if (!bsp || !f || bsp->dfaces.empty()) {
        FError("Corrupt BSP: face pointer is out of bounds");
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(bsp->dfaces.data());
    const uintptr_t address = reinterpret_cast<uintptr_t>(f);
    const size_t byte_size = bsp->dfaces.size() * sizeof(mface_t);
    if (address < begin || address - begin >= byte_size || (address - begin) % sizeof(mface_t) != 0) {
        FError("Corrupt BSP: face pointer is out of bounds");
    }

    const size_t index = (address - begin) / sizeof(mface_t);
    if (index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        FError("Corrupt BSP: face index exceeds the supported range");
    }
    return static_cast<int>(index);
}

const bsp2_dnode_t *BSP_GetNode(const mbsp_t *bsp, int nodenum)
{
    if (!bsp || nodenum < 0 || static_cast<size_t>(nodenum) >= bsp->dnodes.size()) {
        FError("Corrupt BSP: node {} is out of bounds", nodenum);
    }
    return &bsp->dnodes[static_cast<size_t>(nodenum)];
}

const mleaf_t *BSP_GetLeaf(const mbsp_t *bsp, int leafnum)
{
    if (!bsp || leafnum < 0 || static_cast<size_t>(leafnum) >= bsp->dleafs.size()) {
        FError("Corrupt BSP: leaf {} is out of bounds", leafnum);
    }
    return &bsp->dleafs[static_cast<size_t>(leafnum)];
}

int BSP_GetLeafNum(const mbsp_t *bsp, const mleaf_t *leaf)
{
    if (!bsp || !leaf || bsp->dleafs.empty()) {
        FError("Corrupt BSP: leaf pointer is out of bounds");
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(bsp->dleafs.data());
    const uintptr_t address = reinterpret_cast<uintptr_t>(leaf);
    const size_t byte_size = bsp->dleafs.size() * sizeof(mleaf_t);
    if (address < begin || address - begin >= byte_size || (address - begin) % sizeof(mleaf_t) != 0) {
        FError("Corrupt BSP: leaf pointer is out of bounds");
    }

    const size_t index = (address - begin) / sizeof(mleaf_t);
    if (index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        FError("Corrupt BSP: leaf index exceeds the supported range");
    }
    return static_cast<int>(index);
}

const mleaf_t *BSP_GetLeafFromNodeNum(const mbsp_t *bsp, int nodenum)
{
    if (nodenum >= 0) {
        FError("Corrupt BSP: node reference {} does not encode a leaf", nodenum);
    }

    const int64_t leafnum = -1ll - static_cast<int64_t>(nodenum);
    if (leafnum > std::numeric_limits<int>::max()) {
        FError("Corrupt BSP: leaf reference {} is out of bounds", nodenum);
    }
    return BSP_GetLeaf(bsp, static_cast<int>(leafnum));
}

const dplane_t *BSP_GetPlane(const mbsp_t *bsp, int planenum)
{
    if (!bsp || planenum < 0 || static_cast<size_t>(planenum) >= bsp->dplanes.size()) {
        FError("Corrupt BSP: plane {} is out of bounds", planenum);
    }
    return &bsp->dplanes[static_cast<size_t>(planenum)];
}

const mface_t *BSP_GetFace(const mbsp_t *bsp, int fnum)
{
    if (!bsp || fnum < 0 || static_cast<size_t>(fnum) >= bsp->dfaces.size()) {
        FError("Corrupt BSP: face {} is out of bounds", fnum);
    }
    return &bsp->dfaces[static_cast<size_t>(fnum)];
}

const mtexinfo_t *BSP_GetTexinfo(const mbsp_t *bsp, int texinfo)
{
    if (!bsp || texinfo < 0) {
        return nullptr;
    }
    if (static_cast<size_t>(texinfo) >= bsp->texinfo.size()) {
        return nullptr;
    }
    const mtexinfo_t *tex = &bsp->texinfo[static_cast<size_t>(texinfo)];
    return tex;
}

mface_t *BSP_GetFace(mbsp_t *bsp, int fnum)
{
    return const_cast<mface_t *>(BSP_GetFace(const_cast<const mbsp_t *>(bsp), fnum));
}

static void ValidateFaceSurfedgeRange(const mbsp_t *bsp, const mface_t *face)
{
    if (!bsp || !face) {
        FError("Face edge lookup requires valid BSP and face objects");
    }
    if (face->firstedge < 0 || face->numedges < 0) {
        FError("Corrupt BSP: face has invalid surfedge range ({}, {})", face->firstedge, face->numedges);
    }

    const size_t firstedge = static_cast<size_t>(face->firstedge);
    const size_t numedges = static_cast<size_t>(face->numedges);
    if (firstedge > bsp->dsurfedges.size() || numedges > bsp->dsurfedges.size() - firstedge) {
        FError("Corrupt BSP: face surfedge range ({}, {}) exceeds {} entries", firstedge, numedges,
            bsp->dsurfedges.size());
    }
}

/* small helper that just retrieves the correct vertex from face->surfedge->edge lookups */
int Face_VertexAtIndex(const mbsp_t *bsp, const mface_t *f, int v)
{
    ValidateFaceSurfedgeRange(bsp, f);
    if (v < 0 || v >= f->numedges) {
        FError("Corrupt BSP: face vertex index {} is outside [0, {})", v, f->numedges);
    }

    const size_t surfedge_index = static_cast<size_t>(f->firstedge) + static_cast<size_t>(v);
    const int32_t surfedge = bsp->dsurfedges[surfedge_index];
    const uint64_t edge_index =
        surfedge < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(surfedge)) : static_cast<uint64_t>(surfedge);
    if (edge_index >= bsp->dedges.size()) {
        FError("Corrupt BSP: surfedge {} references edge {}, but only {} edges exist", surfedge_index, edge_index,
            bsp->dedges.size());
    }

    const uint32_t vertex = bsp->dedges[static_cast<size_t>(edge_index)][surfedge < 0 ? 1 : 0];
    if (static_cast<uint64_t>(vertex) >= static_cast<uint64_t>(bsp->dvertexes.size()) ||
        static_cast<uint64_t>(vertex) > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        FError("Corrupt BSP: edge {} references vertex {}, but only {} vertices exist", edge_index, vertex,
            bsp->dvertexes.size());
    }
    return static_cast<int>(vertex);
}

const qvec3f &Vertex_GetPos(const mbsp_t *bsp, int num)
{
    if (!bsp || num < 0 || static_cast<size_t>(num) >= bsp->dvertexes.size()) {
        FError("Corrupt BSP: vertex {} is out of bounds", num);
    }
    return bsp->dvertexes[static_cast<size_t>(num)];
}

const qvec3f &Face_PointAtIndex(const mbsp_t *bsp, const mface_t *f, int v)
{
    const int vertnum = Face_VertexAtIndex(bsp, f, v);
    return Vertex_GetPos(bsp, vertnum);
}

qvec3d Face_Normal(const mbsp_t *bsp, const mface_t *f)
{
    return Face_Plane(bsp, f).normal;
}

qplane3f Face_Plane(const mbsp_t *bsp, const mface_t *f)
{
    if (!bsp || !f || f->planenum < 0 || static_cast<size_t>(f->planenum) >= bsp->dplanes.size()) {
        FError("Corrupt BSP: face plane {} is out of bounds", f ? f->planenum : -1);
    }
    qplane3f result = bsp->dplanes[static_cast<size_t>(f->planenum)];

    if (f->side) {
        return -result;
    }

    return result;
}

const mtexinfo_t *Face_Texinfo(const mbsp_t *bsp, const mface_t *face)
{
    if (!bsp || !face || face->texinfo < 0 || static_cast<size_t>(face->texinfo) >= bsp->texinfo.size())
        return nullptr;

    return &bsp->texinfo[face->texinfo];
}

const miptex_t *Face_Miptex(const mbsp_t *bsp, const mface_t *face)
{
    if (!bsp || !face) {
        return nullptr;
    }

    // no miptex data (Q2 maps)
    if (!bsp->dtex.textures.size())
        return nullptr;

    const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);

    if (texinfo == nullptr)
        return nullptr;

    if (texinfo->miptex < 0 || static_cast<size_t>(texinfo->miptex) >= bsp->dtex.textures.size()) {
        return nullptr;
    }

    const miptex_t &miptex = bsp->dtex.textures[static_cast<size_t>(texinfo->miptex)];

    // sometimes the texture just wasn't written. including its name.
    if (miptex.name.empty())
        return nullptr;

    return &miptex;
}

std::string_view Face_TextureNameView(const mbsp_t *bsp, const mface_t *face)
{
    const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);
    if (!texinfo) {
        return {};
    }

    if (!texinfo->texturename.empty()) {
        return texinfo->texturename;
    }

    const miptex_t *miptex = Face_Miptex(bsp, face);
    return miptex ? std::string_view(miptex->name) : std::string_view{};
}

const char *Face_TextureName(const mbsp_t *bsp, const mface_t *face)
{
    const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);

    if (!texinfo) {
        return "";
    }

    // Q2 has texture written directly here
    if (!texinfo->texturename.empty()) {
        return texinfo->texturename.c_str();
    }

    // Q1 has it on the miptex
    const auto *miptex = Face_Miptex(bsp, face);

    if (miptex) {
        return miptex->name.c_str();
    }

    return "";
}

const qvec3f &GetSurfaceVertexPoint(const mbsp_t *bsp, const mface_t *f, int v)
{
    return bsp->dvertexes[Face_VertexAtIndex(bsp, f, v)];
}

static int TextureName_Contents(const gamedef_t *game, const char *texname)
{
    if (!Q_strncasecmp(texname, "sky", 3))
        return CONTENTS_SKY;
    else if (texname[0] == '*') // don't check liquids if not prefixed as such
    {
        if (!Q_strncasecmp(texname, "*lava", 5))
            return CONTENTS_LAVA;
        else if (!Q_strncasecmp(texname, "*slime", 6))
            return CONTENTS_SLIME;
        else
            return CONTENTS_WATER;
    } else if (texname[0] == '!' && game->allows_hl_contents) // don't check liquids if not prefixed as such
    {
        if (!Q_strncasecmp(texname, "!lava", 5))
            return CONTENTS_LAVA;
        else if (!Q_strncasecmp(texname, "!slime", 6))
            return CONTENTS_SLIME;
        else
            return CONTENTS_WATER;
    }
    return CONTENTS_SOLID;
}

// FIXME: name is misleading since we return true for opaque Q2 liquids
bool // mxd
ContentsOrSurfaceFlags_IsTranslucent(const mbsp_t *bsp, const int contents_or_surf_flags)
{
    if (!bsp || !bsp->loadversion || !bsp->loadversion->game) {
        FError("Contents query requires a valid BSP version");
    }
    if (bsp->loadversion->game->id == GAME_QUAKE_II)
        return (contents_or_surf_flags & (Q2_SURF_TRANS33 | Q2_SURF_TRANS66 | Q2_SURF_WARP));
    else
        return contents_or_surf_flags == CONTENTS_WATER || contents_or_surf_flags == CONTENTS_LAVA ||
               contents_or_surf_flags == CONTENTS_SLIME;
}

bool // mxd. Moved here from ltface.c (was Face_IsLiquid)
Face_IsTranslucent(const mbsp_t *bsp, const mface_t *face)
{
    return ContentsOrSurfaceFlags_IsTranslucent(bsp, Face_ContentsOrSurfaceFlags(bsp, face));
}

int // mxd. Returns CONTENTS_ value for Q1, Q2_SURF_ bitflags for Q2...
Face_ContentsOrSurfaceFlags(const mbsp_t *bsp, const mface_t *face)
{
    if (!bsp || !bsp->loadversion || !bsp->loadversion->game || !face) {
        FError("Face contents query requires valid BSP and face objects");
    }
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        const mtexinfo_t *info = Face_Texinfo(bsp, face);
        if (!info) {
            FError("Corrupt BSP: face texture info is out of bounds");
        }
        return info->flags.native_q2;
    } else {
        return TextureName_Contents(bsp->loadversion->game, Face_TextureName(bsp, face));
    }
}

const dmodelh2_t *BSP_DModelForModelString(const mbsp_t *bsp, const std::string &submodel_str)
{
    if (!bsp || submodel_str.size() < 2 || submodel_str.front() != '*') {
        return nullptr;
    }

    int submodel = -1;
    const char *const first = submodel_str.data() + 1;
    const char *const last = submodel_str.data() + submodel_str.size();
    const auto [end, error] = std::from_chars(first, last, submodel);
    if (error != std::errc{} || end != last || submodel < 0 || static_cast<size_t>(submodel) >= bsp->dmodels.size()) {
        return nullptr;
    }

    return &bsp->dmodels[static_cast<size_t>(submodel)];
}

// Tests hull 0 of the given model
bool Light_PointInSolid(
    const mbsp_t *bsp, const dmodelh2_t *model, const std::vector<contentflags_t> &extended_flags, const qvec3d &point)
{
    if (!bsp || !model) {
        FError("Light point query requires valid BSP and model objects");
    }

    std::vector<int> pending{model->headnode[0]};
    size_t visited_nodes = 0;
    while (!pending.empty()) {
        const int nodenum = pending.back();
        pending.pop_back();

        if (nodenum < 0) {
            const int leafnum = BSP_GetLeafNum(bsp, BSP_GetLeafFromNodeNum(bsp, nodenum));
            if (static_cast<size_t>(leafnum) >= extended_flags.size()) {
                FError("Corrupt BSP: leaf {} has no extended contents entry", leafnum);
            }

            const auto contentflags = extended_flags[static_cast<size_t>(leafnum)];
            // These are solids for this test (luxels can't be put inside).
            if (contentflags.flags & (EWT_VISCONTENTS_SOLID | EWT_VISCONTENTS_DETAIL_WALL | EWT_VISCONTENTS_SKY)) {
                return true;
            }
            continue;
        }

        if (++visited_nodes > bsp->dnodes.size()) {
            FError("Corrupt BSP: hull 0 node tree contains a cycle or shared node");
        }

        const bsp2_dnode_t *node = BSP_GetNode(bsp, nodenum);
        const double dist = BSP_GetPlane(bsp, node->planenum)->distance_to_fast(point);
        if (!std::isfinite(dist)) {
            FError("Corrupt BSP: non-finite node-plane distance at node {}", nodenum);
        }

        if (dist > 0.1) {
            pending.push_back(node->children[SIDE_FRONT]);
        } else if (dist < -0.1) {
            pending.push_back(node->children[SIDE_BACK]);
        } else {
            // Too close to the plane: inspect both sides without recursion.
            pending.push_back(node->children[SIDE_BACK]);
            pending.push_back(node->children[SIDE_FRONT]);
        }
    }

    return false;
}

bool Light_PointInWorld(const mbsp_t *bsp, const std::vector<contentflags_t> &extended_flags, const qvec3d &point)
{
    return Light_PointInSolid(bsp, BSP_GetWorldModel(bsp), extended_flags, point);
}

static std::vector<qplane3d> Face_AllocInwardFacingEdgePlanes(const mbsp_t *bsp, const mface_t *face)
{
    ValidateFaceSurfedgeRange(bsp, face);
    if (face->numedges < 3) {
        FError("Corrupt BSP: face has fewer than three edges");
    }

    std::vector<qplane3d> out;
    out.reserve(static_cast<size_t>(face->numedges));

    const qplane3f faceplane = Face_Plane(bsp, face);
    const qvec3d face_normal = faceplane.normal;
    const double face_normal_length_squared = qv::length2(face_normal);
    if (!std::isfinite(face_normal_length_squared) || face_normal_length_squared <= 0.0) {
        FError("Corrupt BSP: face has a zero or non-finite plane normal");
    }

    for (int i = 0; i < face->numedges; i++) {
        const qvec3f &v0 = GetSurfaceVertexPoint(bsp, face, i);
        const qvec3f &v1 = GetSurfaceVertexPoint(bsp, face, (i + 1) % face->numedges);

        const qvec3d start = v0;
        const qvec3d edge = qvec3d(v1) - start;
        const double edge_length_squared = qv::length2(edge);
        if (!std::isfinite(edge_length_squared) || edge_length_squared <= 0.0) {
            FError("Corrupt BSP: face edge {} is zero-length or non-finite", i);
        }

        const qvec3d edgevec = edge / std::sqrt(edge_length_squared);
        const qvec3d normal = qv::cross(edgevec, face_normal);
        const double normal_length_squared = qv::length2(normal);
        const double distance = qv::dot(normal, start);
        if (!std::isfinite(normal_length_squared) || normal_length_squared <= 0.0 || !std::isfinite(distance)) {
            FError("Corrupt BSP: face edge {} cannot form a finite inward plane", i);
        }

        out.emplace_back(normal, distance);
    }

    return out;
}

static bool EdgePlanes_PointInside(const std::vector<qplane3d> &edgeplanes, const qvec3d &point)
{
    for (auto &plane : edgeplanes) {
        if (plane.distance_to(point) < 0) {
            return false;
        }
    }
    return true;
}

/**
 * pass 0,0,0 for wantedNormal to disable the normal check
 */
static void BSP_FindFacesAtPoint_r(const mbsp_t *bsp, const int root_nodenum, const qvec3d &point,
    const qvec3d &wantedNormal, std::vector<const mface_t *> &result)
{
    std::vector<int> pending{root_nodenum};
    size_t visited_nodes = 0;
    while (!pending.empty()) {
        const int nodenum = pending.back();
        pending.pop_back();

        if (nodenum < 0) {
            // Faces are owned by nodes, but validate every reached leaf.
            BSP_GetLeafFromNodeNum(bsp, nodenum);
            continue;
        }

        if (++visited_nodes > bsp->dnodes.size()) {
            FError("Corrupt BSP: hull 0 node tree contains a cycle or shared node");
        }

        const bsp2_dnode_t *node = BSP_GetNode(bsp, nodenum);
        const double dist = BSP_GetPlane(bsp, node->planenum)->distance_to_fast(point);
        if (!std::isfinite(dist)) {
            FError("Corrupt BSP: non-finite node-plane distance at node {}", nodenum);
        }

        if (dist > 0.1) {
            pending.push_back(node->children[SIDE_FRONT]);
            continue;
        }
        if (dist < -0.1) {
            pending.push_back(node->children[SIDE_BACK]);
            continue;
        }

        // Point is close to this node plane. Check all faces on the plane.
        const size_t first_face = static_cast<size_t>(node->firstface);
        const size_t num_faces = static_cast<size_t>(node->numfaces);
        if (first_face > bsp->dfaces.size() || num_faces > bsp->dfaces.size() - first_face) {
            FError("Corrupt BSP: node {} face range ({}, {}) is out of bounds", nodenum, first_face, num_faces);
        }
        for (size_t i = 0; i < num_faces; i++) {
            const mface_t *face = &bsp->dfaces[first_face + i];
            // First check if it's facing the right way.
            const qvec3d faceNormal = Face_Normal(bsp, face);

            if (wantedNormal != qvec3d{0, 0, 0} && qv::dot(faceNormal, wantedNormal) < 0) {
                continue;
            }

            // Next test if it's within the boundaries of the face.
            const auto edgeplanes = Face_AllocInwardFacingEdgePlanes(bsp, face);

            if (EdgePlanes_PointInside(edgeplanes, point)) {
                result.push_back(face);
            }
        }

        // Push back first so front retains the original depth-first order.
        pending.push_back(node->children[SIDE_BACK]);
        pending.push_back(node->children[SIDE_FRONT]);
    }
}

std::vector<const mface_t *> BSP_FindFacesAtPoint(
    const mbsp_t *bsp, const dmodelh2_t *model, const qvec3d &point, const qvec3d &wantedNormal)
{
    if (!bsp || !model) {
        FError("Face point query requires valid BSP and model objects");
    }

    std::vector<const mface_t *> result;
    BSP_FindFacesAtPoint_r(bsp, model->headnode[0], point, wantedNormal, result);
    return result;
}

const mface_t *BSP_FindFaceAtPoint(
    const mbsp_t *bsp, const dmodelh2_t *model, const qvec3d &point, const qvec3d &wantedNormal)
{
    if (!bsp || !model) {
        FError("Face point query requires valid BSP and model objects");
    }

    std::vector<const mface_t *> result;
    BSP_FindFacesAtPoint_r(bsp, model->headnode[0], point, wantedNormal, result);

    if (result.empty()) {
        return nullptr;
    }
    return result[0];
}

const bsp2_dnode_t *BSP_FindNodeAtPoint(
    const mbsp_t *bsp, const dmodelh2_t *model, const qvec3d &point, const qvec3d &wanted_normal)
{
    if (!bsp || !model) {
        FError("Node point query requires valid BSP and model objects");
    }

    std::vector<int> pending{model->headnode[0]};
    size_t visited_nodes = 0;
    while (!pending.empty()) {
        const int nodenum = pending.back();
        pending.pop_back();

        if (nodenum < 0) {
            BSP_GetLeafFromNodeNum(bsp, nodenum);
            continue;
        }

        if (++visited_nodes > bsp->dnodes.size()) {
            FError("Corrupt BSP: hull 0 node tree contains a cycle or shared node");
        }

        const bsp2_dnode_t *node = BSP_GetNode(bsp, nodenum);
        const dplane_t *plane = BSP_GetPlane(bsp, node->planenum);
        const double dist = plane->distance_to_fast(point);
        if (!std::isfinite(dist)) {
            FError("Corrupt BSP: non-finite node-plane distance at node {}", nodenum);
        }

        if (dist > 0.1) {
            pending.push_back(node->children[SIDE_FRONT]);
            continue;
        }
        if (dist < -0.1) {
            pending.push_back(node->children[SIDE_BACK]);
            continue;
        }

        if (qv::epsilonEqual(1.0, fabs(qv::dot(plane->normal, wanted_normal)), 0.01)) {
            return node;
        }

        // Push back first so front retains the original depth-first order.
        pending.push_back(node->children[SIDE_BACK]);
        pending.push_back(node->children[SIDE_FRONT]);
    }

    return nullptr;
}

static const mleaf_t *BSP_FindLeafAtPoint_r(const mbsp_t *bsp, const int nodenum, const qvec3d &point)
{
    int current = nodenum;
    size_t visited_nodes = 0;
    while (current >= 0) {
        if (++visited_nodes > bsp->dnodes.size()) {
            FError("Corrupt BSP: hull 0 node tree contains a cycle");
        }

        const bsp2_dnode_t *node = BSP_GetNode(bsp, current);
        const double dist = BSP_GetPlane(bsp, node->planenum)->distance_to_fast(point);
        if (!std::isfinite(dist)) {
            FError("Corrupt BSP: non-finite node-plane distance at node {}", current);
        }

        current = node->children[dist >= 0 ? SIDE_FRONT : SIDE_BACK];
    }

    return BSP_GetLeafFromNodeNum(bsp, current);
}

const mleaf_t *BSP_FindLeafAtPoint(const mbsp_t *bsp, const dmodelh2_t *model, const qvec3d &point)
{
    if (!bsp || !model) {
        FError("Leaf point query requires valid BSP and model objects");
    }
    return BSP_FindLeafAtPoint_r(bsp, model->headnode[0], point);
}

static clipnode_info_t BSP_FindClipnodeAtPoint_r(const mbsp_t *bsp, const int parent_clipnodenum,
    const planeside_t parent_side, const int clipnodenum, const qvec3d &point)
{
    int current = clipnodenum;
    int parent = parent_clipnodenum;
    planeside_t side = parent_side;
    size_t visited_nodes = 0;

    while (current >= 0) {
        if (static_cast<size_t>(current) >= bsp->dclipnodes.size()) {
            FError("Corrupt BSP: clipnode {} is out of bounds", current);
        }
        if (++visited_nodes > bsp->dclipnodes.size()) {
            FError("Corrupt BSP: clipnode tree contains a cycle");
        }

        const auto *node = &bsp->dclipnodes[static_cast<size_t>(current)];
        const double dist = BSP_GetPlane(bsp, node->planenum)->distance_to_fast(point);
        if (!std::isfinite(dist)) {
            FError("Corrupt BSP: non-finite clipnode-plane distance at clipnode {}", current);
        }

        parent = current;
        side = dist >= 0 ? SIDE_FRONT : SIDE_BACK;
        current = node->children[side];
    }

    return {.parent_clipnode = parent, .side = side, .contents = current};
}
clipnode_info_t BSP_FindClipnodeAtPoint(
    const mbsp_t *bsp, hull_index_t hullnum, const dmodelh2_t *model, const qvec3d &point)
{
    if (!bsp || !model || !hullnum || *hullnum == 0 || static_cast<size_t>(*hullnum) >= model->headnode.size()) {
        FError("Clipnode point query requires a valid non-zero hull and model");
    }
    return BSP_FindClipnodeAtPoint_r(
        bsp, 0, static_cast<planeside_t>(-1), model->headnode[static_cast<size_t>(*hullnum)], point);
}

int BSP_FindContentsAtPoint(const mbsp_t *bsp, hull_index_t hullnum, const dmodelh2_t *model, const qvec3d &point)
{
    if (!bsp || !model) {
        FError("Contents point query requires valid BSP and model objects");
    }

    const size_t hull = static_cast<size_t>(hullnum.value_or(0));
    if (hull >= model->headnode.size()) {
        FError("Contents point query hull {} is out of bounds", hull);
    }
    if (hull == 0) {
        return BSP_FindLeafAtPoint_r(bsp, model->headnode[0], point)->contents;
    }
    const auto info = BSP_FindClipnodeAtPoint_r(bsp, 0, static_cast<planeside_t>(-1), model->headnode[hull], point);
    return info.contents;
}

std::vector<const mface_t *> Leaf_Markfaces(const mbsp_t *bsp, const mleaf_t *leaf)
{
    BSP_GetLeafNum(bsp, leaf);
    const size_t first = static_cast<size_t>(leaf->firstmarksurface);
    const size_t count = static_cast<size_t>(leaf->nummarksurfaces);
    if (first > bsp->dleaffaces.size() || count > bsp->dleaffaces.size() - first) {
        FError("Corrupt BSP: leaf marksurface range ({}, {}) is out of bounds", first, count);
    }

    std::vector<const mface_t *> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const uint32_t face_index = bsp->dleaffaces[first + i];
        if (static_cast<size_t>(face_index) >= bsp->dfaces.size()) {
            FError("Corrupt BSP: leaf marksurface references face {} out of {}", face_index, bsp->dfaces.size());
        }
        result.push_back(&bsp->dfaces[static_cast<size_t>(face_index)]);
    }

    return result;
}

std::vector<const dbrush_t *> Leaf_Brushes(const mbsp_t *bsp, const mleaf_t *leaf)
{
    BSP_GetLeafNum(bsp, leaf);
    const size_t first = static_cast<size_t>(leaf->firstleafbrush);
    const size_t count = static_cast<size_t>(leaf->numleafbrushes);
    if (first > bsp->dleafbrushes.size() || count > bsp->dleafbrushes.size() - first) {
        FError("Corrupt BSP: leaf brush range ({}, {}) is out of bounds", first, count);
    }

    std::vector<const dbrush_t *> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const uint32_t brush_index = bsp->dleafbrushes[first + i];
        if (static_cast<size_t>(brush_index) >= bsp->dbrushes.size()) {
            FError("Corrupt BSP: leaf brush list references brush {} out of {}", brush_index, bsp->dbrushes.size());
        }
        result.push_back(&bsp->dbrushes[static_cast<size_t>(brush_index)]);
    }

    return result;
}

std::vector<qvec3f> Face_Points(const mbsp_t *bsp, const mface_t *face)
{
    ValidateFaceSurfedgeRange(bsp, face);
    std::vector<qvec3f> points;

    points.reserve(static_cast<size_t>(face->numedges));

    for (int j = 0; j < face->numedges; j++) {
        points.push_back(Face_PointAtIndex(bsp, face, j));
    }

    return points;
}

polylib::winding_t Face_Winding(const mbsp_t *bsp, const mface_t *face)
{
    ValidateFaceSurfedgeRange(bsp, face);
    polylib::winding_t w{};

    for (int j = 0; j < face->numedges; j++) {
        w.push_back(Face_PointAtIndex(bsp, face, j));
    }

    return w;
}

qvec3f Face_Centroid(const mbsp_t *bsp, const mface_t *face)
{
    auto points = Face_Points(bsp, face);
    return qv::PolyCentroid(points.begin(), points.end());
}

void Face_DebugPrint(const mbsp_t *bsp, const mface_t *face)
{
    ValidateFaceSurfedgeRange(bsp, face);
    const mtexinfo_t *tex = Face_Texinfo(bsp, face);
    if (!tex) {
        FError("Corrupt BSP: face texture info is out of bounds");
    }
    const char *texname = Face_TextureName(bsp, face);

    logging::print("face {}, texture '{}', {} edges; vectors:\n"
                   "{}\n",
        Face_GetNum(bsp, face), texname, face->numedges, tex->vecs);

    for (int i = 0; i < face->numedges; i++) {
        const int edge = bsp->dsurfedges[static_cast<size_t>(face->firstedge) + static_cast<size_t>(i)];
        const int vert = Face_VertexAtIndex(bsp, face, i);
        const qvec3f &point = GetSurfaceVertexPoint(bsp, face, i);
        logging::print("{} {:3} ({:3.3}, {:3.3}, {:3.3}) :: edge {}\n", i ? "          " : "    verts ", vert, point[0],
            point[1], point[2], edge);
    }
}

aabb3f Model_BoundsOfFaces(const mbsp_t &bsp, const dmodelh2_t &model)
{
    aabb3f result;
    for (int i = model.firstface; i < model.firstface + model.numfaces; ++i) {
        auto &face = bsp.dfaces[i];
        for (int j = 0; j < face.numedges; ++j) {
            result += Face_PointAtIndex(&bsp, &face, j);
        }
    }
    return result;
}

/*
===============
CompressRow
===============
*/
void CompressRow(const uint8_t *vis, const size_t numbytes, std::back_insert_iterator<std::vector<uint8_t>> it)
{
    for (size_t i = 0; i < numbytes; i++) {
        it++ = vis[i];

        if (vis[i]) {
            continue;
        }

        int32_t rep = 1;

        for (i++; i < numbytes; i++) {
            if (vis[i] || rep == 255) {
                break;
            }
            rep++;
        }

        it++ = rep;
        i--;
    }
}

size_t DecompressedVisSize(const mbsp_t *bsp)
{
    if (!bsp || !bsp->loadversion || !bsp->loadversion->game) {
        FError("visibility decompression requires a valid BSP version");
    }

    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        const size_t clusters = bsp->dvis.bit_offsets.size();
        return (clusters / 8) + (clusters % 8 != 0);
    }

    const dmodelh2_t *world = BSP_GetWorldModel(bsp);
    if (world->visleafs < 0) {
        FError("Corrupt BSP: world model has a negative visibility leaf count");
    }
    const size_t visleafs = static_cast<size_t>(world->visleafs);
    return (visleafs / 8) + (visleafs % 8 != 0);
}

int VisleafToLeafnum(int visleaf)
{
    return visleaf + 1;
}

int LeafnumToVisleaf(int leafnum)
{
    return leafnum - 1;
}

// returns true if pvs can see leaf
bool Pvs_LeafVisible(const mbsp_t *bsp, const std::vector<uint8_t> &pvs, const mleaf_t *leaf)
{
    if (!bsp || !bsp->loadversion || !bsp->loadversion->game || !leaf) {
        FError("PVS query requires valid BSP and leaf objects");
    }
    const int leafnum = BSP_GetLeafNum(bsp, leaf);

    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        if (leaf->cluster < 0) {
            return false;
        }

        const size_t cluster = static_cast<size_t>(leaf->cluster);
        if (cluster >= bsp->dvis.bit_offsets.size()) {
            logging::print("Pvs_LeafVisible: invalid visofs for cluster {}\n", leaf->cluster);
            return false;
        }
        const int32_t offset = bsp->dvis.get_bit_offset(VIS_PVS, cluster);
        if (offset < 0 || static_cast<size_t>(offset) >= bsp->dvis.bits.size() || cluster / 8 >= pvs.size()) {
            logging::print("Pvs_LeafVisible: invalid visibility data for cluster {}\n", leaf->cluster);
            return false;
        }

        return !!(pvs[cluster >> 3] & nth_bit(cluster & 7));
    } else {
        const int visleaf = LeafnumToVisleaf(leafnum);

        if (leafnum == 0) {
            // can't see into the shared solid leaf
            return false;
        }
        const dmodelh2_t *world = BSP_GetWorldModel(bsp);
        if (visleaf < 0 || visleaf >= world->visleafs || static_cast<size_t>(visleaf) / 8 >= pvs.size()) {
            logging::print("Pvs_LeafVisible: invalid visibility data for leaf {}\n", leafnum);
            return false;
        }

        return !!(pvs[static_cast<size_t>(visleaf) >> 3] & nth_bit(static_cast<size_t>(visleaf) & 7));
    }
}

// from DarkPlaces (Mod_Q1BSP_DecompressVis)
bool DecompressVis(const uint8_t *in, const uint8_t *inend, uint8_t *out, uint8_t *outend)
{
    int c;
    uint8_t *outstart = out;
    while (out < outend) {
        if (in == inend) {
            logging::print("DecompressVis: input underrun (decompressed {} of {} output bytes)\n", (out - outstart),
                (outend - outstart));
            return false;
        }

        c = *in++;
        if (c) {
            *out++ = c;
            continue;
        }

        if (in == inend) {
            logging::print("DecompressVis: input underrun (during zero-run) (decompressed {} of {} output bytes)\n",
                (out - outstart), (outend - outstart));
            return false;
        }

        const int run_length = *in++;
        if (!run_length) {
            logging::print("DecompressVis: 0 repeat\n");
            return false;
        }

        for (c = run_length; c > 0; c--) {
            if (out == outend) {
                logging::print("DecompressVis: output overrun (decompressed {} of {} output bytes)\n", (out - outstart),
                    (outend - outstart));
                return false;
            }
            *out++ = 0;
        }
    }
    return true;
}

/**
 * Decompress visdata for the entire map, and returns a map of:
 *
 *  - Q2: cluster number to decompressed visdata
 *  - Q1/others: visofs to decompressed visdata
 *
 * Q1 uses visofs as the map key, rather than e.g. visleaf number or leaf number, because if func_detail is in use,
 * many leafs will share the same visofs. This avoids storing the same visdata redundantly.
 */
std::unordered_map<int, std::vector<uint8_t>> DecompressAllVis(const mbsp_t *bsp, bool trans_water)
{
    std::unordered_map<int, std::vector<uint8_t>> result;

    const size_t decompressed_size = DecompressedVisSize(bsp);

    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        if (bsp->dvis.bit_offsets.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            FError("visibility cluster count {} exceeds the supported range", bsp->dvis.bit_offsets.size());
        }

        for (size_t cluster = 0; cluster < bsp->dvis.bit_offsets.size(); ++cluster) {
            const int32_t offset = bsp->dvis.get_bit_offset(VIS_PVS, cluster);
            if (offset < 0 || static_cast<size_t>(offset) >= bsp->dvis.bits.size()) {
                logging::print("DecompressAllVis: invalid visofs for cluster {}\n", cluster);
                continue;
            }

            std::vector<uint8_t> decompressed(decompressed_size);
            if (!decompressed.empty()) {
                if (!DecompressVis(bsp->dvis.bits.data() + static_cast<size_t>(offset),
                        bsp->dvis.bits.data() + bsp->dvis.bits.size(), decompressed.data(),
                        decompressed.data() + decompressed.size())) {
                    continue;
                }
            }
            result[static_cast<int>(cluster)] = std::move(decompressed);
        }
    } else {
        for (size_t leafnum = 0; leafnum < bsp->dleafs.size(); ++leafnum) {
            const auto &leaf = bsp->dleafs[leafnum];
            if (leaf.visofs < 0) {
                continue;
            }

            const int map_key = leaf.visofs;

            if (result.find(map_key) != result.end()) {
                // already decompressed this cluster
                continue;
            }

            if (static_cast<size_t>(leaf.visofs) >= bsp->dvis.bits.size()) {
                logging::print("DecompressAllVis: invalid visofs for leaf {}\n", leafnum);
                continue;
            }

            std::vector<uint8_t> decompressed(decompressed_size);
            if (!decompressed.empty()) {
                if (!DecompressVis(bsp->dvis.bits.data() + static_cast<size_t>(leaf.visofs),
                        bsp->dvis.bits.data() + bsp->dvis.bits.size(), decompressed.data(),
                        decompressed.data() + decompressed.size())) {
                    continue;
                }
            }
            result[map_key] = std::move(decompressed);
        }
    }

    return result;
}

void BSP_VisitAllLeafs(const mbsp_t &bsp, const dmodelh2_t &model, const std::function<void(const mleaf_t &)> &visitor)
{
    if (!visitor) {
        FError("BSP leaf traversal requires a visitor");
    }

    std::vector<int> pending{model.headnode[0]};
    size_t visited_nodes = 0;
    while (!pending.empty()) {
        const int nodenum = pending.back();
        pending.pop_back();

        if (nodenum < 0) {
            visitor(*BSP_GetLeafFromNodeNum(&bsp, nodenum));
            continue;
        }
        if (++visited_nodes > bsp.dnodes.size()) {
            FError("Corrupt BSP: hull 0 node tree contains a cycle or shared node");
        }

        const bsp2_dnode_t *node = BSP_GetNode(&bsp, nodenum);
        const dplane_t *plane = BSP_GetPlane(&bsp, node->planenum);
        if (!std::isfinite(plane->normal[0]) || !std::isfinite(plane->normal[1]) || !std::isfinite(plane->normal[2]) ||
            !std::isfinite(plane->dist)) {
            FError("Corrupt BSP: node {} has a non-finite plane", nodenum);
        }
        pending.push_back(node->children[SIDE_BACK]);
        pending.push_back(node->children[SIDE_FRONT]);
    }
}

void BSPX_ValidateDecoupledLM(const bspx_decoupled_lm_perface &face, size_t face_num)
{
    for (size_t row = 0; row < 2; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            if (!std::isfinite(face.world_to_lm_space.at(row, column))) {
                FError("DECOUPLED_LM face {} has a non-finite world-to-lightmap matrix component at ({}, {})", face_num,
                    row, column);
            }
        }
    }
}

bspx_decoupled_lm_perface BSPX_DecoupledLM(const bspxentries_t &entries, int face_num)
{
    constexpr size_t serialized_face_size = (sizeof(uint16_t) * 2) + sizeof(int32_t) + (sizeof(float) * 8);
    const auto &lump_bytes = entries.at("DECOUPLED_LM");

    if (face_num < 0 || static_cast<size_t>(face_num) > std::numeric_limits<size_t>::max() / serialized_face_size) {
        FError("invalid DECOUPLED_LM face index {}", face_num);
    }

    const size_t offset = static_cast<size_t>(face_num) * serialized_face_size;
    if (offset > lump_bytes.size() || serialized_face_size > lump_bytes.size() - offset) {
        FError("DECOUPLED_LM face index {} is outside the lump", face_num);
    }

    auto stream = imemstream(lump_bytes.data() + offset, serialized_face_size);
    stream >> endianness<std::endian::little>;

    bspx_decoupled_lm_perface result{};
    stream >= result;
    if (!stream || stream.tellg() != static_cast<std::streamoff>(serialized_face_size)) {
        FError("malformed DECOUPLED_LM data for face {}", face_num);
    }
    BSPX_ValidateDecoupledLM(result, static_cast<size_t>(face_num));
    return result;
}

std::optional<bspxfacenormals> BSPX_FaceNormals(const mbsp_t &bsp, const bspxentries_t &entries)
{
    auto it = entries.find("FACENORMALS");
    if (it == entries.end()) {
        return std::nullopt;
    }
    if (it->second.empty()) {
        logging::print("WARNING: bad FACENORMALS lump\n");
        return std::nullopt;
    }

    auto stream = imemstream(it->second.data(), it->second.size());
    stream >> endianness<std::endian::little>;

    bspxfacenormals result;
    result.stream_read(stream, bsp);
    if (!stream || stream.tellg() != static_cast<std::streamoff>(it->second.size())) {
        logging::print("WARNING: bad FACENORMALS lump\n");
        return std::nullopt;
    }
    return result;
}

std::optional<lightgrid_octree_t> BSPX_LightgridOctree(const bspxentries_t &entries)
{
    auto it = entries.find("LIGHTGRID_OCTREE");
    if (it == entries.end()) {
        return std::nullopt;
    }
    if (it->second.empty()) {
        logging::print("WARNING: bad LIGHTGRID_OCTREE lump\n");
        return std::nullopt;
    }

    auto stream = imemstream(it->second.data(), it->second.size());
    stream >> endianness<std::endian::little>;

    lightgrid_octree_t result;
    stream >= result;

    if (!stream || stream.tellg() != static_cast<std::streamoff>(it->second.size())) {
        logging::print("WARNING: bad LIGHTGRID_OCTREE lump\n");
        return std::nullopt;
    }

    return result;
}

std::optional<lightgrids_t> BSPX_Lightgrids(const bspxentries_t &entries)
{
    auto it = entries.find("LIGHTGRIDS");
    if (it == entries.end()) {
        return std::nullopt;
    }
    if (it->second.empty()) {
        logging::print("WARNING: bad LIGHTGRIDS lump\n");
        return std::nullopt;
    }

    auto stream = imemstream(it->second.data(), it->second.size());
    stream >> endianness<std::endian::little>;

    lightgrids_t result;
    stream >= result;

    if (!stream || stream.tellg() != static_cast<std::streamoff>(it->second.size())) {
        logging::print("WARNING: bad LIGHTGRIDS lump\n");
        return std::nullopt;
    }

    return result;
}

qvec2d WorldToTexCoord(const qvec3d &world, const mtexinfo_t *tex)
{
    /*
     * The (long double) casts below are important: The original code
     * was written for x87 floating-point which uses 80-bit floats for
     * intermediate calculations. But if you compile it without the
     * casts for modern x86_64, the compiler will round each
     * intermediate result to a 32-bit float, which introduces extra
     * rounding error.
     *
     * This becomes a problem if the rounding error causes the light
     * utilities and the engine to disagree about the lightmap size
     * for some surfaces.
     *
     * Casting to (long double) keeps the intermediate values at at
     * least 64 bits of precision, probably 128.
     */
    return tex->vecs.uvs<long double>(world);
}

qvec2f Face_WorldToTexCoord(const mbsp_t *bsp, const mface_t *face, const qvec3f &world)
{
    const mtexinfo_t *tex = Face_Texinfo(bsp, face);

    if (tex == nullptr)
        return {};

    return WorldToTexCoord(world, tex);
}

qmat4x4f WorldToTexSpace(const mbsp_t *bsp, const mface_t *f)
{
    const mtexinfo_t *tex = Face_Texinfo(bsp, f);
    if (tex == nullptr) {
        Q_assert_unreachable();
        return qmat4x4f();
    }
    const qplane3f plane = Face_Plane(bsp, f);

    //           [s]
    // T * vec = [t]
    //           [distOffPlane]
    //           [?]

    qmat4x4f T{
        tex->vecs.at(0, 0), tex->vecs.at(1, 0), plane.normal[0], 0, // col 0
        tex->vecs.at(0, 1), tex->vecs.at(1, 1), plane.normal[1], 0, // col 1
        tex->vecs.at(0, 2), tex->vecs.at(1, 2), plane.normal[2], 0, // col 2
        tex->vecs.at(0, 3), tex->vecs.at(1, 3), -plane.dist, 1 // col 3
    };
    return T;
}

qmat4x4f TexSpaceToWorld(const mbsp_t *bsp, const mface_t *f)
{
    return qv::inverse(WorldToTexSpace(bsp, f));
}

// faceextents_t

faceextents_t::faceextents_t(const mface_t &face, const mbsp_t &bsp, float lightmapshift)
{
    worldToTexCoordMatrix = WorldToTexSpace(&bsp, &face);
    texCoordToWorldMatrix = TexSpaceToWorld(&bsp, &face);

    aabb2d tex_bounds;

    for (int i = 0; i < face.numedges; i++) {
        const qvec3f &worldpoint = Face_PointAtIndex(&bsp, &face, i);
        const qvec2f texcoord = Face_WorldToTexCoord(&bsp, &face, worldpoint);

#ifdef PARANOID
        // self test
        auto texcoordRT = this->worldToTexCoord(worldpoint);
        auto worldpointRT = this->texCoordToWorld(texcoord);
        Q_assert(qv::epsilonEqual(texcoordRT, texcoord, 0.1f));
        Q_assert(qv::epsilonEqual(worldpointRT, worldpoint, 0.1f));
        // end self test
#endif

        tex_bounds += texcoord;
        bounds += worldpoint;
    }

    qvec2i lm_mins;
    for (int i = 0; i < 2; i++) {
        tex_bounds[0][i] = floor(tex_bounds[0][i] / lightmapshift);
        tex_bounds[1][i] = ceil(tex_bounds[1][i] / lightmapshift);
        lm_mins[i] = static_cast<int>(tex_bounds[0][i]);
        lm_extents[i] = static_cast<int>(tex_bounds[1][i] - tex_bounds[0][i]);

        if (lm_extents[i] >= MAXDIMENSION * (16.0 / lightmapshift)) {
            const qplane3f plane = Face_Plane(&bsp, &face);
            const qvec3f &point = Face_PointAtIndex(&bsp, &face, 0); // grab first vert
            const char *texname = Face_TextureName(&bsp, &face);

            logging::print("WARNING: Bad surface extents (may not load in vanilla Q1 engines):\n"
                           "   surface {}, {} extents = {}, shift = {}\n"
                           "   texture {} at ({})\n"
                           "   surface normal ({})\n",
                Face_GetNum(&bsp, &face), i ? "t" : "s", lm_extents[i], lightmapshift, texname, point, plane.normal);
        }
    }

    // calculate a bounding sphere for the face
    qvec3d radius = (bounds.maxs() - bounds.mins()) * 0.5;

    origin = bounds.mins() + radius;
    this->radius = qv::length(radius);

    qmat4x4f LMToTexCoordMatrix = qmat4x4f::row_major({lightmapshift, 0, 0, lm_mins[0] * lightmapshift, 0,
        lightmapshift, 0, lm_mins[1] * lightmapshift, 0, 0, 1, 0, 0, 0, 0, 1});
    qmat4x4f TexCoordToLMMatrix = qv::inverse(LMToTexCoordMatrix);

    lmToWorldMatrix = texCoordToWorldMatrix * LMToTexCoordMatrix;
    worldToLMMatrix = TexCoordToLMMatrix * worldToTexCoordMatrix;
}

faceextents_t::faceextents_t(
    const mface_t &face, const mbsp_t &bsp, uint16_t lmwidth, uint16_t lmheight, texvecf world_to_lm_space)
{
    const qplane3f plane = Face_Plane(&bsp, &face);

    if (lmwidth > 0 && lmheight > 0) {
        lm_extents = {lmwidth - 1, lmheight - 1};
    }

    worldToTexCoordMatrix = WorldToTexSpace(&bsp, &face);
    texCoordToWorldMatrix = TexSpaceToWorld(&bsp, &face);

    worldToLMMatrix.set_row(0, world_to_lm_space.row(0));
    worldToLMMatrix.set_row(1, world_to_lm_space.row(1));
    worldToLMMatrix.set_row(2, qvec4f(plane.normal[0], plane.normal[1], plane.normal[2], -plane.dist));
    worldToLMMatrix.set_row(3, {0, 0, 0, 1});

    lmToWorldMatrix = qv::inverse(worldToLMMatrix);

    // bounds
    for (int i = 0; i < face.numedges; i++) {
        const qvec3f &worldpoint = Face_PointAtIndex(&bsp, &face, i);
        bounds += worldpoint;
    }

    // calculate a bounding sphere for the face
    qvec3d radius = (bounds.maxs() - bounds.mins()) * 0.5;

    origin = bounds.mins() + radius;
    this->radius = qv::length(radius);
}

faceextents_t::faceextents_t(
    const mface_t &face, const mbsp_t &bsp, world_units_per_luxel_t tag, float world_units_per_luxel)
{
    const qplane3f plane = Face_Plane(&bsp, &face);
    auto orig_normal = Face_Normal(&bsp, &face);
    size_t axis = qv::indexOfLargestMagnitudeComponent(orig_normal);

#if 0
    if (orig_normal == qvec3f(-1, 0, 0)) {
        logging::print("-x\n");
    }
#endif

    qvec3f snapped_normal{};
    if (orig_normal[axis] > 0) {
        snapped_normal[axis] = 1;
    } else {
        snapped_normal[axis] = -1;
    }

    auto [t, b] = qv::MakeTangentAndBitangentUnnormalized(snapped_normal);
    t = t * (1 / world_units_per_luxel);
    b = b * (1 / world_units_per_luxel);

    qmat<float, 2, 3> world_to_lm;
    world_to_lm.set_row(0, t);
    world_to_lm.set_row(1, b);

    aabb2f lm_bounds;
    for (int i = 0; i < face.numedges; i++) {
        const qvec3f &worldpoint = Face_PointAtIndex(&bsp, &face, i);
        const qvec2f lmcoord = world_to_lm * worldpoint;
        lm_bounds += lmcoord;
    }

    qvec2i lm_mins;
    for (int i = 0; i < 2; i++) {
        lm_bounds[0][i] = floor(lm_bounds[0][i]);
        lm_bounds[1][i] = ceil(lm_bounds[1][i]);
        lm_mins[i] = static_cast<int>(lm_bounds[0][i]);
        lm_extents[i] = static_cast<int>(lm_bounds[1][i] - lm_bounds[0][i]);
    }

    worldToLMMatrix.set_row(0, qvec4f(world_to_lm.row(0), -lm_mins[0]));
    worldToLMMatrix.set_row(1, qvec4f(world_to_lm.row(1), -lm_mins[1]));
    worldToLMMatrix.set_row(2, qvec4f(plane.normal[0], plane.normal[1], plane.normal[2], -plane.dist));
    worldToLMMatrix.set_row(3, qvec4f(0, 0, 0, 1));

    lmToWorldMatrix = qv::inverse(worldToLMMatrix);

    // world <-> tex conversions
    worldToTexCoordMatrix = WorldToTexSpace(&bsp, &face);
    texCoordToWorldMatrix = TexSpaceToWorld(&bsp, &face);

    // bounds
    for (int i = 0; i < face.numedges; i++) {
        const qvec3f &worldpoint = Face_PointAtIndex(&bsp, &face, i);
        bounds += worldpoint;

#if 0
        auto lm = worldToLMMatrix * qvec4f(worldpoint, 1.0f);

        logging::print("testing world {} -> lm {}\n",
            worldpoint,
            lm);
#endif
    }

    // calculate a bounding sphere for the face
    qvec3d radius = (bounds.maxs() - bounds.mins()) * 0.5;

    origin = bounds.mins() + radius;
    this->radius = qv::length(radius);
}

int faceextents_t::width() const
{
    return lm_extents[0] + 1;
}

int faceextents_t::height() const
{
    return lm_extents[1] + 1;
}

int faceextents_t::numsamples() const
{
    return width() * height();
}

qvec2i faceextents_t::lmsize() const
{
    return {width(), height()};
}

qvec2f faceextents_t::worldToTexCoord(qvec3f world) const
{
    const qvec4f worldPadded(world, 1.0f);
    const qvec4f res = worldToTexCoordMatrix * worldPadded;

    Q_assert(res[3] == 1.0f);

    return res;
}

qvec3f faceextents_t::texCoordToWorld(qvec2f tc) const
{
    const qvec4f tcPadded(tc[0], tc[1], 0.0f, 1.0f);
    const qvec4f res = texCoordToWorldMatrix * tcPadded;

    Q_assert(fabs(res[3] - 1.0f) < 0.01f);

    return res;
}

qvec2f faceextents_t::worldToLMCoord(qvec3f world) const
{
    const qvec4f worldPadded(world, 1.0f);
    const qvec4f res = worldToLMMatrix * worldPadded;
    return res;
}

qvec3f faceextents_t::LMCoordToWorld(qvec2f lm) const
{
    const qvec4f lmPadded(lm[0], lm[1], 0.0f, 1.0f);
    const qvec4f res = lmToWorldMatrix * lmPadded;
    return res;
}

static std::optional<size_t> lightmap_pixel_index(
    const mface_t *face, const faceextents_t &faceextents, qvec2i coord, int style, std::string_view description)
{
    const int64_t width_signed = static_cast<int64_t>(faceextents.lm_extents[0]) + 1;
    const int64_t height_signed = static_cast<int64_t>(faceextents.lm_extents[1]) + 1;
    if (width_signed <= 0 || height_signed <= 0) {
        FError("invalid {} lightmap dimensions {}x{}", description, width_signed, height_signed);
    }
    if (coord[0] < 0 || coord[1] < 0 || coord[0] >= width_signed || coord[1] >= height_signed) {
        FError("{} lightmap coordinate {} is outside {}x{}", description, coord, width_signed, height_signed);
    }

    const auto style_it = std::ranges::find(face->styles, style);
    if (style_it == face->styles.end()) {
        return std::nullopt;
    }

    const size_t width = static_cast<size_t>(width_signed);
    const size_t height = static_cast<size_t>(height_signed);
    if (width > std::numeric_limits<size_t>::max() / height) {
        FError("{} lightmap dimensions {}x{} overflow the sample count", description, width, height);
    }
    const size_t samples_per_style = width * height;
    const size_t style_index = static_cast<size_t>(style_it - face->styles.begin());
    if (style_index > std::numeric_limits<size_t>::max() / samples_per_style) {
        FError("{} lightmap style offset overflow", description);
    }
    const size_t style_offset = style_index * samples_per_style;
    const size_t coordinate_offset = static_cast<size_t>(coord[1]) * width + static_cast<size_t>(coord[0]);
    if (style_offset > std::numeric_limits<size_t>::max() - coordinate_offset) {
        FError("{} lightmap pixel offset overflow", description);
    }

    return style_offset + coordinate_offset;
}

/**
 * Samples the lightmap at an integer coordinate in the given style
 */
qvec3b LM_Sample(const mbsp_t *bsp, const mface_t *face, const lit_variant_t *lit, const faceextents_t &faceextents,
    int byte_offset_of_face, qvec2i coord, int style)
{
    if (byte_offset_of_face == -1) {
        return {0, 0, 0};
    }

    const auto pixel = lightmap_pixel_index(face, faceextents, coord, style, "LDR");
    if (!pixel) {
        return {0, 0, 0};
    }

    if (byte_offset_of_face < 0) {
        FError("invalid negative LDR lightmap offset {}", byte_offset_of_face);
    }
    const size_t face_offset = static_cast<size_t>(byte_offset_of_face);

    if (lit && std::holds_alternative<lit1_t>(*lit)) {
        const auto &lit_data = std::get<lit1_t>(*lit).rgbdata;
        if (face_offset > std::numeric_limits<size_t>::max() / 3 || *pixel > std::numeric_limits<size_t>::max() / 3) {
            FError("RGB .lit sample offset overflow");
        }
        const size_t lit_face_offset = face_offset * 3;
        const size_t pixel_offset = *pixel * 3;
        if (lit_face_offset > std::numeric_limits<size_t>::max() - pixel_offset) {
            FError("RGB .lit sample offset overflow");
        }
        const size_t sample_offset = lit_face_offset + pixel_offset;
        if (sample_offset > lit_data.size() || 3 > lit_data.size() - sample_offset) {
            FError("RGB .lit sample {} is outside the file ({} bytes)", *pixel, lit_data.size());
        }

        return {lit_data[sample_offset], lit_data[sample_offset + 1], lit_data[sample_offset + 2]};
    } else if (bsp->loadversion->game->has_rgb_lightmap) {
        if (*pixel > std::numeric_limits<size_t>::max() / 3) {
            FError("native RGB lightmap sample offset overflow");
        }
        const size_t pixel_offset = *pixel * 3;
        if (face_offset > std::numeric_limits<size_t>::max() - pixel_offset) {
            FError("native RGB lightmap sample offset overflow");
        }
        const size_t sample_offset = face_offset + pixel_offset;
        if (sample_offset > bsp->dlightdata.size() || 3 > bsp->dlightdata.size() - sample_offset) {
            FError("native RGB lightmap sample {} is outside the lump ({} bytes)", *pixel, bsp->dlightdata.size());
        }

        return {bsp->dlightdata[sample_offset], bsp->dlightdata[sample_offset + 1], bsp->dlightdata[sample_offset + 2]};
    } else if (!lit || std::holds_alternative<lit_none>(*lit)) {
        if (face_offset > std::numeric_limits<size_t>::max() - *pixel) {
            FError("native grayscale lightmap sample offset overflow");
        }
        const size_t sample_offset = face_offset + *pixel;
        if (sample_offset >= bsp->dlightdata.size()) {
            FError(
                "native grayscale lightmap sample {} is outside the lump ({} bytes)", *pixel, bsp->dlightdata.size());
        }

        const uint8_t sample = bsp->dlightdata[sample_offset];
        return {sample, sample, sample};
    } else {
        FError("LM_Sample requires RGB or no .lit data");
    }
}

qvec3f LM_Sample_HDR(const mbsp_t *bsp, const mface_t *face, const faceextents_t &faceextents, int byte_offset_of_face,
    qvec2i coord, const lit_variant_t *lit, const bspxentries_t *bspx)
{
    if (byte_offset_of_face == -1) {
        return {0, 0, 0};
    }

    const auto pixel_index = lightmap_pixel_index(face, faceextents, coord, 0, "HDR");
    if (!pixel_index) {
        return {0, 0, 0};
    }

    if (byte_offset_of_face < 0) {
        FError("invalid negative HDR lightmap offset {}", byte_offset_of_face);
    }
    if (bsp->loadversion->game->has_rgb_lightmap && byte_offset_of_face % 3 != 0) {
        FError("RGB HDR lightmap offset {} is not sample-aligned", byte_offset_of_face);
    }

    const size_t sample_offset_of_face = bsp->loadversion->game->has_rgb_lightmap
                                             ? static_cast<size_t>(byte_offset_of_face) / 3
                                             : static_cast<size_t>(byte_offset_of_face);
    if (sample_offset_of_face > std::numeric_limits<size_t>::max() - *pixel_index) {
        FError("HDR lightmap sample index overflow");
    }
    const size_t sample_index = sample_offset_of_face + *pixel_index;

    std::optional<uint32_t> packed_sample;
    if (lit && std::holds_alternative<lit_hdr>(*lit)) {
        const auto &samples = std::get<lit_hdr>(*lit).samples;
        if (sample_index >= samples.size()) {
            FError("HDR .lit sample {} is outside the file ({} samples)", sample_index, samples.size());
        }
        packed_sample = samples[sample_index];
    } else if (bspx) {
        if (auto it = bspx->find("LIGHTING_E5BGR9"); it != bspx->end()) {
            constexpr size_t packed_sample_size = sizeof(uint32_t);
            if (sample_index > std::numeric_limits<size_t>::max() / packed_sample_size) {
                FError("BSPX HDR lightmap sample index overflow");
            }

            const size_t byte_offset = sample_index * packed_sample_size;
            const auto &bytes = it->second;
            if (byte_offset > bytes.size() || packed_sample_size > bytes.size() - byte_offset) {
                FError("BSPX HDR lightmap sample {} is outside the lump ({} bytes)", sample_index, bytes.size());
            }

            packed_sample = static_cast<uint32_t>(bytes[byte_offset]) |
                            (static_cast<uint32_t>(bytes[byte_offset + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[byte_offset + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[byte_offset + 3]) << 24);
        }
    }

    if (!packed_sample) {
        throw std::runtime_error("LM_Sample_HDR requires either an HDR .lit file or BSPX lump");
    }

    return HDR_UnpackE5BRG9(*packed_sample);
}

std::map<int, std::vector<int>> ClusterToLeafnumsMap(const mbsp_t *bsp)
{
    if (!bsp) {
        FError("Cluster-to-leaf mapping requires a BSP");
    }

    std::map<int, std::vector<int>> result;
    BSP_VisitAllLeafs(*bsp, *BSP_GetWorldModel(bsp), [&](const mleaf_t &leaf) {
        // cluster -1 is invalid
        if (leaf.cluster != -1) {
            result[leaf.cluster].push_back(BSP_GetLeafNum(bsp, &leaf));
        }
    });
    return result;
}
