#pragma once

#include <common/aligned_allocator.hh>
#include <common/qvec.hh>
#include <common/log.hh> // for FError

#include <vector>
#include <set>

#include <embree4/rtcore.h>
#include <embree4/rtcore_ray.h>

#include <light/raystream.hh>

struct mbsp_t;
class modelinfo_t;
struct mface_t;

class light_t;
struct mtexinfo_t;
namespace img
{
struct texture;
}

void ResetEmbree();
void Embree_TraceInit(const mbsp_t *bsp);
const std::set<const mface_t *> &ShadowCastingSolidFacesSet();

struct ray_io
{
    RTCRayHit ray;
    RayPayload payload;
};

struct alignas(16) aligned_vec3
{
    float x, y, z, w;
};

class raystream_embree_common_t : public virtual RayStream
{
protected:
    aligned_vector<ray_io> _rays;

public:
    inline raystream_embree_common_t() = default;
    inline raystream_embree_common_t(size_t capacity) { _rays.reserve(capacity); }
    virtual ~raystream_embree_common_t() = default;

    void resize(size_t size) override { _rays.resize(size); }
    void reserve(size_t size) override { _rays.reserve(size); }

    ray_io &getRay(size_t index) { return _rays[index]; }
    const ray_io &getRay(size_t index) const { return _rays[index]; };

    size_t numPushedRays() const override { return _rays.size(); }

    void clearPushedRays() override { _rays.clear(); }

    const RayPayload &getPayload(size_t index) const override { return _rays[index].payload; }

protected:
    static inline RTCRayHit SetupRay(
        unsigned int rayindex, const aligned_vec3 &start, const aligned_vec3 &dir, float dist)
    {
        RTCRayHit ray;
        ray.ray.org_x = start.x;
        ray.ray.org_y = start.y;
        ray.ray.org_z = start.z;
        ray.ray.tnear = start.w;

        ray.ray.dir_x = dir.x; // can be un-normalized
        ray.ray.dir_y = dir.y;
        ray.ray.dir_z = dir.z;
        ray.ray.time = dir.w; // not using

        ray.ray.tfar = dist;
        ray.ray.mask = 1; // we're not using, but needs to be set if embree is compiled with masks
        ray.ray.id = rayindex;
        ray.ray.flags = 0; // reserved

        ray.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        ray.hit.primID = RTC_INVALID_GEOMETRY_ID;
        ray.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
        return ray;
    }
};

extern RTCScene scene;

struct ray_source_info : public RTCRayQueryContext
{
    raystream_embree_common_t *raystream; // may be null if this ray is not from a ray stream
    const modelinfo_t *self;
    int shadowmask;

    ray_source_info(raystream_embree_common_t *raystream_, const modelinfo_t *self_, int shadowmask_);
    RTCIntersectArguments setup_intersection_arguments();
    RTCOccludedArguments setup_occluded_arguments();
};

struct triinfo
{
    const modelinfo_t *modelinfo;
    const mface_t *face;
    const mtexinfo_t *texinfo;

    const img::texture *texture;
    float alpha;
    bool is_fence, is_glass;
    bool transparent_for_lighting = false;

    // cached from modelinfo for faster access
    bool shadowworldonly;
    bool shadowself;
    bool switchableshadow;
    int32_t switchshadstyle;

    int channelmask;
};

struct sceneinfo
{
    unsigned geomID;

    std::vector<triinfo> triInfo;
};

extern sceneinfo skygeom; // sky. always occludes.
extern sceneinfo solidgeom; // solids. always occludes.
extern sceneinfo filtergeom; // conditional occluders.. needs to run ray intersection filter

inline const sceneinfo &Embree_SceneinfoForGeomID(unsigned int geomID)
{
    if (geomID == skygeom.geomID) {
        return skygeom;
    } else if (geomID == solidgeom.geomID) {
        return solidgeom;
    } else if (geomID == filtergeom.geomID) {
        return filtergeom;
    } else {
        FError("unexpected geomID");
    }
}

class raystream_intersection_t : public raystream_embree_common_t, public RayStreamIntersection
{
public:
    using raystream_embree_common_t::raystream_embree_common_t;

    inline void pushRay(int i, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) override
    {
        const RTCRayHit rayHit =
            SetupRay(_rays.size(), {origin[0], origin[1], origin[2], 0.f}, {dir[0], dir[1], dir[2], 0.f}, dist);

        RayPayload payload;
        payload.index = i;
        payload.maxdist = dist;
        payload.color = color ? *color : qvec3f{};
        payload.normalcontrib = normalcontrib ? *normalcontrib : qvec3f{};

        _rays.push_back(ray_io{.ray = rayHit, .payload = payload});
    }

    inline void tracePushedRaysIntersection(const modelinfo_t *self, int shadowmask) override
    {
        if (!_rays.size())
            return;

        ray_source_info ctx2(this, self, shadowmask);

        RTCIntersectArguments embree4_args = ctx2.setup_intersection_arguments();
        for (auto &ray : _rays)
            rtcIntersect1(scene, &ray.ray, &embree4_args);
    }

    const qvec3f &getPushedRayDir(size_t j) const override { return *((qvec3f *)&_rays[j].ray.ray.dir_x); }

    const float getPushedRayHitDist(size_t j) const override { return _rays[j].ray.ray.tfar; }

    hittype_t getPushedRayHitType(size_t j) const override
    {
        const unsigned id = _rays[j].ray.hit.geomID;
        if (id == RTC_INVALID_GEOMETRY_ID) {
            return hittype_t::NONE;
        } else if (id == skygeom.geomID) {
            return hittype_t::SKY;
        } else {
            return hittype_t::SOLID;
        }
    }

    const triinfo *getPushedRayHitFaceInfo(size_t j) const override
    {
        const RTCRayHit &ray = _rays[j].ray;

        if (ray.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            return nullptr;
        }

        const sceneinfo &si = Embree_SceneinfoForGeomID(ray.hit.geomID);
        const triinfo *face = &si.triInfo.at(ray.hit.primID);
        Q_assert(face != nullptr);

        return face;
    }
};

class raystream_occlusion_t : public raystream_embree_common_t, public RayStreamOcclusion
{
public:
    using raystream_embree_common_t::raystream_embree_common_t;

    inline void pushRay(int i, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) override
    {
        const RTCRay ray =
            SetupRay(_rays.size(), {origin[0], origin[1], origin[2], 0.f}, {dir[0], dir[1], dir[2], 0.f}, dist).ray;

        RayPayload payload;
        payload.index = i;
        payload.maxdist = dist;
        payload.color = color ? *color : qvec3f{};
        payload.normalcontrib = normalcontrib ? *normalcontrib : qvec3f{};

        _rays.push_back(ray_io{.ray = {ray}, .payload = payload});
    }

    inline void tracePushedRaysOcclusion(const modelinfo_t *self, int shadowmask) override
    {
        if (!_rays.size())
            return;

        ray_source_info ctx2(this, self, shadowmask);
        RTCOccludedArguments embree4_args = ctx2.setup_occluded_arguments();
        for (auto &ray : _rays)
            rtcOccluded1(scene, &ray.ray.ray, &embree4_args);
    }

    bool getPushedRayOccluded(size_t j) const override { return (_rays[j].ray.ray.tfar < 0.0f); }

    const qvec3f &getPushedRayDir(size_t j) const override { return *((qvec3f *)&_rays[j].ray.ray.dir_x); }
};
