/*  Copyright (C) 2016 Eric Wasylishen

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

#include <light/trace_embree.hh>
#include <light/lightcontext.hh>

#include <light/light.hh>
#include <light/lightcontext.hh>
#include <light/trace.hh> // for SampleTexture

#include <common/bsputils.hh>
#include <common/polylib.hh>
#include <algorithm>
#include <vector>
#include <climits>
#include <limits>
#include <set>
#include <string_view>

sceneinfo skygeom; // sky. always occludes.
sceneinfo solidgeom; // solids. always occludes.
sceneinfo filtergeom; // conditional occluders.. needs to run ray intersection filter
sceneinfo skipgeom; // generated skip-brush hulls. always occludes when channels match.

// set of faces in `solidgeom`,
std::set<const mface_t *> shadow_casting_solid_faces;

static RTCDevice device;
RTCScene scene;

static const mbsp_t *bsp_static;

struct model_face_range_t
{
    size_t first;
    size_t count;
};

static model_face_range_t CheckedModelFaceRange(const mbsp_t *bsp, const dmodelh2_t *model, size_t modelnum)
{
    if (bsp == nullptr || model == nullptr) {
        FError("Embree model {} has no BSP data", modelnum);
    }
    if (model->firstface < 0 || model->numfaces < 0) {
        FError("Embree model {} has an invalid face range ({}, {})", modelnum, model->firstface, model->numfaces);
    }

    const size_t first = static_cast<size_t>(model->firstface);
    const size_t count = static_cast<size_t>(model->numfaces);
    if (first > bsp->dfaces.size() || count > bsp->dfaces.size() - first) {
        FError("Embree model {} face range ({}, {}) exceeds {} faces", modelnum, first, count, bsp->dfaces.size());
    }
    return {first, count};
}

static size_t CheckedFaceTexinfoIndex(const mbsp_t *bsp, const mface_t *face)
{
    if (bsp == nullptr || face == nullptr) {
        FError("Embree geometry has a null BSP or face");
    }
    if (face->texinfo < 0 || static_cast<size_t>(face->texinfo) >= bsp->texinfo.size()) {
        FError("Embree face {} references invalid texinfo {}", Face_GetNum(bsp, face), face->texinfo);
    }
    if (g_ctx == nullptr || static_cast<size_t>(face->texinfo) >= g_ctx->extended_texinfo_flags.size()) {
        FError("Embree face {} has no extended flags for texinfo {}", Face_GetNum(bsp, face), face->texinfo);
    }
    return static_cast<size_t>(face->texinfo);
}

static const surfflags_t &CheckedExtendedFlagsForFace(const mbsp_t *bsp, const mface_t *face)
{
    return g_ctx->extended_texinfo_flags[CheckedFaceTexinfoIndex(bsp, face)];
}

static void ValidateEmbreeInputs(const mbsp_t *bsp)
{
    if (bsp == nullptr || bsp->loadversion == nullptr || bsp->loadversion->game == nullptr) {
        FError("Embree initialization requires a valid BSP and game version");
    }
    if (g_ctx == nullptr || g_ctx->bsp != bsp) {
        FError("Embree initialization requires the matching light context");
    }
    if (bsp->dmodels.empty()) {
        FError("Embree initialization requires at least one BSP model");
    }
    if (bsp->dfaces.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        FError("Embree initialization cannot address {} BSP faces", bsp->dfaces.size());
    }
    if (g_ctx->modelinfo.size() != bsp->dmodels.size()) {
        FError("Embree initialization has {} model records for {} BSP models", g_ctx->modelinfo.size(),
            bsp->dmodels.size());
    }
    if (g_ctx->face_textures.size() < bsp->dfaces.size()) {
        FError("Embree initialization has {} cached face textures for {} BSP faces", g_ctx->face_textures.size(),
            bsp->dfaces.size());
    }

    for (size_t modelnum = 0; modelnum < bsp->dmodels.size(); ++modelnum) {
        const dmodelh2_t *model = &bsp->dmodels[modelnum];
        CheckedModelFaceRange(bsp, model, modelnum);

        const modelinfo_t *modelinfo = g_ctx->modelinfo[modelnum];
        if (modelinfo == nullptr || modelinfo->bsp != bsp || modelinfo->model != model) {
            FError("Embree model {} has inconsistent lighting metadata", modelnum);
        }
    }

    for (const mface_t &face : bsp->dfaces) {
        CheckedFaceTexinfoIndex(bsp, &face);
    }

    for (const modelinfo_t *modelinfo : g_ctx->tracelist) {
        if (modelinfo == nullptr || modelinfo->bsp != bsp || modelinfo->model == nullptr ||
            std::find(g_ctx->modelinfo.begin(), g_ctx->modelinfo.end(), modelinfo) == g_ctx->modelinfo.end()) {
            FError("Embree trace list contains inconsistent model metadata");
        }
    }
}

void ResetEmbree()
{
    skygeom = {};
    solidgeom = {};
    filtergeom = {};
    skipgeom = {};
    shadow_casting_solid_faces = {};

    if (scene) {
        rtcReleaseScene(scene);
        scene = nullptr;
    }

    if (device) {
        rtcReleaseDevice(device);
        device = nullptr;
    }

    bsp_static = nullptr;
}

const std::set<const mface_t *> &ShadowCastingSolidFacesSet()
{
    return shadow_casting_solid_faces;
}

/**
 * Returns 1.0 unless a custom alpha value is set.
 * The priority is: "_light_alpha" (read from extended_texinfo_flags), then "alpha", then Q2 surface flags
 */
static float Face_Alpha(const mbsp_t *bsp, const modelinfo_t *modelinfo, const mface_t *face)
{
    const surfflags_t &extended_flags = CheckedExtendedFlagsForFace(bsp, face);
    const int surf_flags = Face_ContentsOrSurfaceFlags(bsp, face);
    const bool is_q2 = bsp->loadversion->game->id == GAME_QUAKE_II;

    if (extended_flags.light_alpha) {
        return *extended_flags.light_alpha;
    }

    // next check "alpha" key (q1)
    if (modelinfo->alpha.is_changed()) {
        return modelinfo->alpha.value();
    }

    // next handle q2 surface flags
    if (is_q2) {
        if (surf_flags & Q2_SURF_TRANS33) {
            return 0.33f;
        }
        if (surf_flags & Q2_SURF_TRANS66) {
            return 0.66f;
        }
    }

    // no alpha requested
    return 1.0f;
}

struct embree_vertex_t
{
    float point[4];
};

struct embree_triangle_t
{
    uint32_t v0, v1, v2;
};

class embree_geometry_guard_t
{
public:
    explicit embree_geometry_guard_t(RTCGeometry geometry)
        : geometry_(geometry)
    {
    }

    ~embree_geometry_guard_t()
    {
        if (geometry_ != nullptr) {
            rtcReleaseGeometry(geometry_);
        }
    }

    embree_geometry_guard_t(const embree_geometry_guard_t &) = delete;
    embree_geometry_guard_t &operator=(const embree_geometry_guard_t &) = delete;

    [[nodiscard]] RTCGeometry get() const noexcept { return geometry_; }

private:
    RTCGeometry geometry_;
};

static void AddGeometryCount(size_t &total, size_t increment, std::string_view kind)
{
    if (increment > std::numeric_limits<size_t>::max() - total) {
        FError("Embree {} count overflow", kind);
    }
    total += increment;
}

static size_t CheckedVertexCount(size_t triangle_count)
{
    constexpr size_t VERTICES_PER_TRIANGLE = 3;
    if (triangle_count > std::numeric_limits<uint32_t>::max() / VERTICES_PER_TRIANGLE) {
        FError("Embree geometry has too many triangles ({})", triangle_count);
    }
    return triangle_count * VERTICES_PER_TRIANGLE;
}

sceneinfo CreateGeometry(
    const mbsp_t *bsp, RTCDevice g_device, RTCScene scene, const std::vector<const mface_t *> &faces)
{
    size_t triangle_count = 0;
    for (const mface_t *face : faces) {
        if (face == nullptr) {
            continue;
        }
        CheckedFaceTexinfoIndex(bsp, face);
        if (face->numedges < 3 || ModelInfoForFace(bsp, Face_GetNum(bsp, face)) == nullptr) {
            continue;
        }
        AddGeometryCount(triangle_count, static_cast<size_t>(face->numedges - 2), "triangle");
    }
    const size_t vertex_count = CheckedVertexCount(triangle_count);

    embree_geometry_guard_t geometry(rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE));
    if (geometry.get() == nullptr) {
        FError("Embree failed to create triangle geometry");
    }
    // We're not using masks, but they need to be set to something or else all
    // rays miss when Embree is compiled with mask support.
    rtcSetGeometryMask(geometry.get(), 1);
    rtcSetGeometryBuildQuality(geometry.get(), RTC_BUILD_QUALITY_MEDIUM);
    rtcSetGeometryTimeStepCount(geometry.get(), 1);

    auto *vertices = static_cast<embree_vertex_t *>(rtcSetNewGeometryBuffer(
        geometry.get(), RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(embree_vertex_t), vertex_count));
    auto *triangles = static_cast<embree_triangle_t *>(rtcSetNewGeometryBuffer(
        geometry.get(), RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(embree_triangle_t), triangle_count));
    if ((vertex_count != 0 && vertices == nullptr) || (triangle_count != 0 && triangles == nullptr)) {
        FError("Embree failed to allocate geometry buffers ({} vertices, {} triangles)", vertex_count, triangle_count);
    }

    sceneinfo s;
    s.triInfo.reserve(triangle_count);
    size_t vertex_index = 0;
    size_t triangle_index = 0;

    // FIXME: reuse vertices
    auto add_tri = [&](const mface_t *face, int bsp_vert0, int bsp_vert1, int bsp_vert2, const modelinfo_t *modelinfo) {
        const qvec3f final_pos0 = Vertex_GetPos(bsp, bsp_vert0) + modelinfo->offset;
        const qvec3f final_pos1 = Vertex_GetPos(bsp, bsp_vert1) + modelinfo->offset;
        const qvec3f final_pos2 = Vertex_GetPos(bsp, bsp_vert2) + modelinfo->offset;

        const uint32_t first_vertex_index = static_cast<uint32_t>(vertex_index);
        vertices[vertex_index++] = {.point{final_pos0[0], final_pos0[1], final_pos0[2], 0.0f}};
        vertices[vertex_index++] = {.point{final_pos1[0], final_pos1[1], final_pos1[2], 0.0f}};
        vertices[vertex_index++] = {.point{final_pos2[0], final_pos2[1], final_pos2[2], 0.0f}};
        triangles[triangle_index++] = {first_vertex_index, first_vertex_index + 1, first_vertex_index + 2};

        const surfflags_t &extended_flags = CheckedExtendedFlagsForFace(bsp, face);

        triinfo info{};

        info.face = face;
        info.modelinfo = modelinfo;
        info.texinfo = &bsp->texinfo[CheckedFaceTexinfoIndex(bsp, face)];

        info.texture = Face_Texture(bsp, face);

        // FIXME: don't these need to check extended_flags?
        info.shadowworldonly = modelinfo->shadowworldonly.boolValue();
        info.shadowself = modelinfo->shadowself.boolValue();
        info.switchableshadow = modelinfo->switchableshadow.boolValue();
        info.switchshadstyle = modelinfo->switchshadstyle.value();

        info.channelmask = extended_flags.object_channel_mask.value_or(modelinfo->object_channel_mask.value());

        info.alpha = Face_Alpha(bsp, modelinfo, face);

        // mxd
        if (bsp->loadversion->game->id == GAME_QUAKE_II) {
            const int surf_flags = Face_ContentsOrSurfaceFlags(bsp, face);
            info.is_fence = surf_flags & Q2_SURF_ALPHATEST;
            info.is_glass = !info.is_fence && (surf_flags & (Q2_SURF_TRANS33 | Q2_SURF_TRANS66));
        } else {
            const char *name = Face_TextureName(bsp, face);
            info.is_fence = (name[0] == '{');
            info.is_glass = (info.alpha < 1.0f);
        }

        s.triInfo.push_back(info);
    };

    auto add_face = [&](const mface_t *face, const modelinfo_t *modelinfo) {
        if (face->numedges < 3)
            return;

        for (int j = 2; j < face->numedges; j++) {
            int bsp_vert0 = Face_VertexAtIndex(bsp, face, j - 1);
            int bsp_vert1 = Face_VertexAtIndex(bsp, face, j);
            int bsp_vert2 = Face_VertexAtIndex(bsp, face, 0);

            add_tri(face, bsp_vert0, bsp_vert1, bsp_vert2, modelinfo);
        }
    };

    for (const mface_t *face : faces) {
        // NOTE: can be null for "skip" faces
        if (face == nullptr) {
            continue;
        }
        const modelinfo_t *modelinfo = ModelInfoForFace(bsp, Face_GetNum(bsp, face));

        if (modelinfo) {
            add_face(face, modelinfo);
        }
    }

    Q_assert(vertex_index == vertex_count);
    Q_assert(triangle_index == triangle_count);
    Q_assert(s.triInfo.size() == triangle_count);

    rtcCommitGeometry(geometry.get());
    s.geomID = rtcAttachGeometry(scene, geometry.get());
    if (s.geomID == RTC_INVALID_GEOMETRY_ID) {
        FError("Embree failed to attach triangle geometry");
    }
    return s;
}

static sceneinfo CreateGeometryFromWindings(RTCDevice g_device, RTCScene scene,
    const std::vector<polylib::winding3f_t> &windings, const std::vector<const modelinfo_t *> &winding_models)
{
    if (windings.size() != winding_models.size()) {
        FError("Embree skip geometry has {} windings but {} model records", windings.size(), winding_models.size());
    }

    sceneinfo s;
    if (windings.empty()) {
        return s;
    }

    // Count first so Embree can own the only vertex/index buffers. Keep the
    // arithmetic checked before converting indices to uint32_t.
    size_t numtris = 0;
    size_t numverts = 0;
    for (size_t i = 0; i < windings.size(); ++i) {
        const auto &winding = windings[i];
        if (winding.size() < 3) {
            FError("Embree skip winding has fewer than 3 vertices");
        }
        if (winding_models[i] == nullptr) {
            FError("Embree skip winding {} has no model metadata", i);
        }
        AddGeometryCount(numtris, winding.size() - 2, "skip triangle");
        AddGeometryCount(numverts, winding.size(), "skip vertex");
    }
    if (numverts > std::numeric_limits<uint32_t>::max()) {
        FError("Embree skip geometry has too many vertices ({})", numverts);
    }

    embree_geometry_guard_t geometry(rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE));
    if (geometry.get() == nullptr) {
        FError("Embree failed to create skip geometry");
    }
    rtcSetGeometryBuildQuality(geometry.get(), RTC_BUILD_QUALITY_MEDIUM);
    rtcSetGeometryMask(geometry.get(), 1);
    rtcSetGeometryTimeStepCount(geometry.get(), 1);

    // fill in vertices
    auto *vertices = static_cast<embree_vertex_t *>(rtcSetNewGeometryBuffer(
        geometry.get(), RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(embree_vertex_t), numverts));
    auto *triangles = static_cast<embree_triangle_t *>(rtcSetNewGeometryBuffer(
        geometry.get(), RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(embree_triangle_t), numtris));
    if (vertices == nullptr || triangles == nullptr) {
        FError("Embree failed to allocate skip geometry buffers ({} vertices, {} triangles)", numverts, numtris);
    }
    s.triInfo.reserve(numtris);
    {
        size_t vert_index = 0;
        for (const auto &winding : windings) {
            for (size_t j = 0; j < winding.size(); j++) {
                for (size_t k = 0; k < 3; k++) {
                    vertices[vert_index + j].point[k] = winding.at(j)[k];
                }
                vertices[vert_index + j].point[3] = 0.0f;
            }
            vert_index += winding.size();
        }
        Q_assert(vert_index == numverts);
    }

    // fill in triangles
    size_t tri_index = 0;
    uint32_t vert_index = 0;
    for (size_t winding_index = 0; winding_index < windings.size(); ++winding_index) {
        const auto &winding = windings[winding_index];
        const modelinfo_t *modelinfo = winding_models[winding_index];
        for (size_t j = 2; j < winding.size(); j++) {
            embree_triangle_t *tri = &triangles[tri_index];
            tri->v0 = vert_index + static_cast<uint32_t>(j - 1);
            tri->v1 = vert_index + static_cast<uint32_t>(j);
            tri->v2 = vert_index;
            tri_index++;

            triinfo info{};
            info.modelinfo = modelinfo;
            info.alpha = 1.0f;
            info.shadowworldonly = modelinfo->shadowworldonly.boolValue();
            info.shadowself = modelinfo->shadowself.boolValue();
            info.switchableshadow = modelinfo->switchableshadow.boolValue();
            info.switchshadstyle = modelinfo->switchshadstyle.value();
            info.channelmask = modelinfo->object_channel_mask.value();
            s.triInfo.push_back(info);
        }
        vert_index += static_cast<uint32_t>(winding.size());
    }
    Q_assert(vert_index == numverts);
    Q_assert(tri_index == numtris);
    Q_assert(s.triInfo.size() == numtris);

    rtcCommitGeometry(geometry.get());
    s.geomID = rtcAttachGeometry(scene, geometry.get());
    if (s.geomID == RTC_INVALID_GEOMETRY_ID) {
        FError("Embree failed to attach skip geometry");
    }
    return s;
}

void ErrorCallback(void *userptr, const RTCError code, const char *str)
{
    fmt::print("RTC Error {}: {}\n", static_cast<int>(code), str);
}

const triinfo &Embree_LookupTriangleInfo(unsigned int geomID, unsigned int primID)
{
    const sceneinfo &info = Embree_SceneinfoForGeomID(geomID);
    return info.triInfo.at(primID);
}

inline qvec3f Embree_RayEndpoint(RTCRayN *ray, const qvec3f &dir, size_t N, size_t i)
{
    qvec3f org{RTCRayN_org_x(ray, N, i), RTCRayN_org_y(ray, N, i), RTCRayN_org_z(ray, N, i)};
    float &tfar = RTCRayN_tfar(ray, N, i);

    return org + (dir * tfar);
}

static void AddGlassToRay(ray_source_info *context, unsigned rayIndex, float opacity, const qvec3f &glasscolor);
static void AddDynamicOccluderToRay(ray_source_info *context, unsigned rayIndex, int style);

// called to evaluate transparency
static void Embree_FilterFuncN(const struct RTCFilterFunctionNArguments *args)
{
    int *const valid = args->valid;
    struct RTCRayN *const ray = args->ray;
    struct RTCHitN *const potentialHit = args->hit;
    const unsigned int N = args->N;

    const int VALID = -1;
    const int INVALID = 0;

    ray_source_info *rsi = static_cast<ray_source_info *>(args->context);

    for (size_t i = 0; i < N; i++) {
        if (valid[i] != VALID) {
            // we only need to handle valid rays
            continue;
        }

        const unsigned &rayID = RTCRayN_id(ray, N, i);
        const unsigned &geomID = RTCHitN_geomID(potentialHit, N, i);
        const unsigned &primID = RTCHitN_primID(potentialHit, N, i);

        // unpack ray index
        const unsigned rayIndex = rayID;

        const modelinfo_t *source_modelinfo = rsi->self;
        const triinfo &hit_triinfo = Embree_LookupTriangleInfo(geomID, primID);

        if (!(hit_triinfo.channelmask & rsi->shadowmask)) {
            // reject hit
            valid[i] = INVALID;
            continue;
        }

        if (!hit_triinfo.modelinfo) {
            // we hit a "skip" face with no associated model
            // reject hit (???)
            valid[i] = INVALID;
            continue;
        }

        if (hit_triinfo.shadowworldonly) {
            // we hit "_shadowworldonly" "1" geometry. Ignore the hit unless we are from world.
            if (!source_modelinfo || !source_modelinfo->isWorld()) {
                // reject hit
                valid[i] = INVALID;
                continue;
            }
        }

        if (hit_triinfo.shadowself) {
            // only casts shadows on itself
            if (source_modelinfo != hit_triinfo.modelinfo) {
                // reject hit
                valid[i] = INVALID;
                continue;
            }
        }

        if (hit_triinfo.switchableshadow) {
            // we hit a dynamic shadow caster. reject the hit, but store the
            // info about what we hit.

            const int style = hit_triinfo.switchshadstyle;

            // only treat it as a switchable shadow on models that are part of a different style-group.
            // i.e. the switchable shadow caster should still self-shadow, when the shadow is "off"
            if (!source_modelinfo || source_modelinfo->switchshadstyle.value() != style) {
                AddDynamicOccluderToRay(rsi, rayIndex, style);

                // reject hit
                valid[i] = INVALID;
                continue;
            }
        }

        float alpha = hit_triinfo.alpha;

        // test fence textures and glass
        if (hit_triinfo.is_fence || hit_triinfo.is_glass) {
            qvec3f rayDir =
                qv::normalize(qvec3f{RTCRayN_dir_x(ray, N, i), RTCRayN_dir_y(ray, N, i), RTCRayN_dir_z(ray, N, i)});
            qvec3f hitpoint = Embree_RayEndpoint(ray, rayDir, N, i);
            const qvec4b sample = SampleTexture(hit_triinfo.face, hit_triinfo.texinfo, hit_triinfo.texture, bsp_static,
                hitpoint); // mxd. Palette index -> color_rgba

            if (hit_triinfo.is_glass) {
                // hit glass...

                // mxd. Adjust alpha by texture alpha?
                if (sample[3] < 255)
                    alpha = sample[3] / 255.0f;

                qvec3f potentialHitGeometryNormal = qv::normalize(qvec3f{RTCHitN_Ng_x(potentialHit, N, i),
                    RTCHitN_Ng_y(potentialHit, N, i), RTCHitN_Ng_z(potentialHit, N, i)});

                const float raySurfaceCosAngle = qv::dot(rayDir, potentialHitGeometryNormal);

                // only pick up the color of the glass on the _exiting_ side of the glass.
                // (we currently trace "backwards", from surface point --> light source)
                if (raySurfaceCosAngle < 0) {
                    AddGlassToRay(rsi, rayIndex, alpha, sample.xyz() * (1.0f / 255.0f));
                }

                // reject hit
                valid[i] = INVALID;
                continue;
            }

            if (hit_triinfo.is_fence) {
                if (sample[3] < 255) {
                    // reject hit
                    valid[i] = INVALID;
                    continue;
                }
            }
        }

        // accept hit
        // (just need to leave the `valid` value set to VALID)
    }
}

/**
 * For use with all rays coming from a model with non-default channel mask
 */
static void PerRay_FilterFuncN(const struct RTCFilterFunctionNArguments *args)
{
    int *const valid = args->valid;
    struct RTCHitN *const potentialHit = args->hit;
    struct RTCRayN *const ray = args->ray;
    const unsigned int N = args->N;

    const int VALID = -1;
    const int INVALID = 0;

    auto *rsi = static_cast<ray_source_info *>(args->context);

    for (size_t i = 0; i < N; i++) {
        if (valid[i] != VALID) {
            // we only need to handle valid rays
            continue;
        }

        const unsigned &geomID = RTCHitN_geomID(potentialHit, N, i);
        const unsigned &primID = RTCHitN_primID(potentialHit, N, i);

        // unpack ray index
        const triinfo &hit_triinfo = Embree_LookupTriangleInfo(geomID, primID);

        if (!(hit_triinfo.channelmask & rsi->shadowmask)) {
            // reject hit
            valid[i] = INVALID;
            continue;
        }

        // accept hit
        // (just need to leave the `valid` value set to VALID)
    }
}

// building faces for skip-textured bmodels

qplane3f Node_Plane(const mbsp_t *bsp, const bsp2_dnode_t *node, bool side)
{
    if (node == nullptr) {
        FError("Embree skip tree contains a null node");
    }
    qplane3f plane = *BSP_GetPlane(bsp, node->planenum);

    if (side) {
        return -plane;
    }

    return plane;
}

/**
 * `planes` all of the node planes that bound this leaf, facing inward.
 */
static void Leaf_MakeFaces(const mbsp_t *bsp, const modelinfo_t *modelinfo, const mleaf_t *leaf,
    const std::vector<qplane3f> &planes, std::vector<polylib::winding3f_t> &result)
{
    if (bsp == nullptr || modelinfo == nullptr || leaf == nullptr) {
        FError("Embree skip face generation requires valid BSP, model, and leaf data");
    }

    for (const qplane3f &plane : planes) {
        // flip the inward-facing split plane to get the outward-facing plane of the face we're constructing
        qplane3f faceplane = -plane;

        std::optional<polylib::winding3f_t> winding = polylib::winding3f_t::from_plane(faceplane, 10e6);
        if (!winding) {
            continue;
        }

        // clip `winding` by all of the other planes
        for (const qplane3f &plane2 : planes) {
            if (&plane2 == &plane)
                continue;

            // discard the back, continue clipping the front part
            winding = winding->clip_front(plane2);

            // check if everything was clipped away
            if (!winding)
                break;
        }

        if (!winding) {
            // logging::print("WARNING: winding clipped away\n");
        } else {
            result.push_back(winding->translate(modelinfo->offset));
        }
    }
}

void MakeFaces_r(const mbsp_t *bsp, const modelinfo_t *modelinfo, const int nodenum, std::vector<qplane3f> *planes,
    std::vector<polylib::winding3f_t> &result)
{
    if (bsp == nullptr || bsp->loadversion == nullptr || bsp->loadversion->game == nullptr || modelinfo == nullptr ||
        planes == nullptr) {
        FError("Embree skip face generation requires valid BSP, model, and plane-stack data");
    }

    enum class traversal_state_t : uint8_t
    {
        ENTER,
        FRONT_COMPLETE,
        BACK_COMPLETE
    };
    struct traversal_frame_t
    {
        int nodenum;
        traversal_state_t state = traversal_state_t::ENTER;
    };

    const size_t initial_plane_count = planes->size();
    std::vector<traversal_frame_t> stack{{nodenum}};
    std::vector<uint8_t> active_nodes(bsp->dnodes.size(), 0);

    try {
        while (!stack.empty()) {
            traversal_frame_t &frame = stack.back();

            if (frame.nodenum < 0) {
                const mleaf_t *leaf = BSP_GetLeafFromNodeNum(bsp, frame.nodenum);
                if ((bsp->loadversion->game->id == GAME_QUAKE_II) ? (leaf->contents & Q2_CONTENTS_SOLID)
                                                                  : leaf->contents == CONTENTS_SOLID) {
                    Leaf_MakeFaces(bsp, modelinfo, leaf, *planes, result);
                }
                stack.pop_back();
                continue;
            }

            const size_t node_index = static_cast<size_t>(frame.nodenum);
            if (node_index >= bsp->dnodes.size()) {
                FError("Embree skip tree node {} is out of bounds", frame.nodenum);
            }
            const bsp2_dnode_t *node = &bsp->dnodes[node_index];

            if (frame.state == traversal_state_t::ENTER) {
                if (active_nodes[node_index]) {
                    FError("Embree skip tree contains a cycle at node {}", frame.nodenum);
                }
                active_nodes[node_index] = 1;
                frame.state = traversal_state_t::FRONT_COMPLETE;
                planes->push_back(Node_Plane(bsp, node, false));
                stack.push_back({node->children[0]});
                continue;
            }

            if (planes->size() <= initial_plane_count) {
                FError("Embree skip tree plane stack became unbalanced");
            }
            planes->pop_back();

            if (frame.state == traversal_state_t::FRONT_COMPLETE) {
                frame.state = traversal_state_t::BACK_COMPLETE;
                planes->push_back(Node_Plane(bsp, node, true));
                stack.push_back({node->children[1]});
                continue;
            }

            active_nodes[node_index] = 0;
            stack.pop_back();
        }
    } catch (...) {
        planes->resize(initial_plane_count);
        throw;
    }

    if (planes->size() != initial_plane_count) {
        planes->resize(initial_plane_count);
        FError("Embree skip tree plane stack became unbalanced");
    }
}

static void MakeFaces(
    const mbsp_t *bsp, const modelinfo_t *modelinfo, const dmodelh2_t *model, std::vector<polylib::winding3f_t> &result)
{
    if (model == nullptr) {
        FError("Embree skip face generation requires a valid model");
    }
    std::vector<qplane3f> planes;
    MakeFaces_r(bsp, modelinfo, model->headnode[0], &planes, result);
    Q_assert(planes.empty());
}

void Embree_TraceInit(const mbsp_t *bsp)
{
    if (device != nullptr || scene != nullptr) {
        FError("Embree trace state is already initialized");
    }

    std::vector<const mface_t *> skyfaces, solidfaces, filterfaces;
    std::vector<polylib::winding3f_t> skipwindings;
    std::vector<const modelinfo_t *> skipwinding_models;

    try {
        ValidateEmbreeInputs(bsp);
        bsp_static = bsp;

        // check all modelinfos
        for (size_t mi = 0; mi < bsp->dmodels.size(); mi++) {
            const modelinfo_t *model = g_ctx->modelinfo[mi];
            const model_face_range_t face_range = CheckedModelFaceRange(bsp, model->model, mi);

            // check reasons that a bmodel can be shadow casting
            const bool isWorld = model->isWorld();
            const bool shadow = model->shadow.boolValue();
            const bool shadowself = model->shadowself.boolValue();
            const bool shadowworldonly = model->shadowworldonly.boolValue();
            const bool switchableshadow = model->switchableshadow.boolValue();
            const bool has_custom_channel_mask = (model->object_channel_mask.value() != CHANNEL_MASK_DEFAULT);

            if (!(isWorld || shadow || shadowself || shadowworldonly || switchableshadow || has_custom_channel_mask))
                continue;

            for (size_t i = 0; i < face_range.count; i++) {
                const mface_t *face = &bsp->dfaces[face_range.first + i];

                // check for TEX_NOSHADOW
                const surfflags_t &extended_flags = CheckedExtendedFlagsForFace(bsp, face);
                if (extended_flags.no_shadow)
                    continue;

                // handle switchableshadow
                if (switchableshadow) {
                    filterfaces.push_back(face);
                    continue;
                }

                // non-default channel mask
                if (model->object_channel_mask.value() != CHANNEL_MASK_DEFAULT ||
                    extended_flags.object_channel_mask.value_or(CHANNEL_MASK_DEFAULT) != CHANNEL_MASK_DEFAULT) {
                    filterfaces.push_back(face);
                    continue;
                }

                const int contents_or_surf_flags = Face_ContentsOrSurfaceFlags(bsp, face); // mxd
                const mtexinfo_t *texinfo = Face_Texinfo(bsp, face);
                const bool is_q2 = bsp->loadversion->game->id == GAME_QUAKE_II;

                // mxd. Skip NODRAW faces, but not SKY ones (Q2's sky01.wal has both flags set)
                if (is_q2 && (contents_or_surf_flags & Q2_SURF_NODRAW) && !(contents_or_surf_flags & Q2_SURF_SKY))
                    continue;

                // handle glass / water
                const float alpha = Face_Alpha(bsp, model, face);
                if (alpha < 1.0f ||
                    (is_q2 && (contents_or_surf_flags & (Q2_SURF_ALPHATEST | Q2_SURF_TRANS33 | Q2_SURF_TRANS66)))) {
                    filterfaces.push_back(face);
                    continue;
                }

                // fence
                const char *texname = Face_TextureName(bsp, face);
                if (texname[0] == '{') {
                    filterfaces.push_back(face);
                    continue;
                }

                // handle sky
                if (is_q2) {
                    // Q2: arghrad compat: sky faces only emit sunlight if:
                    // sky flag set, light flag set, value nonzero
                    if ((contents_or_surf_flags & Q2_SURF_SKY) != 0 &&
                        (!light_options.arghradcompat.value() ||
                            ((contents_or_surf_flags & Q2_SURF_LIGHT) != 0 && texinfo->value != 0))) {
                        skyfaces.push_back(face);
                        continue;
                    }
                } else {
                    // Q1
                    if (!Q_strncasecmp("sky", texname, 3)) {
                        skyfaces.push_back(face);
                        continue;
                    }
                }

                // liquids
                if (/* texname[0] == '*' */ ContentsOrSurfaceFlags_IsTranslucent(bsp, contents_or_surf_flags)) { // mxd
                    if (!isWorld) {
                        // world liquids never cast shadows; shadow casting bmodel liquids do
                        solidfaces.push_back(face);
                    }
                    continue;
                }

                // solid faces

                if (isWorld || shadow) {
                    solidfaces.push_back(face);
                } else {
                    // shadowself or shadowworldonly
                    Q_assert(shadowself || shadowworldonly);
                    filterfaces.push_back(face);
                }
            }
        }

        /* Special handling of skip-textured bmodels */
        for (const modelinfo_t *modelinfo : g_ctx->tracelist) {
            if (modelinfo->model->numfaces == 0) {
                const size_t first_winding = skipwindings.size();
                MakeFaces(bsp, modelinfo, modelinfo->model, skipwindings);
                skipwinding_models.insert(skipwinding_models.end(), skipwindings.size() - first_winding, modelinfo);
            }
        }

        device = rtcNewDevice(nullptr);
        if (device == nullptr) {
            FError("failed to initialize Embree device");
        }
        rtcSetDeviceErrorFunction(device, ErrorCallback,
            nullptr); // mxd. Changed from rtcDeviceSetErrorFunction to silence compiler warning...

        // log version
        const size_t ver_maj = rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_VERSION_MAJOR);
        const size_t ver_min = rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_VERSION_MINOR);
        const size_t ver_pat = rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_VERSION_PATCH);
        logging::funcprint("Embree version: {}.{}.{}\n", ver_maj, ver_min, ver_pat);

        scene = rtcNewScene(device);
        if (scene == nullptr) {
            FError("failed to initialize Embree scene");
        }
        // necessary for RTCOccludedArguments::filter and RTCIntersectArguments::filter
        // to work, which we use (see: ray_source_info::setup_intersection_arguments() and
        // ray_source_info::setup_occluded_arguments())
        rtcSetSceneFlags(scene, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);

        rtcSetSceneBuildQuality(scene, RTC_BUILD_QUALITY_HIGH);
        skygeom = CreateGeometry(bsp, device, scene, skyfaces);
        solidgeom = CreateGeometry(bsp, device, scene, solidfaces);
        filtergeom = CreateGeometry(bsp, device, scene, filterfaces);
        skipgeom = CreateGeometryFromWindings(device, scene, skipwindings, skipwinding_models);

        rtcSetGeometryIntersectFilterFunction(rtcGetGeometry(scene, filtergeom.geomID), Embree_FilterFuncN);
        rtcSetGeometryOccludedFilterFunction(rtcGetGeometry(scene, filtergeom.geomID), Embree_FilterFuncN);
        if (skipgeom.geomID != RTC_INVALID_GEOMETRY_ID) {
            RTCGeometry skip_geometry = rtcGetGeometry(scene, skipgeom.geomID);
            if (skip_geometry == nullptr) {
                FError("Embree failed to retrieve attached skip geometry");
            }
            rtcSetGeometryIntersectFilterFunction(skip_geometry, Embree_FilterFuncN);
            rtcSetGeometryOccludedFilterFunction(skip_geometry, Embree_FilterFuncN);
        }
        rtcCommitScene(scene);

        // keep a backup of solidfaces
        for (const mface_t *face : solidfaces) {
            shadow_casting_solid_faces.insert(face);
        }
    } catch (...) {
        ResetEmbree();
        throw;
    }

    logging::funcprint("\n");
    logging::print("\t{} sky faces\n", skyfaces.size());
    logging::print("\t{} solid faces\n", solidfaces.size());
    logging::print("\t{} filtered faces\n", filterfaces.size());
    logging::print("\t{} shadow-casting skip faces\n", skipwindings.size());
}

static void AddGlassToRay(ray_source_info *ctx, unsigned rayIndex, float opacity, const qvec3f &glasscolor)
{
    raystream_embree_common_t *rs = ctx->raystream;

    if (rs == nullptr) {
        // FIXME: remove this.. once all ray casts use raystreams
        // happens for bounce lights, e.g. Embree_TestSky
        return;
    }

    // clamp opacity
    opacity = std::clamp(opacity, 0.0f, 1.0f);

    Q_assert(rayIndex < rs->numPushedRays());

    ray_io &ray = rs->getRay(rayIndex);

    ray.payload.hit_glass = true;
    ray.payload.glass_color = glasscolor;
    ray.payload.glass_opacity = opacity;
}

static void AddDynamicOccluderToRay(ray_source_info *ctx, unsigned rayIndex, int style)
{
    raystream_embree_common_t *rs = ctx->raystream;

    if (rs != nullptr) {
        ray_io &ray = rs->getRay(rayIndex);
        ray.payload.dynamic_style = style;
    }
}

ray_source_info::ray_source_info(raystream_embree_common_t *raystream_, const modelinfo_t *self_, int shadowmask_)
    : raystream(raystream_),
      self(self_),
      shadowmask(shadowmask_)
{
    rtcInitRayQueryContext(this);
}

RTCIntersectArguments ray_source_info::setup_intersection_arguments()
{
    RTCIntersectArguments result;

    rtcInitIntersectArguments(&result);
    if (shadowmask != CHANNEL_MASK_DEFAULT) {
        // non-default shadow mask means we have to use the slow path
        result.filter = PerRay_FilterFuncN;
        result.flags = static_cast<RTCRayQueryFlags>(result.flags | RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
    }

    result.context = this;

    return result;
}

RTCOccludedArguments ray_source_info::setup_occluded_arguments()
{
    RTCOccludedArguments result;

    rtcInitOccludedArguments(&result);
    if (shadowmask != CHANNEL_MASK_DEFAULT) {
        // non-default shadow mask means we have to use the slow path
        result.filter = PerRay_FilterFuncN;
        result.flags = static_cast<RTCRayQueryFlags>(result.flags | RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
    }

    result.context = this;

    return result;
}
