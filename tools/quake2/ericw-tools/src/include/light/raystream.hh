#pragma once

#include <common/qvec.hh>
#include <vector>

// Forward declarations
struct triinfo;
class modelinfo_t;

enum class hittype_t : uint8_t
{
    NONE = 0,
    SOLID = 1,
    SKY = 2
};

struct RayPayload
{
    int index;
    qvec3f color;
    qvec3f normalcontrib;
    float maxdist;

    // Glass / Translucency support
    bool hit_glass = false;
    qvec3f glass_color;
    float glass_opacity;

    // Dynamic shadows
    int dynamic_style = 0;
};

class RayStream
{
public:
    virtual ~RayStream() = default;

    virtual void resize(size_t size) = 0;
    virtual void reserve(size_t size) = 0;
    virtual void clearPushedRays() = 0;
    virtual size_t numPushedRays() const = 0;

    // Common accessors
    virtual const RayPayload &getPayload(size_t index) const = 0;

    // Helper to get computed color (handling glass etc)
    inline qvec3f getPushedRayColor(size_t j) const
    {
        const RayPayload &p = getPayload(j);
        qvec3f result = p.color;

        if (p.hit_glass) {
            // mix(result, result * glass_color, glass_opacity)
            const qvec3f tinted = result * p.glass_color;
            result = mix(result, tinted, p.glass_opacity);
        }
        return result;
    }

    // Backend agnostic push
    // virtual void pushRay(...) = 0; // Signature varies between Intersection/Occlusion slightly in usage?
    // Actually they are the same, just the trace function differs.
};

class RayStreamOcclusion : public virtual RayStream
{
public:
    virtual void pushRay(int index, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) = 0;

    virtual void tracePushedRaysOcclusion(const modelinfo_t *self, int shadowmask) = 0;
    virtual bool getPushedRayOccluded(size_t j) const = 0;

    // Debug/Helpers
    virtual const qvec3f &getPushedRayDir(size_t j) const = 0;
};

class RayStreamIntersection : public virtual RayStream
{
public:
    virtual void pushRay(int index, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) = 0;

    virtual void tracePushedRaysIntersection(const modelinfo_t *self, int shadowmask) = 0;

    virtual const float getPushedRayHitDist(size_t j) const = 0;
    virtual hittype_t getPushedRayHitType(size_t j) const = 0;
    virtual const triinfo *getPushedRayHitFaceInfo(size_t j) const = 0;

    // Debug/Helpers
    virtual const qvec3f &getPushedRayDir(size_t j) const = 0;
};
