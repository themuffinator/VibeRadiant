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

#include <common/bspfile.hh>
#include <common/cmdlib.hh>
#include <common/log.hh>

#include <algorithm>
#include <limits>

// dmodelh2_t

void dmodelh2_t::stream_write(std::ostream &s) const
{
    s <= std::tie(mins, maxs, origin, headnode, visleafs, firstface, numfaces);
}

void dmodelh2_t::stream_read(std::istream &s)
{
    s >= std::tie(mins, maxs, origin, headnode, visleafs, firstface, numfaces);
}

// mvis_t

size_t mvis_t::header_offset() const
{
    constexpr size_t fixed_size = sizeof(int32_t);
    constexpr size_t entry_size = sizeof(int32_t) * 2;
    if (bit_offsets.size() > (std::numeric_limits<size_t>::max() - fixed_size) / entry_size) {
        FError("visibility header size overflow ({} clusters)", bit_offsets.size());
    }
    return fixed_size + (entry_size * bit_offsets.size());
}

void mvis_t::set_bit_offset(vistype_t type, size_t cluster, size_t offset)
{
    const size_t type_index = static_cast<size_t>(type);
    if (cluster >= bit_offsets.size() || type_index >= bit_offsets.front().size()) {
        FError("invalid visibility offset target (type {}, cluster {})", type_index, cluster);
    }

    const size_t header_size = header_offset();
    constexpr size_t max_offset = static_cast<size_t>(std::numeric_limits<int32_t>::max());
    if (header_size > max_offset || offset > max_offset - header_size) {
        FError("visibility data offset exceeds the signed BSP offset limit");
    }

    bit_offsets[cluster][type_index] = static_cast<int32_t>(header_size + offset);
}

int32_t mvis_t::get_bit_offset(vistype_t type, size_t cluster) const
{
    const size_t type_index = static_cast<size_t>(type);
    if (cluster >= bit_offsets.size() || type_index >= bit_offsets.front().size()) {
        FError("invalid visibility offset lookup (type {}, cluster {})", type_index, cluster);
    }

    const int32_t stored_offset = bit_offsets[cluster][type_index];
    if (stored_offset < 0) {
        return stored_offset;
    }

    const size_t header_size = header_offset();
    if (header_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        static_cast<size_t>(stored_offset) < header_size) {
        return -1;
    }

    return stored_offset - static_cast<int32_t>(header_size);
}

void mvis_t::resize(size_t numclusters)
{
    bit_offsets.resize(numclusters);
}

void mvis_t::stream_read(std::istream &stream, const lump_t &lump)
{
    constexpr size_t count_size = sizeof(int32_t);
    constexpr size_t offsets_per_cluster_size = sizeof(int32_t) * 2;

    if (lump.filelen < static_cast<int32_t>(count_size)) {
        FError("visibility lump is too small to contain a cluster count ({} bytes)", lump.filelen);
    }

    int32_t numclusters = 0;

    stream >= numclusters;
    if (!stream) {
        FError("unable to read visibility cluster count");
    }

    if (numclusters < 0) {
        FError("visibility lump has a negative cluster count ({})", numclusters);
    }

    const size_t lump_size = static_cast<size_t>(lump.filelen);
    const size_t cluster_count = static_cast<size_t>(numclusters);
    const size_t max_clusters = (lump_size - count_size) / offsets_per_cluster_size;
    if (cluster_count > max_clusters) {
        FError("visibility lump cluster table is truncated ({} clusters, {} byte lump)", cluster_count, lump_size);
    }

    const size_t header_size = count_size + (cluster_count * offsets_per_cluster_size);
    std::vector<std::array<int32_t, 2>> parsed_offsets(cluster_count);

    for (auto &bit_offset : parsed_offsets) {
        stream >= bit_offset;
        if (!stream) {
            FError("unable to read visibility cluster offsets");
        }

        for (const int32_t offset : bit_offset) {
            if (offset == -1) {
                continue;
            }
            if (offset < 0 || static_cast<size_t>(offset) < header_size || static_cast<size_t>(offset) >= lump_size) {
                FError("visibility lump contains an invalid data offset ({})", offset);
            }
        }
    }

    const size_t remaining = lump_size - header_size;
    std::vector<uint8_t> parsed_bits(remaining);
    if (remaining != 0) {
        stream.read(reinterpret_cast<char *>(parsed_bits.data()), static_cast<std::streamsize>(remaining));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(remaining)) {
            FError("visibility lump data is truncated");
        }
    }

    bit_offsets = std::move(parsed_offsets);
    bits = std::move(parsed_bits);
}

void mvis_t::stream_write(std::ostream &stream) const
{
    // no vis data
    if (bit_offsets.empty()) {
        return;
    }

    if (bit_offsets.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        FError("visibility cluster count {} exceeds the signed BSP limit", bit_offsets.size());
    }

    const size_t header_size = header_offset();
    constexpr size_t max_lump_size = static_cast<size_t>(std::numeric_limits<int32_t>::max());
    if (header_size > max_lump_size || bits.size() > max_lump_size - header_size) {
        FError("visibility lump exceeds the signed BSP size limit");
    }
    if (bits.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        FError("visibility bitset is too large for the output stream ({} bytes)", bits.size());
    }
    const size_t total_size = header_size + bits.size();
    for (const auto &bit_offset : bit_offsets) {
        for (const int32_t offset : bit_offset) {
            if (offset != -1 && (offset < 0 || static_cast<size_t>(offset) < header_size ||
                                    static_cast<size_t>(offset) >= total_size)) {
                FError("visibility lump contains an invalid serialized data offset ({})", offset);
            }
        }
    }

    stream <= static_cast<int32_t>(bit_offsets.size());

    // write cluster -> offset tables
    for (const auto &bit_offset : bit_offsets) {
        stream <= bit_offset;
    }

    // write bitset
    stream.write(reinterpret_cast<const char *>(bits.data()), static_cast<std::streamsize>(bits.size()));
    if (!stream) {
        FError("unable to write visibility lump");
    }
}

// dmiptex_t

void dmiptex_t::stream_write(std::ostream &s) const
{
    s <= std::tie(name, width, height, offsets);
}

void dmiptex_t::stream_read(std::istream &s)
{
    s >= std::tie(name, width, height, offsets);
}

// miptex_t

size_t miptex_t::stream_size() const
{
    return data.size();
}

void miptex_t::stream_read(std::istream &stream, size_t len)
{
    constexpr size_t serialized_header_size = 16 + (sizeof(uint32_t) * 2) + (sizeof(int32_t) * MIPLEVELS);
    if (len < serialized_header_size) {
        FError("texture record is too small to contain a miptex header ({} bytes)", len);
    }
    if (len > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        FError("texture record is too large to read ({} bytes)", len);
    }

    std::vector<uint8_t> parsed_data(len);
    stream.read(reinterpret_cast<char *>(parsed_data.data()), static_cast<std::streamsize>(len));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(len)) {
        FError("texture record is truncated (expected {} bytes)", len);
    }

    imemstream miptex_stream(parsed_data.data(), parsed_data.size());
    miptex_stream >> endianness<std::endian::little>;

    dmiptex_t dtex{};
    miptex_stream >= dtex;
    if (!miptex_stream) {
        FError("unable to read texture header");
    }

    for (size_t level = 0; level < dtex.offsets.size(); ++level) {
        const int32_t offset = dtex.offsets[level];
        if (offset <= 0) {
            continue;
        }

        const uint64_t mip_width = static_cast<uint64_t>(dtex.width >> level);
        const uint64_t mip_height = static_cast<uint64_t>(dtex.height >> level);
        const uint64_t mip_size = mip_width * mip_height;
        const size_t offset_value = static_cast<size_t>(offset);
        if (offset_value < serialized_header_size || offset_value > len || mip_size > len - offset_value) {
            FError("texture mip level {} exceeds its {} byte record", level, len);
        }
    }

    const auto name_end = std::find(dtex.name.begin(), dtex.name.end(), '\0');
    name.assign(dtex.name.begin(), name_end);
    width = dtex.width;
    height = dtex.height;
    offsets = dtex.offsets;
    data = std::move(parsed_data);
    null_texture = false;
}

void miptex_t::stream_write(std::ostream &stream) const
{
    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        FError("texture record is too large to serialize ({} bytes)", data.size());
    }
    stream.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) {
        FError("unable to write texture record");
    }
}

// dmiptexlump_t

void dmiptexlump_t::stream_read(std::istream &stream, const lump_t &lump)
{
    constexpr size_t count_size = sizeof(int32_t);
    constexpr size_t offset_size = sizeof(int32_t);
    constexpr size_t serialized_miptex_header_size = 16 + (sizeof(uint32_t) * 2) + (sizeof(int32_t) * MIPLEVELS);

    if (lump.filelen < static_cast<int32_t>(count_size)) {
        FError("texture lump is too small to contain a texture count ({} bytes)", lump.filelen);
    }

    int32_t nummiptex = 0;
    stream >= nummiptex;
    if (!stream) {
        FError("unable to read texture count");
    }

    if (nummiptex < 0) {
        FError("texture lump has a negative texture count ({})", nummiptex);
    }

    const size_t lump_size = static_cast<size_t>(lump.filelen);
    const size_t texture_count = static_cast<size_t>(nummiptex);
    const size_t max_textures = (lump_size - count_size) / offset_size;
    if (texture_count > max_textures) {
        FError("texture lump offset table is truncated ({} textures, {} byte lump)", texture_count, lump_size);
    }

    const size_t header_size = count_size + (texture_count * offset_size);

    // load in all of the offsets, we need them
    // to calculate individual data sizes.
    std::vector<int32_t> parsed_offsets(texture_count);

    for (int32_t &offset : parsed_offsets) {
        stream >= offset;
        if (!stream) {
            FError("unable to read texture lump offsets");
        }

        if (offset < 0) {
            // Negative offsets are the traditional marker for an omitted
            // texture. Preserve compatibility with maps that use values
            // other than exactly -1 for that marker.
            continue;
        }

        if (static_cast<size_t>(offset) < header_size || static_cast<size_t>(offset) >= lump_size) {
            FError("texture lump contains an invalid texture offset ({})", offset);
        }
    }

    std::vector<miptex_t> parsed_textures;
    parsed_textures.reserve(texture_count);

    for (size_t i = 0; i < texture_count; ++i) {
        miptex_t &tex = parsed_textures.emplace_back();

        const int32_t offset = parsed_offsets[i];

        // dummy texture?
        if (offset < 0) {
            tex.null_texture = true;
            continue;
        }

        // move to miptex position (technically required
        // because there might be dummy data between the offsets
        // and the mip textures themselves...)
        const auto absolute_offset = static_cast<std::streamoff>(lump.fileofs) + static_cast<std::streamoff>(offset);
        stream.seekg(absolute_offset);
        if (!stream) {
            FError("unable to seek to texture {} at lump offset {}", i, offset);
        }

        // Find the next texture in physical file order. Valid BSPs normally
        // list textures in order, but using the nearest greater offset also
        // handles reordered tables without reading across lump boundaries.
        size_t next_offset = lump_size;
        for (const int32_t candidate : parsed_offsets) {
            if (candidate > offset) {
                next_offset = std::min(next_offset, static_cast<size_t>(candidate));
            }
        }

        const size_t texture_size = next_offset - static_cast<size_t>(offset);
        if (texture_size < serialized_miptex_header_size) {
            FError("texture {} record is too small ({} bytes)", i, texture_size);
        }
        tex.stream_read(stream, texture_size);
    }

    textures = std::move(parsed_textures);
}

void dmiptexlump_t::stream_write(std::ostream &stream) const
{
    constexpr size_t count_size = sizeof(int32_t);
    constexpr size_t offset_size = sizeof(int32_t);
    constexpr size_t serialized_miptex_header_size = 16 + (sizeof(uint32_t) * 2) + (sizeof(int32_t) * MIPLEVELS);
    constexpr size_t max_lump_size = static_cast<size_t>(std::numeric_limits<int32_t>::max());

    const std::streampos start_position = stream.tellp();
    if (start_position == std::streampos(-1)) {
        FError("unable to determine texture lump output position");
    }
    const std::streamoff start_offset = static_cast<std::streamoff>(start_position);
    if (start_offset < 0) {
        FError("texture lump output position is negative");
    }

    if (textures.size() > (max_lump_size - count_size) / offset_size) {
        FError("texture count {} exceeds the signed BSP texture lump limit", textures.size());
    }
    const int32_t texture_count = static_cast<int32_t>(textures.size());
    const size_t header_size = count_size + (offset_size * textures.size());

    std::vector<int32_t> offsets;
    offsets.reserve(textures.size());
    std::vector<uint8_t> padding_before(textures.size(), 0);
    size_t relative_position = header_size;
    const size_t start_alignment = static_cast<size_t>(start_offset % 4);

    for (size_t i = 0; i < textures.size(); ++i) {
        const auto &texture = textures[i];
        if (texture.null_texture) {
            offsets.push_back(-1);
            continue;
        }

        if (texture.stream_size() < serialized_miptex_header_size) {
            FError("texture {} record is too small to serialize ({} bytes)", i, texture.stream_size());
        }
        if (texture.stream_size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            FError("texture {} record is too large to serialize ({} bytes)", i, texture.stream_size());
        }

        const size_t absolute_alignment = (start_alignment + (relative_position % 4)) % 4;
        const size_t padding = (4 - absolute_alignment) % 4;
        if (padding > max_lump_size - relative_position) {
            FError("texture lump padding exceeds the signed BSP texture lump limit");
        }
        relative_position += padding;
        padding_before[i] = static_cast<uint8_t>(padding);

        offsets.push_back(static_cast<int32_t>(relative_position));
        if (texture.stream_size() > max_lump_size - relative_position) {
            FError("texture {} data exceeds the signed BSP texture lump limit", i);
        }
        relative_position += texture.stream_size();
    }

    const auto max_stream_offset = static_cast<uintmax_t>(std::numeric_limits<std::streamoff>::max());
    if (relative_position > max_stream_offset ||
        static_cast<uintmax_t>(start_offset) > max_stream_offset - relative_position) {
        FError("texture lump output position exceeds the stream position limit");
    }

    stream <= texture_count;
    for (const int32_t offset : offsets) {
        stream <= offset;
    }
    if (!stream) {
        FError("unable to write texture lump metadata");
    }

    constexpr char zero_padding[4]{};
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto &texture = textures[i];
        if (!texture.null_texture) {
            const auto padding = static_cast<std::streamsize>(padding_before[i]);
            if (padding != 0) {
                stream.write(zero_padding, padding);
            }
            texture.stream_write(stream);
            if (!stream) {
                FError("unable to write texture {} data", i);
            }
        }
    }
}

size_t dmiptexlump_t::stream_size() const
{
    omemsizestream stream;
    stream_write(stream);
    return stream.tellp();
}

// dplane_t

void dplane_t::stream_write(std::ostream &s) const
{
    s <= std::tie(normal, dist, type);
}

void dplane_t::stream_read(std::istream &s)
{
    s >= std::tie(normal, dist, type);
}

// bsp2_dnode_t

void bsp2_dnode_t::stream_write(std::ostream &s) const
{
    s <= std::tie(planenum, children, mins, maxs, firstface, numfaces);
}
void bsp2_dnode_t::stream_read(std::istream &s)
{
    s >= std::tie(planenum, children, mins, maxs, firstface, numfaces);
}

// mface_t

void mface_t::stream_write(std::ostream &s) const
{
    s <= std::tie(planenum, side, firstedge, numedges, texinfo, styles, lightofs);
}
void mface_t::stream_read(std::istream &s)
{
    s >= std::tie(planenum, side, firstedge, numedges, texinfo, styles, lightofs);
}

// bsp2_dclipnode_t

void bsp2_dclipnode_t::stream_write(std::ostream &s) const
{
    s <= std::tie(planenum, children);
}

void bsp2_dclipnode_t::stream_read(std::istream &s)
{
    s >= std::tie(planenum, children);
}

// mleaf_t

static auto tuple(const mleaf_t &l)
{
    return std::tie(l.contents, l.visofs, l.mins, l.maxs, l.firstmarksurface, l.nummarksurfaces, l.ambient_level,
        l.cluster, l.area, l.firstleafbrush, l.numleafbrushes);
}

// darea_t

void darea_t::stream_write(std::ostream &s) const
{
    s <= std::tie(numareaportals, firstareaportal);
}

void darea_t::stream_read(std::istream &s)
{
    s >= std::tie(numareaportals, firstareaportal);
}

// dareaportal_t

void dareaportal_t::stream_write(std::ostream &s) const
{
    s <= std::tie(portalnum, otherarea);
}

void dareaportal_t::stream_read(std::istream &s)
{
    s >= std::tie(portalnum, otherarea);
}

// dbrush_t

void dbrush_t::stream_write(std::ostream &s) const
{
    s <= std::tie(firstside, numsides, contents);
}

void dbrush_t::stream_read(std::istream &s)
{
    s >= std::tie(firstside, numsides, contents);
}

// q2_dbrushside_qbism_t

void q2_dbrushside_qbism_t::stream_write(std::ostream &s) const
{
    s <= std::tie(planenum, texinfo);
}

void q2_dbrushside_qbism_t::stream_read(std::istream &s)
{
    s >= std::tie(planenum, texinfo);
}

// mbsp_t

size_t mbsp_t::lightsamples() const
{
    if (!loadversion || !loadversion->game) {
        FError("light sample count requires a valid BSP version");
    }
    if (loadversion->game->has_rgb_lightmap) {
        if (dlightdata.size() % 3 != 0) {
            FError("RGB light data size {} is not sample-aligned", dlightdata.size());
        }
        return dlightdata.size() / 3;
    } else {
        return dlightdata.size();
    }
}
