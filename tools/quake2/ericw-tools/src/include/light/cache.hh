#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <map>

#include <common/bspfile.hh>
#include <light/light.hh>
#include <light/entities.hh>

struct light_face_cache_t
{
    uint64_t light_hash;
    // We store the raw lightmap samples.
    // light_surfaces[i].samples
    std::vector<lightsample_t> samples;
    // Also store bspx/decoupled data if needed?
    // For now let's focus on the main lightmap samples.

    // Actually, we store "lightmaps" (the final result) or "samples" (intermediate)?
    // The previous plan said "skip expensive raytracing". Raytracing produces 'samples'.
    // So caching 'samples' is the right level.
};

struct light_cache_t
{
    uint64_t bsp_checksum;

    // Map face index to cache data
    std::map<int, light_face_cache_t> faces;
};

bool LoadCache(const std::string &filename, light_cache_t &cache);
bool SaveCache(const std::string &filename, const light_cache_t &cache);
uint64_t CalculateBSPGeoHash(const mbsp_t *bsp);
uint64_t CalculateLightHash(const lightsurf_t *surf, const std::vector<std::unique_ptr<light_t>> &lights);
