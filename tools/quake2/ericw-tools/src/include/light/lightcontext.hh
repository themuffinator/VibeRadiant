#pragma once

#include <vector>
#include <memory>
#include <unordered_map>
#include <span>

#include "light/light.hh"
#include "common/bspfile.hh"
#include "common/bspxfile.hh"
#include "common/imglib.hh"
#include "light/write.hh" // facesup_t

class modelinfo_t;
struct lightsurf_t;
struct mbsp_t;

namespace vibey
{
namespace light
{

struct LightContext
{
    // Options
    const settings::light_settings *options = nullptr;

    // BSP Integration
    mbsp_t *bsp = nullptr;

    // Data Structures
    std::vector<modelinfo_t *> modelinfo;
    std::vector<const modelinfo_t *> tracelist;
    std::vector<const modelinfo_t *> selfshadowlist;
    std::vector<const modelinfo_t *> shadowworldonlylist;
    std::vector<const modelinfo_t *> switchableshadowlist;

    std::vector<surfflags_t> extended_texinfo_flags;
    std::vector<contentflags_t> extended_content_flags;

    // Face Data
    struct face_texture_cache
    {
        const img::texture *image;
        qvec3b averageColor;
        qvec3f bounceColor;
    };
    std::vector<face_texture_cache> face_textures;

    std::vector<facesup_t> faces_sup;
    std::vector<bspx_decoupled_lm_perface> facesup_decoupled_global;

    // Light Surfaces
    std::unique_ptr<lightsurf_t[]> light_surfaces;
    std::span<lightsurf_t> light_surfaces_span;
    std::vector<lightsurf_t *> emissive_light_surfaces;

    // Visibility
    std::unordered_map<int, std::vector<uint8_t>> uncompressed_vis;

    // State
    bool dirt_in_use = false;
    int dump_facenum = -1;
    int dump_vertnum = -1;

    // Constructor
    explicit LightContext(const settings::light_settings *opts, mbsp_t *b)
        : options(opts),
          bsp(b)
    {
    }

    // Non-copyable
    LightContext(const LightContext &) = delete;
    LightContext &operator=(const LightContext &) = delete;
};

// Global accessor for legacy support
extern LightContext *g_ctx;

} // namespace light
} // namespace vibey
