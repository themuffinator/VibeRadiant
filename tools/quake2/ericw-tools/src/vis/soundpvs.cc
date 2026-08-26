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

#include <common/log.hh>
#include <vis/vis.hh>
#include <common/bsputils.hh>
#include <common/parallel.hh>

#include <limits>
#include <optional>
/*

Some textures (sky, water, slime, lava) are considered ambien sound emiters.
Find an aproximate distance to the nearest emiter of each class for each leaf.

*/

/*
  ====================
  SurfaceBBox
  ====================
*/
static aabb3d SurfaceBBox(const mbsp_t *bsp, const mface_t *surf)
{
    aabb3d bounds;

    for (size_t i = 0; i < static_cast<size_t>(surf->numedges); i++) {
        const int32_t signed_edge = bsp->dsurfedges[static_cast<size_t>(surf->firstedge) + i];
        const size_t edgenum = static_cast<size_t>(
            signed_edge < 0 ? -static_cast<int64_t>(signed_edge) : static_cast<int64_t>(signed_edge));
        const size_t vertnum = bsp->dedges[edgenum][signed_edge < 0 ? 1 : 0];

        bounds += bsp->dvertexes[vertnum];
    }

    return bounds;
}

static void ValidateAmbientSoundData(const mbsp_t &bsp)
{
    if (!bsp.loadversion || !bsp.loadversion->game) {
        FError("BSP has no valid source game for ambient sound calculation");
    }
    if (portalleafs_real < 0 || static_cast<size_t>(portalleafs_real) >= bsp.dleafs.size()) {
        FError("BSP has {} non-solid leaves, but vis expects {}", bsp.dleafs.empty() ? 0 : bsp.dleafs.size() - 1,
            portalleafs_real);
    }

    if (portalleafs <= 0 || leafbytes_real <= 0) {
        FError("invalid ambient visibility dimensions ({} clusters, {} bytes per row)", portalleafs, leafbytes_real);
    }

    const size_t row_bytes = static_cast<size_t>(leafbytes_real);
    const size_t required_bits = (static_cast<size_t>(portalleafs_real) + 7) >> 3;
    if (required_bits > row_bytes) {
        FError("ambient visibility rows are too short ({} bytes for {} leaves)", row_bytes, portalleafs_real);
    }
    if (static_cast<size_t>(portalleafs) > uncompressed.size() / row_bytes) {
        FError("ambient visibility data is truncated");
    }

    if (vis::extended_texinfo_flags.size() < bsp.texinfo.size()) {
        FError("extended texinfo flag table is truncated");
    }

    for (size_t leafnum = 1; leafnum <= static_cast<size_t>(portalleafs_real); ++leafnum) {
        const mleaf_t &leaf = bsp.dleafs[leafnum];
        if (portalleafs != portalleafs_real && (leaf.cluster < 0 || leaf.cluster >= portalleafs)) {
            FError("leaf {} has invalid visibility cluster {}", leafnum, leaf.cluster);
        }

        const size_t first_marksurface = leaf.firstmarksurface;
        const size_t marksurface_count = leaf.nummarksurfaces;
        if (first_marksurface > bsp.dleaffaces.size() ||
            marksurface_count > bsp.dleaffaces.size() - first_marksurface) {
            FError("leaf {} has an invalid marksurface range", leafnum);
        }

        for (size_t i = 0; i < marksurface_count; ++i) {
            const size_t marksurface = first_marksurface + i;
            const size_t facenum = bsp.dleaffaces[marksurface];
            if (facenum >= bsp.dfaces.size()) {
                FError("leaf {} marksurface {} references invalid face {}", leafnum, marksurface, facenum);
            }

            const mface_t &face = bsp.dfaces[facenum];
            if (face.texinfo < -1 || (face.texinfo >= 0 && static_cast<size_t>(face.texinfo) >= bsp.texinfo.size())) {
                FError("face {} has invalid texinfo {}", facenum, face.texinfo);
            }
            if (face.texinfo >= 0) {
                const mtexinfo_t &texinfo = bsp.texinfo[static_cast<size_t>(face.texinfo)];
                if (texinfo.miptex < -1 ||
                    (texinfo.miptex >= 0 && static_cast<size_t>(texinfo.miptex) >= bsp.dtex.textures.size())) {
                    FError("texinfo {} has invalid texture {}", face.texinfo, texinfo.miptex);
                }
            }

            if (face.firstedge < 0 || face.numedges <= 0 ||
                static_cast<size_t>(face.firstedge) > bsp.dsurfedges.size() ||
                static_cast<size_t>(face.numedges) > bsp.dsurfedges.size() - static_cast<size_t>(face.firstedge)) {
                FError("face {} has an invalid surface-edge range", facenum);
            }

            for (int edge_offset = 0; edge_offset < face.numedges; ++edge_offset) {
                const int32_t signed_edge = bsp.dsurfedges[static_cast<size_t>(face.firstedge) + edge_offset];
                if (signed_edge == std::numeric_limits<int32_t>::min()) {
                    FError("face {} references an invalid edge", facenum);
                }
                const size_t edge = static_cast<size_t>(
                    signed_edge < 0 ? -static_cast<int64_t>(signed_edge) : static_cast<int64_t>(signed_edge));
                if (edge >= bsp.dedges.size()) {
                    FError("face {} references invalid edge {}", facenum, edge);
                }
                const size_t vertex = bsp.dedges[edge][signed_edge < 0 ? 1 : 0];
                if (vertex >= bsp.dvertexes.size()) {
                    FError("edge {} references invalid vertex {}", edge, vertex);
                }
            }
        }
    }
}

struct ambient_source_t
{
    ambient_type_t type;
    aabb3d bounds;
};

static std::optional<ambient_type_t> AmbientTypeForTexture(const miptex_t &miptex, const mbsp_t &bsp)
{
    if (!Q_strncasecmp(miptex.name.data(), "sky", 3)) {
        if (!vis_options.noambientsky.value()) {
            return AMBIENT_SKY;
        }
        return std::nullopt;
    }
    if (!Q_strncasecmp(miptex.name.data(), "*water", 6) ||
        (!Q_strncasecmp(miptex.name.data(), "!water", 6) && bsp.loadversion->game->allows_hl_contents) ||
        !Q_strncasecmp(miptex.name.data(), "*04water", 6) ||
        (!Q_strncasecmp(miptex.name.data(), "!04water", 6) && bsp.loadversion->game->allows_hl_contents)) {
        if (!vis_options.noambientwater.value()) {
            return AMBIENT_WATER;
        }
        return std::nullopt;
    }
    if (!Q_strncasecmp(miptex.name.data(), "*slime", 6) ||
        (!Q_strncasecmp(miptex.name.data(), "!slime", 6) && bsp.loadversion->game->allows_hl_contents)) {
        // Keep compatibility with engines that only use water ambience.
        if (!vis_options.noambientslime.value()) {
            return AMBIENT_WATER;
        }
        return std::nullopt;
    }
    if (!Q_strncasecmp(miptex.name.data(), "*lava", 5) ||
        (!Q_strncasecmp(miptex.name.data(), "!lava", 5) && bsp.loadversion->game->allows_hl_contents)) {
        if (!vis_options.noambientlava.value()) {
            return AMBIENT_LAVA;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

static std::vector<std::vector<ambient_source_t>> BuildAmbientSources(const mbsp_t &bsp)
{
    std::vector<std::vector<ambient_source_t>> result(static_cast<size_t>(portalleafs_real));
    for (int leaf_index = 0; leaf_index < portalleafs_real; ++leaf_index) {
        const mleaf_t &leaf = bsp.dleafs[static_cast<size_t>(leaf_index) + 1];
        auto &sources = result[leaf_index];
        sources.reserve(leaf.nummarksurfaces);

        for (size_t offset = 0; offset < leaf.nummarksurfaces; ++offset) {
            const size_t facenum = bsp.dleaffaces[static_cast<size_t>(leaf.firstmarksurface) + offset];
            const mface_t &face = bsp.dfaces[facenum];
            if (face.texinfo < 0) {
                continue;
            }
            const mtexinfo_t &texinfo = bsp.texinfo[static_cast<size_t>(face.texinfo)];
            if (texinfo.miptex < 0 || vis::extended_texinfo_flags[face.texinfo].noambient) {
                continue;
            }
            const auto type = AmbientTypeForTexture(bsp.dtex.textures[static_cast<size_t>(texinfo.miptex)], bsp);
            if (type) {
                sources.push_back({*type, SurfaceBBox(&bsp, &face)});
            }
        }
    }
    return result;
}

/*
  ====================
  CalcAmbientSounds
  ====================
*/
void CalcAmbientSounds(mbsp_t *bsp)
{
    logging::funcheader();

    if (!bsp) {
        FError("can't calculate ambient sounds for a null BSP");
    }
    if (portalleafs_real < 0 || static_cast<size_t>(portalleafs_real) >= bsp->dleafs.size()) {
        FError("BSP has {} non-solid leaves, but vis expects {}", bsp->dleafs.empty() ? 0 : bsp->dleafs.size() - 1,
            portalleafs_real);
    }

    // fast path for -noambient
    if (vis_options.noambientsky.value() && vis_options.noambientwater.value() && vis_options.noambientslime.value() &&
        vis_options.noambientlava.value()) {
        for (int i = 0; i < portalleafs_real; i++) {
            mleaf_t *leaf = &bsp->dleafs[i + 1];
            for (int j = 0; j < NUM_AMBIENTS; j++) {
                leaf->ambient_level[j] = 0;
            }
        }
        return;
    }

    ValidateAmbientSoundData(*bsp);
    const auto ambient_sources = BuildAmbientSources(*bsp);

    logging::parallel_for(0, portalleafs_real, [&bsp, &ambient_sources](int i) {
        mleaf_t *leaf = &bsp->dleafs[i + 1];

        float dists[NUM_AMBIENTS];

        //
        // clear ambients
        //
        for (int j = 0; j < NUM_AMBIENTS; j++)
            dists[j] = 1020;

        uint8_t *vis;
        if (portalleafs != portalleafs_real) {
            vis = &uncompressed[leaf->cluster * leafbytes_real];
        } else {
            vis = &uncompressed[i * leafbytes_real];
        }

        for (int j = 0; j < portalleafs_real; j++) {
            if (!(vis[j >> 3] & nth_bit(j & 7)))
                continue;

            //
            // check this leaf for sound textures
            //
            for (const ambient_source_t &source : ambient_sources[j]) {
                // find distance from source leaf to polygon
                float maxd = 0;
                for (int l = 0; l < 3; l++) {
                    float d;
                    if (source.bounds.mins()[l] > leaf->maxs[l])
                        d = source.bounds.mins()[l] - leaf->maxs[l];
                    else if (source.bounds.maxs()[l] < leaf->mins[l])
                        d = leaf->mins[l] - source.bounds.maxs()[l];
                    else
                        d = 0;
                    if (d > maxd)
                        maxd = d;
                }

                if (maxd < dists[source.type])
                    dists[source.type] = maxd;
            }
        }

        for (int j = 0; j < NUM_AMBIENTS; j++) {
            float vol;
            if (dists[j] < 100)
                vol = 1.0;
            else {
                vol = (double)(1.0 - dists[j] * 0.002);
                if (vol < 0)
                    vol = 0;
            }
            leaf->ambient_level[j] = (uint8_t)(vol * 255);
        }
    });
}

/*
================
CalcPHS

Calculate the PHS (Potentially Hearable Set)
by ORing together all the PVS visible from a leaf
================
*/
void CalcPHS(mbsp_t *bsp, std::span<const uint8_t> cached_pvs, size_t cached_stride)
{
    logging::funcheader();

    if (portalleafs <= 0) {
        FError("can't calculate PHS without visibility clusters");
    }
    if (bsp->dvis.bit_offsets.size() < static_cast<size_t>(portalleafs) || bsp->dvis.bits.empty()) {
        FError("BSP has incomplete PVS data");
    }

    const size_t row_bytes = (static_cast<size_t>(portalleafs) + 7) >> 3;

    constexpr size_t fixed_header_size = sizeof(int32_t);
    constexpr size_t cluster_header_size = sizeof(int32_t) * 2;
    if (bsp->dvis.bit_offsets.size() >
        (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - fixed_header_size) / cluster_header_size) {
        FError("PHS header exceeds the BSP offset limit");
    }
    const size_t header_size = fixed_header_size + bsp->dvis.bit_offsets.size() * cluster_header_size;
    const size_t max_bits_size = static_cast<size_t>(std::numeric_limits<int32_t>::max()) - header_size;
    if (bsp->dvis.bits.size() > max_bits_size) {
        FError("PVS data exceeds the BSP offset limit");
    }

    std::vector<uint8_t> phs_row(row_bytes);
    std::vector<uint8_t> pvs_row(row_bytes);
    std::vector<uint8_t> compressed;
    compressed.reserve(row_bytes * 2);
    std::vector<uint8_t> original_pvs(row_bytes);

    const bool has_cached_pvs = !cached_pvs.empty();
    if (has_cached_pvs) {
        if (cached_stride < row_bytes ||
            static_cast<size_t>(portalleafs) > std::numeric_limits<size_t>::max() / cached_stride ||
            cached_pvs.size() != static_cast<size_t>(portalleafs) * cached_stride) {
            FError("cached PVS data has inconsistent dimensions");
        }
    } else if (cached_stride != 0) {
        FError("cached PVS stride was provided without cached data");
    }

    auto load_pvs_row = [&](int32_t cluster, std::vector<uint8_t> &output) {
        if (cluster < 0 || cluster >= portalleafs) {
            FError("invalid PVS cluster {}", cluster);
        }
        if (has_cached_pvs) {
            const uint8_t *source = cached_pvs.data() + static_cast<size_t>(cluster) * cached_stride;
            std::copy_n(source, row_bytes, output.begin());
            return;
        }
        const int32_t offset = bsp->dvis.get_bit_offset(VIS_PVS, cluster);
        if (offset < 0 || static_cast<size_t>(offset) >= bsp->dvis.bits.size()) {
            FError("cluster {} has invalid PVS offset {}", cluster, offset);
        }
        const uint8_t *scan = bsp->dvis.bits.data() + offset;
        if (!DecompressVis(
                scan, bsp->dvis.bits.data() + bsp->dvis.bits.size(), output.data(), output.data() + output.size())) {
            FError("cluster {} has malformed PVS data", cluster);
        }
    };

    // A -phsonly input generally already contains an older PHS. Rebuild the
    // visibility payload transactionally so repeated runs replace that data
    // instead of appending another unreachable copy every time.
    mvis_t rebuilt_vis;
    const bool rebuild_vis = vis_options.phsonly.value();
    if (rebuild_vis) {
        rebuilt_vis.resize(static_cast<size_t>(portalleafs));
        rebuilt_vis.bits.reserve(std::min(bsp->dvis.bits.size(), max_bits_size));
        for (int32_t cluster = 0; cluster < portalleafs; ++cluster) {
            load_pvs_row(cluster, pvs_row);
            compressed.clear();
            CompressRow(pvs_row.data(), row_bytes, std::back_inserter(compressed));
            if (compressed.size() > max_bits_size - rebuilt_vis.bits.size()) {
                FError("PVS data exceeds the BSP offset limit");
            }
            rebuilt_vis.set_bit_offset(VIS_PVS, cluster, rebuilt_vis.bits.size());
            rebuilt_vis.bits.insert(rebuilt_vis.bits.end(), compressed.begin(), compressed.end());
        }
    } else if (bsp->dvis.bits.size() <= max_bits_size / 2 && bsp->dvis.bits.size() <= bsp->dvis.bits.max_size() / 2) {
        // Increase capacity for the PHS when it can be doubled safely.
        bsp->dvis.bits.reserve(bsp->dvis.bits.size() * 2);
    }
    mvis_t &output_vis = rebuild_vis ? rebuilt_vis : bsp->dvis;

    int64_t count = 0;
    for (int32_t i = 0; i < portalleafs; i++) {
        load_pvs_row(i, phs_row);
        std::copy(phs_row.begin(), phs_row.end(), original_pvs.begin());

        const uint8_t *scan = original_pvs.data();

        for (size_t j = 0; j < row_bytes; j++) {
            uint8_t bitbyte = scan[j];
            if (!bitbyte)
                continue;
            for (int32_t k = 0; k < 8; k++) {
                if (!(bitbyte & nth_bit(k)))
                    continue;
                // OR this pvs row into the phs
                const size_t index = ((j << 3) + k);
                if (index >= portalleafs)
                    FError("Bad bit in PVS"); // pad bits should be 0
                load_pvs_row(static_cast<int32_t>(index), pvs_row);
                for (size_t byte = 0; byte < row_bytes; ++byte)
                    phs_row[byte] |= pvs_row[byte];
            }
        }
        for (int32_t j = 0; j < portalleafs; j++)
            if (phs_row[j >> 3] & nth_bit(j & 7))
                count++;

        //
        // compress the bit string
        //
        compressed.clear();
        CompressRow(phs_row.data(), row_bytes, std::back_inserter(compressed));

        if (compressed.size() > max_bits_size - output_vis.bits.size()) {
            FError("PHS data exceeds the BSP offset limit");
        }
        output_vis.set_bit_offset(VIS_PHS, i, output_vis.bits.size());

        output_vis.bits.insert(output_vis.bits.end(), compressed.begin(), compressed.end());
    }

    fmt::print("Average clusters hearable: {}\n", count / static_cast<int64_t>(portalleafs));

    output_vis.bits.shrink_to_fit();
    if (rebuild_vis) {
        bsp->dvis = std::move(rebuilt_vis);
    }
}
