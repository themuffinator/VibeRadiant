#include <optix.h>

#include "cuda_runtime.h"
#include <vector_types.h>

enum RayType
{
    RAY_TYPE_RADIANCE = 0,
    RAY_TYPE_OCCLUSION = 1,
    RAY_TYPE_COUNT
};

struct SampleRayData
{
    int index;
    float3 color;
    float3 normalcontrib;
    int hit_type; // 0=none, 1=solid, 2=sky
    int geomID;
    int primID;
    float tfar;

    // Glass
    int hit_glass;
    float3 glass_color;
    float glass_opacity;

    // Dynamic shadow
    int dynamic_style;
};

struct OcclusionRayData
{
    int occluded;
};

extern "C" __global__ void __raygen__rg()
{
    const uint3 idx = optixGetLaunchIndex();
    const uint3 dim = optixGetLaunchDimensions();

    const unsigned int index = idx.y * dim.x + idx.x;

    // Params are passed via global constant buffer usually, or SBT
    // For simplicity assuming we have a launch param buffer pointer
}

extern "C" __global__ void __miss__ms()
{
    // Miss program
    SampleRayData *prd = (SampleRayData *)optixGetPayload_0_ptr();
    if (prd) {
        prd->hit_type = 0; // NONE
        prd->tfar = -1.0f;
    }
}

extern "C" __global__ void __closesthit__ch()
{
    // Closest hit
    const int primID = optixGetPrimitiveIndex();
    const int geomID = optixGetInstanceId();
    // Wait, geomID in Embree is Geometry ID in scene. In OptiX it's usually Instance ID or GAS handle.
    // We'll trust the build logic to map them.

    SampleRayData *prd = (SampleRayData *)optixGetPayload_0_ptr();
    if (prd) {
        prd->hit_type = 1; // SOLID (default)
        prd->primID = primID;
        prd->geomID = geomID;
        prd->tfar = optixGetRayTmax();
    }
}

extern "C" __global__ void __anyhit__ah()
{
    // Alpha test
    // Need access to texture coordinates and textures.
    // This requires significant binding setup.
    // For now, implement pass-through for opaque.
}

extern "C" __global__ void __miss__occlusion()
{
    OcclusionRayData *prd = (OcclusionRayData *)optixGetPayload_0_ptr();
    prd->occluded = 0;
}

extern "C" __global__ void __closesthit__occlusion()
{
    OcclusionRayData *prd = (OcclusionRayData *)optixGetPayload_0_ptr();
    prd->occluded = 1;
}
