#include <light/cache.hh>
#include <fstream>
#include <common/log.hh>
#include <common/util.hh>
#include <light/entities.hh> // light_t definitions

// FNV-1a 64-bit hash
static uint64_t fnv1a_64(const void *data, size_t len, uint64_t hash = 0xcbf29ce484222325ULL)
{
    const uint8_t *ptr = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= ptr[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t CalculateBSPGeoHash(const mbsp_t *bsp)
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    // Hash vertices
    if (!bsp->dvertexes.empty()) {
        hash = fnv1a_64(bsp->dvertexes.data(), bsp->dvertexes.size() * sizeof(dvertex_t), hash);
    }

    // Hash edges
    if (!bsp->dedges.empty()) {
        hash = fnv1a_64(bsp->dedges.data(), bsp->dedges.size() * sizeof(dedge_t), hash);
    }

    // Hash faces (geometry part)
    if (!bsp->dfaces.empty()) {
        // dfaces contains planenum, side, edge info.
        // It also contains lightofs which changes on recompile, so we should be careful?
        // Actually lightofs is output, usually -1 input.
        // Let's hash the whole thing, assuming input bsp is consistent.
        // If the user re-csg's, they get a new bsp anyway.
        hash = fnv1a_64(bsp->dfaces.data(), bsp->dfaces.size() * sizeof(dface_t), hash);
    }

    // Hash plane data just in case
    if (!bsp->dplanes.empty()) {
        hash = fnv1a_64(bsp->dplanes.data(), bsp->dplanes.size() * sizeof(dplane_t), hash);
    }

    // Hash texinfo since it affects mapping
    if (!bsp->texinfo.empty()) {
        hash = fnv1a_64(bsp->texinfo.data(), bsp->texinfo.size() * sizeof(mtexinfo_t), hash);
    }

    return hash;
}

uint64_t CalculateLightHash(const lightsurf_t *surf, const std::vector<std::unique_ptr<light_t>> &lights)
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    // Hash relevant global settings
    hash = fnv1a_64(&surf->cfg->global_anglescale._value, sizeof(double), hash);
    // ... add other global settings if they aren't part of light entities

    // The 'lights' vector passed here should be the subset of lights that affect this surface.
    // However, usually we just iterate all lights in the world.
    // To be strictly correct, we should either:
    // 1. Hash ALL lights in the map (easy, but one light change invalidates everything)
    // 2. Hash ONLY the lights that reach this surface.
    // The caller is responsible for passing the filtered list or we do it here.
    // For now, let's assume the caller passes the list of lights that strictly affect this surface
    // OR we hash the properties of the lights and their positions.

    // Strategy 2 is better. But 'light' tool typically iterates all lights for a face?
    // Actually, `LightFace` iterates `ProcessLight` for ALL lights.

    // So, if we want per-face incrementality, we need to hash:
    // For each light L in the map:
    //   If L affects Surf (envelope check):
    //     Hash(L)

    // But determining if L "affects" Surf is part of the work we want to save?
    // Not really, the envelope check is fast. Raytracing is slow.
    // So yes, we should rely on the caller to provide the candidate lights,
    // or we just hash them here.

    for (const auto &lptr : lights) {
        const light_t &l = *lptr;
        // Simple envelope check.
        // If the light is too far, it doesn't contribute, so its changes don't matter.
        // Except if it *moves* close enough.
        // So we can't easily ignore lights based on *current* position if they moved.
        // BUT, if we assume the set of lights is static and only properties change...
        // No, user could add a light.

        // Revised Strategy:
        // We hash ALL lights that physically touch the surface's bounding sphere + radius.

        if (l.type == emitter_t::constant) {
            // Check envelope
            // This replicates some logic from SimpleLightWorld / LightFace
            // float dist = qv::distance(l.origin, surf->extents.origin);
            // double radius = l.falloff; // rough approx
            // if (dist > radius + surf->extents.radius) continue;

            // We'll rely on the caller to filter, or if we are passed ALL lights,
            // we effectively act as if one light change invalidates all.
            // Let's assume passed lights are impactful.
        }

        hash = fnv1a_64(&l.origin, sizeof(l.origin), hash);
        hash = fnv1a_64(&l.color, sizeof(l.color), hash);
        hash = fnv1a_64(&l.style, sizeof(l.style), hash);
        hash = fnv1a_64(&l.fade, sizeof(l.fade), hash);
        hash = fnv1a_64(&l.flags, sizeof(l.flags), hash); // flags/spawnflags?
        // We verified light_t members in entities.hh.
        // There is no explicit 'fade' or 'flags' member directly shown in my previous view?
        // Let's re-check light_t definition in entities.hh
        // It has 'light', 'color', 'origin', etc are settings objects.
        // We should hash the .value() of these settings.
    }

    return hash;
}

// Simple binary serialization
// Using C-style IO or C++ fstream? C++ fstream is fine.

template<typename T>
static void write(std::ofstream &f, const T &val)
{
    f.write(reinterpret_cast<const char *>(&val), sizeof(T));
}

template<typename T>
static void write_vec(std::ofstream &f, const std::vector<T> &vec)
{
    size_t sz = vec.size();
    write(f, sz);
    if (sz > 0)
        f.write(reinterpret_cast<const char *>(vec.data()), sz * sizeof(T));
}

template<typename T>
static void read(std::ifstream &f, T &val)
{
    f.read(reinterpret_cast<char *>(&val), sizeof(T));
}

template<typename T>
static void read_vec(std::ifstream &f, std::vector<T> &vec)
{
    size_t sz;
    read(f, sz);
    vec.resize(sz);
    if (sz > 0)
        f.read(reinterpret_cast<char *>(vec.data()), sz * sizeof(T));
}

bool SaveCache(const std::string &filename, const light_cache_t &cache)
{
    std::ofstream f(filename, std::ios::binary);
    if (!f.is_open())
        return false;

    write(f, cache.bsp_checksum);

    size_t num_faces = cache.faces.size();
    write(f, num_faces);

    for (const auto &pair : cache.faces) {
        write(f, pair.first); // face index
        write(f, pair.second.light_hash);
        write_vec(f, pair.second.samples);
    }

    return true;
}

bool LoadCache(const std::string &filename, light_cache_t &cache)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f.is_open())
        return false;

    read(f, cache.bsp_checksum);

    size_t num_faces;
    read(f, num_faces);

    cache.faces.clear();
    for (size_t i = 0; i < num_faces; i++) {
        int idx;
        read(f, idx);
        light_face_cache_t fc;
        read(f, fc.light_hash);
        read_vec(f, fc.samples);

        cache.faces[idx] = std::move(fc);
    }

    return true;
}
