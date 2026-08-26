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

#include "common/log.hh"

#include <common/bspxfile.hh>

#include <common/cmdlib.hh>

#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace
{
constexpr size_t BSPX_BRUSH_FACE_SIZE = sizeof(float) * 4;
constexpr size_t BSPX_BRUSH_HEADER_SIZE = (sizeof(float) * 6) + (sizeof(int16_t) * 2);
constexpr size_t BSPX_BRUSH_MODEL_HEADER_SIZE = sizeof(int32_t) * 4;
constexpr size_t BSPX_FACE_NORMAL_SIZE = sizeof(float) * 3;
constexpr size_t BSPX_FACE_NORMAL_INDICES_SIZE = sizeof(uint32_t) * 3;
constexpr size_t LIGHTGRID_HEADER_SIZE =
    (sizeof(float) * 6) + (sizeof(int32_t) * 3) + sizeof(uint8_t) + sizeof(uint32_t);
constexpr size_t LIGHTGRID_NODE_SIZE = (sizeof(int32_t) * 3) + (sizeof(uint32_t) * 8);
constexpr size_t LIGHTGRID_LEAF_HEADER_SIZE = sizeof(int32_t) * 6;
constexpr size_t LIGHTGRIDS_LEAF_HEADER_SIZE = LIGHTGRID_LEAF_HEADER_SIZE + sizeof(uint8_t);
constexpr size_t LIGHTGRID_SUBGRID_MIN_SIZE = LIGHTGRID_HEADER_SIZE + (sizeof(uint32_t) * 2);
constexpr uint32_t LIGHTGRID_FLAG_LEAF = uint32_t{1} << 31;
constexpr uint32_t LIGHTGRID_FLAG_OCCLUDED = uint32_t{1} << 30;

void invalidate_stream(std::istream &stream)
{
    stream.setstate(std::ios_base::failbit);
}

std::optional<size_t> stream_bytes_remaining(std::istream &stream)
{
    if (!stream) {
        return std::nullopt;
    }

    const std::streampos current = stream.tellg();
    if (current == std::streampos(-1)) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios_base::end);
    const std::streampos end = stream.tellg();
    if (!stream || end == std::streampos(-1) || end < current) {
        invalidate_stream(stream);
        return std::nullopt;
    }

    stream.seekg(current);
    if (!stream) {
        return std::nullopt;
    }

    const auto difference = static_cast<uintmax_t>(static_cast<std::streamoff>(end - current));
    if (difference > std::numeric_limits<size_t>::max()) {
        invalidate_stream(stream);
        return std::nullopt;
    }
    return static_cast<size_t>(difference);
}

bool stream_has_bytes(std::istream &stream, size_t count)
{
    const auto remaining = stream_bytes_remaining(stream);
    if (!remaining || count > *remaining) {
        invalidate_stream(stream);
        return false;
    }
    return true;
}

bool count_fits_stream(std::istream &stream, size_t count, size_t minimum_record_size)
{
    if (minimum_record_size != 0 && count > std::numeric_limits<size_t>::max() / minimum_record_size) {
        invalidate_stream(stream);
        return false;
    }
    return stream_has_bytes(stream, count * minimum_record_size);
}

template<typename SerializedCount>
SerializedCount checked_serialized_count(size_t count, std::string_view description)
{
    if (count > static_cast<size_t>(std::numeric_limits<SerializedCount>::max())) {
        FError("{} count {} exceeds the serialized format limit", description, count);
    }
    return static_cast<SerializedCount>(count);
}

std::optional<size_t> checked_grid_volume(const qvec3i &size)
{
    size_t volume = 1;
    for (const int32_t dimension : size) {
        if (dimension < 0) {
            return std::nullopt;
        }

        const size_t value = static_cast<size_t>(dimension);
        if (value != 0 && volume > std::numeric_limits<size_t>::max() / value) {
            return std::nullopt;
        }
        volume *= value;
    }
    return volume;
}

bool validate_lightgrid_header(std::istream &stream, const lightgrid_header_t &header)
{
    constexpr double max_float = static_cast<double>(std::numeric_limits<float>::max());
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(header.grid_dist[axis]) || header.grid_dist[axis] <= 0.0f || header.grid_size[axis] <= 0 ||
            !std::isfinite(header.grid_mins[axis])) {
            invalidate_stream(stream);
            return false;
        }

        const double grid_max =
            static_cast<double>(header.grid_mins[axis]) +
            (static_cast<double>(header.grid_dist[axis]) * static_cast<double>(header.grid_size[axis] - 1));
        if (!std::isfinite(grid_max) || grid_max < -max_float || grid_max > max_float) {
            invalidate_stream(stream);
            return false;
        }
    }
    return true;
}

template<typename Leaf>
bool validate_lightgrid_leaf_bounds(
    std::istream &stream, const lightgrid_header_t &header, const std::vector<Leaf> &leafs)
{
    for (const auto &leaf : leafs) {
        for (size_t axis = 0; axis < 3; ++axis) {
            if (leaf.mins[axis] < 0 || leaf.size[axis] <= 0 || leaf.mins[axis] >= header.grid_size[axis] ||
                leaf.size[axis] > header.grid_size[axis] - leaf.mins[axis]) {
                invalidate_stream(stream);
                return false;
            }
        }
    }
    return true;
}

bool validate_lightgrid_tree(
    std::istream &stream, uint32_t root_node, const std::vector<lightgrid_node_t> &nodes, size_t leaf_count)
{
    const auto referenced_node = [&](uint32_t reference) -> std::optional<size_t> {
        if (reference == LIGHTGRID_FLAG_OCCLUDED) {
            return std::nullopt;
        }

        if ((reference & LIGHTGRID_FLAG_OCCLUDED) != 0) {
            invalidate_stream(stream);
            return std::nullopt;
        }

        if ((reference & LIGHTGRID_FLAG_LEAF) != 0) {
            const size_t leaf_index = static_cast<size_t>(reference & ~LIGHTGRID_FLAG_LEAF);
            if (leaf_index >= leaf_count) {
                invalidate_stream(stream);
            }
            return std::nullopt;
        }

        const size_t node_index = static_cast<size_t>(reference);
        if (node_index >= nodes.size()) {
            invalidate_stream(stream);
            return std::nullopt;
        }
        return node_index;
    };

    referenced_node(root_node);
    for (const auto &node : nodes) {
        for (const uint32_t child : node.children) {
            referenced_node(child);
        }
    }
    if (!stream) {
        return false;
    }

    // Validate the entire directed node graph, including unreachable nodes.
    // An explicit stack prevents a deeply nested malformed tree from
    // overflowing the process stack during validation.
    std::vector<uint8_t> state(nodes.size(), 0);
    std::vector<std::pair<size_t, size_t>> stack;
    for (size_t start = 0; start < nodes.size(); ++start) {
        if (state[start] != 0) {
            continue;
        }

        state[start] = 1;
        stack.emplace_back(start, 0);
        while (!stack.empty()) {
            auto &[node_index, next_child] = stack.back();
            if (next_child == nodes[node_index].children.size()) {
                state[node_index] = 2;
                stack.pop_back();
                continue;
            }

            const uint32_t child_reference = nodes[node_index].children[next_child++];
            const auto child_node = referenced_node(child_reference);
            if (!stream || !child_node) {
                if (!stream) {
                    return false;
                }
                continue;
            }

            if (state[*child_node] == 1) {
                invalidate_stream(stream);
                return false;
            }
            if (state[*child_node] == 0) {
                state[*child_node] = 1;
                stack.emplace_back(*child_node, 0);
            }
        }
    }

    return true;
}
} // namespace

// bspx_header_t

bspx_header_t::bspx_header_t(uint32_t numlumps)
    : numlumps(numlumps)
{
}

void bspx_header_t::stream_write(std::ostream &s) const
{
    s <= std::tie(id, numlumps);
}

void bspx_header_t::stream_read(std::istream &s)
{
    s >= std::tie(id, numlumps);
}

// bspx_lump_t

void bspx_lump_t::stream_write(std::ostream &s) const
{
    s <= std::tie(lumpname, fileofs, filelen);
}

void bspx_lump_t::stream_read(std::istream &s)
{
    s >= std::tie(lumpname, fileofs, filelen);
}

// bspxbrushes_perbrush

void bspxbrushes_perbrush::stream_write(std::ostream &s) const
{
    const uint16_t face_count = checked_serialized_count<uint16_t>(faces.size(), "BRUSHLIST brush face");
    s <= bounds;
    s <= contents;
    s <= face_count;

    for (auto &face : faces) {
        s <= face;
    }
}

void bspxbrushes_perbrush::stream_read(std::istream &s)
{
    aabb3f parsed_bounds{};
    int16_t parsed_contents = 0;
    uint16_t numfaces = 0;

    s >= parsed_bounds;
    s >= parsed_contents;
    s >= numfaces;
    if (!s || !count_fits_stream(s, numfaces, BSPX_BRUSH_FACE_SIZE)) {
        return;
    }

    std::vector<bspxbrushes_perface> parsed_faces(numfaces);
    for (auto &face : parsed_faces) {
        s >= face;
        if (!s) {
            return;
        }
    }

    bounds = parsed_bounds;
    contents = parsed_contents;
    faces = std::move(parsed_faces);
}

// bspxbrushes_permodel

void bspxbrushes_permodel::stream_write(std::ostream &s) const
{
    const int32_t brush_count = checked_serialized_count<int32_t>(brushes.size(), "BRUSHLIST brush");
    size_t face_count = 0;
    for (const auto &brush : brushes) {
        checked_serialized_count<uint16_t>(brush.faces.size(), "BRUSHLIST brush face");
        if (brush.faces.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) - face_count) {
            FError("BRUSHLIST face count exceeds the serialized format limit");
        }
        face_count += brush.faces.size();
    }

    s <= ver;
    s <= modelnum;
    s <= brush_count;
    // Count faces from the brushes and ignore the cached numfaces member.
    s <= static_cast<int32_t>(face_count);

    // next serialize all of the brushes
    for (auto &brush : brushes) {
        s <= brush;
    }
}

void bspxbrushes_permodel::stream_read(std::istream &s)
{
    int32_t parsed_ver = 0;
    int32_t parsed_modelnum = 0;
    int32_t numbrushes = 0;
    int32_t parsed_numfaces = 0;

    s >= parsed_ver;
    s >= parsed_modelnum;
    s >= numbrushes;
    s >= parsed_numfaces;
    if (!s) {
        return;
    }

    if (numbrushes < 0 || parsed_numfaces < 0 ||
        !count_fits_stream(s, static_cast<size_t>(numbrushes), BSPX_BRUSH_HEADER_SIZE)) {
        invalidate_stream(s);
        return;
    }

    std::vector<bspxbrushes_perbrush> parsed_brushes;
    parsed_brushes.reserve(static_cast<size_t>(numbrushes));
    size_t actual_numfaces = 0;
    for (int32_t i = 0; i < numbrushes; ++i) {
        auto &brush = parsed_brushes.emplace_back();
        s >= brush;
        if (!s) {
            return;
        }

        if (brush.faces.size() > static_cast<size_t>(parsed_numfaces) - actual_numfaces) {
            invalidate_stream(s);
            return;
        }
        actual_numfaces += brush.faces.size();
    }
    if (actual_numfaces != static_cast<size_t>(parsed_numfaces)) {
        invalidate_stream(s);
        return;
    }

    ver = parsed_ver;
    modelnum = parsed_modelnum;
    numfaces = parsed_numfaces;
    brushes = std::move(parsed_brushes);
}

// bspxbrushes

void bspxbrushes::stream_write(std::ostream &s) const
{
    for (auto &model : models) {
        s <= model;
    }
}

void bspxbrushes::stream_read(std::istream &s)
{
    std::vector<bspxbrushes_permodel> parsed_models;

    while (true) {
        const auto remaining = stream_bytes_remaining(s);
        if (!remaining) {
            return;
        }
        if (*remaining == 0) {
            models = std::move(parsed_models);
            return;
        }
        if (*remaining < BSPX_BRUSH_MODEL_HEADER_SIZE) {
            invalidate_stream(s);
            return;
        }

        bspxbrushes_permodel model;
        s >= model;

        if (!s) {
            return;
        }

        parsed_models.push_back(std::move(model));
    }
}

// bspxfacenormals_per_vert

void bspxfacenormals_per_vert::stream_write(std::ostream &s) const
{
    s <= std::tie(normal, tangent, bitangent);
}

void bspxfacenormals_per_vert::stream_read(std::istream &s)
{
    s >= std::tie(normal, tangent, bitangent);
}

// bspxfacenormals_per_face

void bspxfacenormals_per_face::stream_write(std::ostream &s) const
{
    for (const auto &v : per_vert) {
        s <= v;
    }
}

void bspxfacenormals_per_face::stream_read(std::istream &s, const mface_t &f)
{
    if (f.numedges < 0 || !count_fits_stream(s, static_cast<size_t>(f.numedges), BSPX_FACE_NORMAL_INDICES_SIZE)) {
        invalidate_stream(s);
        return;
    }

    std::vector<bspxfacenormals_per_vert> parsed_vertices;
    parsed_vertices.reserve(static_cast<size_t>(f.numedges));
    for (int32_t i = 0; i < f.numedges; ++i) {
        bspxfacenormals_per_vert v{};
        s >= v;
        if (!s) {
            return;
        }
        parsed_vertices.push_back(v);
    }

    per_vert = std::move(parsed_vertices);
}

// bspxfacenormals

void bspxfacenormals::stream_write(std::ostream &s) const
{
    // write the table of normals
    s <= checked_serialized_count<uint32_t>(normals.size(), "FACENORMALS normal");

    for (const qvec3f &v : normals) {
        s <= v;
    }

    // write the per-face, per-vertex indices into the prior table
    for (const auto &f : per_face) {
        s <= f;
    }
}

void bspxfacenormals::stream_read(std::istream &s, const mbsp_t &bsp)
{
    // read normals table
    uint32_t normal_count = 0;
    s >= normal_count;
    if (!s || !count_fits_stream(s, normal_count, BSPX_FACE_NORMAL_SIZE)) {
        return;
    }

    std::vector<qvec3f> parsed_normals;
    parsed_normals.reserve(normal_count);
    for (uint32_t i = 0; i < normal_count; ++i) {
        qvec3f v{};
        s >= v;
        if (!s) {
            return;
        }
        parsed_normals.push_back(v);
    }

    // read, based on the faces in the provided bsp
    size_t total_vertices = 0;
    for (const auto &face : bsp.dfaces) {
        if (face.numedges < 0 ||
            static_cast<size_t>(face.numedges) > (std::numeric_limits<size_t>::max() - total_vertices)) {
            invalidate_stream(s);
            return;
        }
        total_vertices += static_cast<size_t>(face.numedges);
    }
    if (!count_fits_stream(s, total_vertices, BSPX_FACE_NORMAL_INDICES_SIZE)) {
        return;
    }

    std::vector<bspxfacenormals_per_face> parsed_faces;
    parsed_faces.reserve(bsp.dfaces.size());
    for (const auto &f : bsp.dfaces) {
        bspxfacenormals_per_face pf;
        pf.stream_read(s, f);
        if (!s) {
            return;
        }

        for (const auto &indices : pf.per_vert) {
            if (indices.normal >= parsed_normals.size() || indices.tangent >= parsed_normals.size() ||
                indices.bitangent >= parsed_normals.size()) {
                invalidate_stream(s);
                return;
            }
        }
        parsed_faces.push_back(std::move(pf));
    }

    normals = std::move(parsed_normals);
    per_face = std::move(parsed_faces);
}

// bspx_decoupled_lm_perface

void bspx_decoupled_lm_perface::stream_write(std::ostream &s) const
{
    s <= std::tie(lmwidth, lmheight, offset, world_to_lm_space);
}

void bspx_decoupled_lm_perface::stream_read(std::istream &s)
{
    s >= std::tie(lmwidth, lmheight, offset, world_to_lm_space);
}

// LIGHTGRID_OCTREE

// lightgrid_header_t

void lightgrid_header_t::stream_write(std::ostream &s) const
{
    s <= std::tie(grid_dist, grid_size, grid_mins, num_styles, root_node);
}

void lightgrid_header_t::stream_read(std::istream &s)
{
    s >= std::tie(grid_dist, grid_size, grid_mins, num_styles, root_node);
}

// lightgrid_node_t

void lightgrid_node_t::stream_write(std::ostream &s) const
{
    s <= std::tie(division_point, children);
}

void lightgrid_node_t::stream_read(std::istream &s)
{
    s >= std::tie(division_point, children);
}

// bspx_lightgrid_samples_t

void bspx_lightgrid_samples_t::stream_write(std::ostream &s) const
{
    if (occluded) {
        // occluded marker
        s <= static_cast<uint8_t>(0xff);
    } else {
        if (used_samples > samples_by_style.size()) {
            FError("LIGHTGRID_OCTREE sample set exceeds its in-memory style capacity");
        }
        s <= used_samples;

        for (int j = 0; j < static_cast<int>(used_samples); ++j) {
            s <= samples_by_style[j].style;
            s <= samples_by_style[j].color;
        }
    }
}

void bspx_lightgrid_samples_t::stream_read(std::istream &s)
{
    uint8_t used_styles_in = 0;
    s >= used_styles_in;
    if (!s) {
        return;
    }

    bspx_lightgrid_samples_t parsed{};

    if (used_styles_in == 0xff) {
        parsed.occluded = true;
        *this = parsed;
        // point is occluded, no color data follows
        return;
    }

    constexpr size_t sample_size = sizeof(uint8_t) + (sizeof(uint8_t) * 3);
    if (!count_fits_stream(s, used_styles_in, sample_size)) {
        return;
    }

    // point is unoccluded, 0 or more style/color pairs follow
    for (int j = 0; j < used_styles_in; ++j) {
        bspx_lightgrid_sample_t sample{};
        s >= sample.style;
        s >= sample.color;
        if (!s) {
            return;
        }

        if (!parsed.insert(sample)) {
            logging::print(
                "WARNING: LIGHTGRID_OCTREE exceeds implementation limit of {} styles\n", samples_by_style.size());
        }
    }

    *this = parsed;
}

// lightgrid_leaf_t

const bspx_lightgrid_samples_t &lightgrid_leaf_t::at(int x, int y, int z) const
{
    const auto expected_samples = checked_grid_volume(size);
    if (!expected_samples || samples.size() != *expected_samples || x < 0 || y < 0 || z < 0 || x >= size[0] ||
        y >= size[1] || z >= size[2]) {
        FError("invalid LIGHTGRID_OCTREE leaf dimensions or sample coordinate");
    }

    const size_t index = ((static_cast<size_t>(z) * static_cast<size_t>(size[1])) + static_cast<size_t>(y)) *
                             static_cast<size_t>(size[0]) +
                         static_cast<size_t>(x);
    return samples.at(index);
}

qvec3f lightgrid_leaf_t::world_pos(const lightgrid_header_t &header, int x, int y, int z) const
{
    qvec3i grid_coord = mins + qvec3i(x, y, z);

    return header.grid_mins + (qvec3f(grid_coord) * header.grid_dist);
}

void lightgrid_leaf_t::stream_write(std::ostream &s) const
{
    const auto expected_samples = checked_grid_volume(size);
    if (!expected_samples || samples.size() != *expected_samples) {
        FError("LIGHTGRID_OCTREE leaf sample count does not match its dimensions");
    }

    s <= std::tie(mins, size);
    for (const auto &sample : samples) {
        s <= sample;
    }
}

void lightgrid_leaf_t::stream_read(std::istream &s)
{
    qvec3i parsed_mins{};
    qvec3i parsed_size{};
    s >= std::tie(parsed_mins, parsed_size);
    if (!s) {
        return;
    }

    const auto sample_count = checked_grid_volume(parsed_size);
    if (!sample_count || !count_fits_stream(s, *sample_count, sizeof(uint8_t))) {
        invalidate_stream(s);
        return;
    }

    std::vector<bspx_lightgrid_samples_t> parsed_samples;
    parsed_samples.reserve(*sample_count);
    for (size_t i = 0; i < *sample_count; ++i) {
        auto &sample = parsed_samples.emplace_back();
        sample.stream_read(s);
        if (!s) {
            return;
        }
    }

    mins = parsed_mins;
    size = parsed_size;
    samples = std::move(parsed_samples);
}

// lightgrid_octree_t

void lightgrid_octree_t::stream_write(std::ostream &s) const
{
    const uint32_t node_count = checked_serialized_count<uint32_t>(nodes.size(), "LIGHTGRID_OCTREE node");
    const uint32_t leaf_count = checked_serialized_count<uint32_t>(leafs.size(), "LIGHTGRID_OCTREE leaf");

    s <= header;

    s <= node_count;
    for (const auto &node : nodes)
        s <= node;

    s <= leaf_count;
    for (const auto &leaf : leafs)
        s <= leaf;
}

void lightgrid_octree_t::stream_read(std::istream &s)
{
    lightgrid_header_t parsed_header{};
    s >= parsed_header;
    if (!s || !validate_lightgrid_header(s, parsed_header)) {
        return;
    }

    uint32_t num_nodes = 0;
    s >= num_nodes;
    if (!s || !count_fits_stream(s, num_nodes, LIGHTGRID_NODE_SIZE)) {
        return;
    }

    std::vector<lightgrid_node_t> parsed_nodes;
    parsed_nodes.reserve(num_nodes);
    for (uint32_t i = 0; i < num_nodes; ++i) {
        lightgrid_node_t &node = parsed_nodes.emplace_back();
        s >= node;
        if (!s) {
            return;
        }
    }

    uint32_t num_leafs = 0;
    s >= num_leafs;
    if (!s || !count_fits_stream(s, num_leafs, LIGHTGRID_LEAF_HEADER_SIZE)) {
        return;
    }

    std::vector<lightgrid_leaf_t> parsed_leafs;
    parsed_leafs.reserve(num_leafs);
    for (uint32_t i = 0; i < num_leafs; ++i) {
        lightgrid_leaf_t &leaf = parsed_leafs.emplace_back();
        s >= leaf;
        if (!s) {
            return;
        }
    }

    if (!validate_lightgrid_leaf_bounds(s, parsed_header, parsed_leafs) ||
        !validate_lightgrid_tree(s, parsed_header.root_node, parsed_nodes, parsed_leafs.size())) {
        return;
    }

    header = parsed_header;
    nodes = std::move(parsed_nodes);
    leafs = std::move(parsed_leafs);
}

// LIGHTGRIDS lump

// lightgrids_sampleset_t

void lightgrids_sampleset_t::stream_write(std::ostream &s) const
{
    if (occluded) {
        // occluded marker
        s <= static_cast<uint8_t>(0xff);
        return;
    }

    if (used_samples > samples_by_style.size()) {
        FError("LIGHTGRIDS sample set exceeds its in-memory style capacity");
    }
    s <= used_samples;

    for (int j = 0; j < used_samples; ++j) {
        const lightgrids_sample_t &sample = samples_by_style[j];
        s <= sample.style;

        // determine the flags
        uint8_t flags = 0;
        for (int side_index = 0; side_index < 6; ++side_index) {
            if (sample.colors[side_index] != qvec3b(0, 0, 0)) {
                flags |= (1 << side_index);
            }
        }

        // write the flags, then write the corresponding sides' colors out
        s <= flags;

        for (int side_index = 0; side_index < 6; ++side_index) {
            if (flags & (1 << side_index)) {
                s <= sample.colors[side_index];
            }
        }
    }
}

void lightgrids_sampleset_t::stream_read(std::istream &s)
{
    uint8_t used_styles_in = 0;
    s >= used_styles_in;
    if (!s) {
        return;
    }

    lightgrids_sampleset_t parsed{};

    if (used_styles_in == 0xff) {
        parsed.occluded = true;
        *this = parsed;
        // point is occluded, no color data follows
        return;
    }

    // Every sample has at least a style byte and a flags byte. Individual
    // color payloads are checked after the flags are read.
    if (!count_fits_stream(s, used_styles_in, sizeof(uint8_t) * 2)) {
        return;
    }

    // point is unoccluded, `used_styles_in` cubes follow
    for (int j = 0; j < used_styles_in; ++j) {
        lightgrids_sample_t sample{};
        s >= sample.style;

        // there are 0 to 6 color samples, for the faces of a cube.
        // they're always given in the following order:
        //
        // index:        0,  1,  2,  3,  4,  5
        // cube normal: +x, -x, +y, -y, +z, -z
        //
        // if `flags & (1 << index)` is set, it means that index is included.
        // if they're omitted, it means the cube is black on that side.
        //
        // e.g. 0b101 means we'd read the +x color, then the +y color, and assume
        // all other faces of the cube are black.
        uint8_t flags = 0;
        s >= flags;
        if (!s) {
            return;
        }

        size_t color_count = 0;
        for (int side_index = 0; side_index < 6; ++side_index) {
            if (flags & (1 << side_index)) {
                ++color_count;
            }
        }
        if (!count_fits_stream(s, color_count, sizeof(uint8_t) * 3)) {
            return;
        }

        for (int side_index = 0; side_index < 6; ++side_index) {
            if (flags & (1 << side_index)) {
                s >= sample.colors[side_index];
                if (!s) {
                    return;
                }
            }
        }

        if (!parsed.insert(sample)) {
            logging::print("WARNING: LIGHTGRIDS exceeds implementation limit of {} styles\n", samples_by_style.size());
        }
    }

    *this = parsed;
}

// lightgrids_leaf_t

const lightgrids_sampleset_t &lightgrids_leaf_t::at(int x, int y, int z) const
{
    const auto expected_samples = checked_grid_volume(size);
    if (!expected_samples || samples.size() != *expected_samples || x < 0 || y < 0 || z < 0 || x >= size[0] ||
        y >= size[1] || z >= size[2]) {
        FError("invalid LIGHTGRIDS leaf dimensions or sample coordinate");
    }

    const size_t index = ((static_cast<size_t>(z) * static_cast<size_t>(size[1])) + static_cast<size_t>(y)) *
                             static_cast<size_t>(size[0]) +
                         static_cast<size_t>(x);
    return samples.at(index);
}

qvec3f lightgrids_leaf_t::world_pos(const lightgrid_header_t &header, int x, int y, int z) const
{
    qvec3i grid_coord = mins + qvec3i(x, y, z);

    return header.grid_mins + (qvec3f(grid_coord) * header.grid_dist);
}

void lightgrids_leaf_t::stream_write(std::ostream &s) const
{
    const auto expected_samples = checked_grid_volume(size);
    if (!expected_samples || *expected_samples != samples.size()) {
        FError("LIGHTGRIDS leaf sample count does not match its dimensions");
    }
    for (const auto &sampleset : samples) {
        if (sampleset.used_samples > sampleset.samples_by_style.size()) {
            FError("LIGHTGRIDS sample set exceeds its in-memory style capacity");
        }
    }

    // compute max_styles
    uint8_t max_styles = 0;
    for (auto &sampleset : samples) {
        max_styles = std::max(max_styles, sampleset.used_samples);
    }

    s <= std::tie(mins, size);
    s <= max_styles;

    // write samples
    for (auto &sampleset : samples) {
        s <= sampleset;
    }
}

void lightgrids_leaf_t::stream_read(std::istream &s)
{
    qvec3i parsed_mins{};
    qvec3i parsed_size{};
    s >= std::tie(parsed_mins, parsed_size);

    uint8_t max_styles = 0; // unused
    s >= max_styles;
    if (!s) {
        return;
    }

    const auto sample_count = checked_grid_volume(parsed_size);
    if (!sample_count || !count_fits_stream(s, *sample_count, sizeof(uint8_t))) {
        invalidate_stream(s);
        return;
    }

    std::vector<lightgrids_sampleset_t> parsed_samples(*sample_count);
    for (lightgrids_sampleset_t &sampleset : parsed_samples) {
        s >= sampleset;
        if (!s) {
            return;
        }
    }

    mins = parsed_mins;
    size = parsed_size;
    samples = std::move(parsed_samples);
}

// subgrid_t

void subgrid_t::stream_write(std::ostream &s) const
{
    const uint32_t node_count = checked_serialized_count<uint32_t>(nodes.size(), "LIGHTGRIDS node");
    const uint32_t leaf_count = checked_serialized_count<uint32_t>(leafs.size(), "LIGHTGRIDS leaf");

    s <= header;

    s <= node_count;
    for (const lightgrid_node_t &node : nodes)
        s <= node;

    s <= leaf_count;
    for (const lightgrids_leaf_t &leaf : leafs)
        s <= leaf;
}

void subgrid_t::stream_read(std::istream &s)
{
    lightgrid_header_t parsed_header{};
    s >= parsed_header;
    if (!s || !validate_lightgrid_header(s, parsed_header)) {
        return;
    }

    uint32_t num_nodes = 0;
    s >= num_nodes;
    if (!s || !count_fits_stream(s, num_nodes, LIGHTGRID_NODE_SIZE)) {
        return;
    }

    std::vector<lightgrid_node_t> parsed_nodes;
    parsed_nodes.reserve(num_nodes);
    for (uint32_t i = 0; i < num_nodes; ++i) {
        auto &node = parsed_nodes.emplace_back();
        s >= node;
        if (!s) {
            return;
        }
    }

    uint32_t num_leafs = 0;
    s >= num_leafs;
    if (!s || !count_fits_stream(s, num_leafs, LIGHTGRIDS_LEAF_HEADER_SIZE)) {
        return;
    }

    std::vector<lightgrids_leaf_t> parsed_leafs;
    parsed_leafs.reserve(num_leafs);
    for (uint32_t i = 0; i < num_leafs; ++i) {
        auto &leaf = parsed_leafs.emplace_back();
        s >= leaf;
        if (!s) {
            return;
        }
    }

    if (!validate_lightgrid_leaf_bounds(s, parsed_header, parsed_leafs) ||
        !validate_lightgrid_tree(s, parsed_header.root_node, parsed_nodes, parsed_leafs.size())) {
        return;
    }

    header = parsed_header;
    nodes = std::move(parsed_nodes);
    leafs = std::move(parsed_leafs);
}

// lightgrids_t

void lightgrids_t::stream_write(std::ostream &s) const
{
    for (const auto &lightgrid : subgrids) {
        const std::streampos begin_pos = s.tellp();
        if (begin_pos == std::streampos(-1)) {
            FError("unable to determine LIGHTGRIDS subgrid start position");
        }

        // write a placeholder for the size, we'll overwrite after.
        s <= static_cast<uint32_t>(0);

        // write the lightgrid itself
        s <= lightgrid;

        const std::streampos end_pos = s.tellp();
        if (!s || end_pos == std::streampos(-1) || end_pos < begin_pos) {
            FError("unable to determine LIGHTGRIDS subgrid size");
        }
        const std::streamoff lightgrid_size =
            static_cast<std::streamoff>(end_pos - begin_pos) - static_cast<std::streamoff>(sizeof(uint32_t));
        if (lightgrid_size < 0 || static_cast<uintmax_t>(lightgrid_size) > std::numeric_limits<uint32_t>::max()) {
            FError("LIGHTGRIDS subgrid exceeds the serialized format limit");
        }

        // seek back to start and overwrite the placeholder with the actual size
        s.seekp(begin_pos);
        s <= static_cast<uint32_t>(lightgrid_size);

        s.seekp(end_pos);
        if (!s) {
            FError("unable to finalize LIGHTGRIDS subgrid size");
        }
    }
}

void lightgrids_t::stream_read(std::istream &s)
{
    std::vector<subgrid_t> parsed_subgrids;

    while (true) {
        const auto remaining = stream_bytes_remaining(s);
        if (!remaining) {
            return;
        }
        if (*remaining == 0) {
            subgrids = std::move(parsed_subgrids);
            return;
        }
        if (*remaining < sizeof(uint32_t)) {
            invalidate_stream(s);
            return;
        }

        uint32_t lightgrid_size_bytes = 0;
        s >= lightgrid_size_bytes;
        if (!s) {
            return;
        }

        const auto payload_remaining = stream_bytes_remaining(s);
        if (!payload_remaining || lightgrid_size_bytes < LIGHTGRID_SUBGRID_MIN_SIZE ||
            lightgrid_size_bytes > *payload_remaining || !std::in_range<std::streamsize>(lightgrid_size_bytes)) {
            invalidate_stream(s);
            return;
        }

        std::vector<uint8_t> subgrid_bytes(lightgrid_size_bytes);
        if (!subgrid_bytes.empty()) {
            const auto requested = static_cast<std::streamsize>(subgrid_bytes.size());
            s.read(reinterpret_cast<char *>(subgrid_bytes.data()), requested);
            if (!s || s.gcount() != requested) {
                invalidate_stream(s);
                return;
            }
        }

        imemstream subgrid_stream(subgrid_bytes.data(), subgrid_bytes.size());
        subgrid_stream >> endianness<std::endian::little>;
        subgrid_t parsed_subgrid{};
        subgrid_stream >= parsed_subgrid;

        if (!subgrid_stream || subgrid_stream.tellg() != static_cast<std::streamoff>(subgrid_bytes.size())) {
            invalidate_stream(s);
            return;
        }

        parsed_subgrids.push_back(std::move(parsed_subgrid));
    }
}
