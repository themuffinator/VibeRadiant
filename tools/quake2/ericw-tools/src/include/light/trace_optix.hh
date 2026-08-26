#pragma once

#include <light/raystream.hh>
#include <vector>

struct mbsp_t;

void Optix_TraceInit(const mbsp_t *bsp);
void Optix_TraceCleanup();

class RayStreamIntersectionOptix : public RayStreamIntersection
{
public:
    RayStreamIntersectionOptix();
    virtual ~RayStreamIntersectionOptix();

    void resize(size_t size) override;
    void reserve(size_t size) override;
    void clearPushedRays() override;
    size_t numPushedRays() const override;

    const RayPayload &getPayload(size_t index) const override;

    void pushRay(int index, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) override;

    void tracePushedRaysIntersection(const modelinfo_t *self, int shadowmask) override;

    const float getPushedRayHitDist(size_t j) const override;
    hittype_t getPushedRayHitType(size_t j) const override;
    const triinfo *getPushedRayHitFaceInfo(size_t j) const override;

    const qvec3f &getPushedRayDir(size_t j) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

class RayStreamOcclusionOptix : public RayStreamOcclusion
{
public:
    RayStreamOcclusionOptix();
    virtual ~RayStreamOcclusionOptix();

    void resize(size_t size) override;
    void reserve(size_t size) override;
    void clearPushedRays() override;
    size_t numPushedRays() const override;

    const RayPayload &getPayload(size_t index) const override;

    void pushRay(int index, const qvec3f &origin, const qvec3f &dir, float dist, const qvec3f *color = nullptr,
        const qvec3f *normalcontrib = nullptr) override;

    void tracePushedRaysOcclusion(const modelinfo_t *self, int shadowmask) override;
    bool getPushedRayOccluded(size_t j) const override;

    const qvec3f &getPushedRayDir(size_t j) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
