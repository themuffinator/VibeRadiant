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

#include <common/bspinfo.hh>
#include <common/log.hh>
#include <common/cmdlib.hh>
#include <common/bspfile.hh>
#include <common/bsputils.hh>
#include <common/ostream.hh>

#include <fstream>
#include <iomanip>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <fmt/core.h>
#include <common/json.hh>
#include "common/fs.hh"
#include "common/imglib.hh"
#include "common/litfile.hh"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../extern/stb_image_write.h"

static std::string hex_string(const uint8_t *bytes, const size_t count)
{
    std::string str;

    for (size_t i = 0; i < count; ++i) {
        fmt::format_to(std::back_inserter(str), "{:02x}", bytes[i]);
    }

    return str;
}

/**
 * returns a JSON array of models
 */
static Json::Value serialize_bspxbrushlist(const std::vector<uint8_t> &lump)
{
    Json::Value j = Json::Value(Json::arrayValue);

    imemstream p(lump.data(), lump.size(), std::ios_base::in | std::ios_base::binary);

    p >> endianness<std::endian::little>;
    bspxbrushes structured;
    p >= structured;
    if (!p) {
        FError("malformed BRUSHLIST lump");
    }

    for (const bspxbrushes_permodel &src_model : structured.models) {
        auto &model = j.append(Json::Value(Json::objectValue));
        model["ver"] = src_model.ver;
        model["modelnum"] = src_model.modelnum;
        model["numbrushes"] = static_cast<Json::UInt64>(src_model.brushes.size());
        model["numfaces"] = src_model.numfaces;
        auto &brushes = (model["brushes"] = Json::Value(Json::arrayValue));

        for (const bspxbrushes_perbrush &src_brush : src_model.brushes) {
            auto &brush = brushes.append(Json::Value(Json::objectValue));
            brush["mins"] = to_json(src_brush.bounds.mins());
            brush["maxs"] = to_json(src_brush.bounds.maxs());
            brush["contents"] = src_brush.contents;
            auto &faces = (brush["faces"] = Json::Value(Json::arrayValue));

            for (const bspxbrushes_perface &src_face : src_brush.faces) {
                auto &face = faces.append(Json::Value(Json::objectValue));
                face["normal"] = to_json(src_face.normal);
                face["dist"] = src_face.dist;
            }
        }
    }

    return j;
}

static Json::Value serialize_bspx_decoupled_lm(const std::vector<uint8_t> &lump)
{
    constexpr size_t serialized_face_size = (sizeof(uint16_t) * 2) + sizeof(int32_t) + (sizeof(float) * 8);
    if (lump.size() % serialized_face_size != 0) {
        FError("DECOUPLED_LM size {} is not a whole number of {}-byte face records", lump.size(), serialized_face_size);
    }

    auto j = Json::Value(Json::arrayValue);

    imemstream p(lump.data(), lump.size(), std::ios_base::in | std::ios_base::binary);

    p >> endianness<std::endian::little>;

    for (size_t face_index = 0; face_index < lump.size() / serialized_face_size; ++face_index) {
        bspx_decoupled_lm_perface src_face;
        p >= src_face;

        if (!p) {
            FError("DECOUPLED_LM face record {} is truncated", face_index);
        }
        BSPX_ValidateDecoupledLM(src_face, face_index);

        auto &model = j.append(Json::objectValue);
        model["lmwidth"] = src_face.lmwidth;
        model["lmheight"] = src_face.lmheight;
        model["offset"] = src_face.offset;
        model["world_to_lm_space"] =
            json_array({to_json(src_face.world_to_lm_space.row(0)), to_json(src_face.world_to_lm_space.row(1))});
    }

    return j;
}

/**
 * The MIT License (MIT)
 * Copyright (c) 2016 tomykaira
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
template<typename T>
static void Base64EncodeTo(const uint8_t *data, size_t in_len, T p)
{
    static constexpr char sEncodingTable[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
        'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k',
        'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6',
        '7', '8', '9', '+', '/'};

    if (in_len == 0)
        return;

    size_t i;

    if (in_len == 1) {
        *p++ = sEncodingTable[(data[0] >> 2) & 0x3F];
        *p++ = sEncodingTable[((data[0] & 0x3) << 4)];
        *p++ = '=';
        *p++ = '=';
        return;
    }

    if (in_len == 2) {
        *p++ = sEncodingTable[(data[0] >> 2) & 0x3F];
        *p++ = sEncodingTable[((data[0] & 0x3) << 4) | ((int)(data[1] & 0xF0) >> 4)];
        *p++ = sEncodingTable[((data[1] & 0xF) << 2)];
        *p++ = '=';
        return;
    }

    for (i = 0; i < in_len - 2; i += 3) {
        *p++ = sEncodingTable[(data[i] >> 2) & 0x3F];
        *p++ = sEncodingTable[((data[i] & 0x3) << 4) | ((int)(data[i + 1] & 0xF0) >> 4)];
        *p++ = sEncodingTable[((data[i + 1] & 0xF) << 2) | ((int)(data[i + 2] & 0xC0) >> 6)];
        *p++ = sEncodingTable[data[i + 2] & 0x3F];
    }
    if (i < in_len) {
        *p++ = sEncodingTable[(data[i] >> 2) & 0x3F];
        if (i == (in_len - 1)) {
            *p++ = sEncodingTable[((data[i] & 0x3) << 4)];
            *p++ = '=';
        } else {
            *p++ = sEncodingTable[((data[i] & 0x3) << 4) | ((int)(data[i + 1] & 0xF0) >> 4)];
            *p++ = sEncodingTable[((data[i + 1] & 0xF) << 2)];
        }
        *p++ = '=';
    }
}

static std::string serialize_image(const std::optional<img::texture> &texture_opt)
{
    if (!texture_opt) {
        FError("can't serialize image in BSP?");
    }

    auto &texture = texture_opt.value();
    if (texture.width == 0 || texture.height == 0) {
        FError("can't serialize an image with zero-sized dimensions {}x{}", texture.width, texture.height);
    }
    constexpr uint32_t components = 4;
    constexpr auto max_stbi_dimension = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (texture.height > max_stbi_dimension || texture.width > max_stbi_dimension / components) {
        FError("can't serialize image dimensions {}x{} with a representable PNG stride", texture.width, texture.height);
    }
    if (static_cast<size_t>(texture.height) > std::numeric_limits<size_t>::max() / texture.width) {
        FError("can't serialize image dimensions {}x{}: pixel count overflows", texture.width, texture.height);
    }
    const size_t expected_pixels = static_cast<size_t>(texture.width) * texture.height;
    if (texture.pixels.size() != expected_pixels) {
        FError("can't serialize image dimensions {}x{} from {} RGBA pixels (expected {})", texture.width,
            texture.height, texture.pixels.size(), expected_pixels);
    }

    const int width = static_cast<int>(texture.width);
    const int height = static_cast<int>(texture.height);
    const int stride = static_cast<int>(texture.width * components);
    std::vector<uint8_t> buf;
    const int write_succeeded = stbi_write_png_to_func(
        [](void *context, void *data, int size) {
            std::copy(reinterpret_cast<uint8_t *>(data), reinterpret_cast<uint8_t *>(data) + size,
                std::back_inserter(*reinterpret_cast<decltype(buf) *>(context)));
        },
        &buf, width, height, components, texture.pixels.data(), stride);
    if (write_succeeded == 0) {
        FError("failed to encode {}x{} image as PNG", texture.width, texture.height);
    }

    std::string str{"data:image/png;base64,"};

    Base64EncodeTo(buf.data(), buf.size(), std::back_inserter(str));

    return str;
}

static size_t checked_atlas_product(size_t lhs, size_t rhs, std::string_view description)
{
    if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
        FError("lightmap atlas {} size overflow", description);
    }

    return lhs * rhs;
}

static uint16_t read_little_u16(const std::vector<uint8_t> &bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

static uint32_t read_little_u32(const std::vector<uint8_t> &bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

static void finish_output_file(std::ofstream &stream, const fs::path &path)
{
    stream.flush();
    if (!stream) {
        FError("error writing {}", path);
    }

    stream.close();
    if (!stream) {
        FError("error closing {}", path);
    }
}

static void remove_staged_output(const fs::path &path)
{
    std::error_code ignored;
    if (fs::remove(path, ignored) || !ignored) {
        return;
    }

    ignored.clear();
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ignored);
    if (!ignored) {
        fs::remove(path, ignored);
    }
}

struct staged_output_cleanup_t
{
    explicit staged_output_cleanup_t(fs::path path)
        : path(std::move(path))
    {
    }

    ~staged_output_cleanup_t()
    {
        if (active) {
            remove_staged_output(path);
        }
    }

    void release() noexcept { active = false; }

    fs::path path;
    bool active = true;
};

static fs::file_status checked_output_status(const fs::path &path, std::string_view description)
{
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        FError("unable to inspect {} {}: {}", description, path, error.message());
    }
    return status;
}

static void ensure_output_path_available(const fs::path &path, std::string_view description)
{
    if (fs::exists(checked_output_status(path, description))) {
        FError("{} {} already exists", description, path);
    }
}

static void remove_output_backup(const fs::path &backup_path, const fs::path &output_path)
{
    std::error_code error;
    if (fs::remove(backup_path, error) || !error) {
        return;
    }

    std::error_code permission_error;
    fs::permissions(backup_path, fs::perms::owner_write, fs::perm_options::add, permission_error);
    if (!permission_error) {
        error.clear();
        if (fs::remove(backup_path, error) || !error) {
            return;
        }
    }

    FError("output {} was written, but its temporary backup {} could not be removed: {}", output_path, backup_path,
        error.message());
}

template<typename Writer>
static void write_output_file(const fs::path &path, std::ios_base::openmode mode, Writer &&writer)
{
    const fs::file_status destination_status = checked_output_status(path, "output path");
    const bool destination_exists = fs::exists(destination_status);
    if (destination_exists && !fs::is_regular_file(destination_status)) {
        FError("output path {} exists and is not a regular file", path);
    }

    fs::path temporary_path = path;
    temporary_path += ".tmp";
    ensure_output_path_available(temporary_path, "temporary output path");

    fs::path backup_path = path;
    backup_path += ".previous.tmp";
    ensure_output_path_available(backup_path, "backup output path");

    staged_output_cleanup_t staged_cleanup(temporary_path);

    std::ofstream stream(temporary_path, mode | std::ios_base::out | std::ios_base::trunc);
    if (!stream) {
        FError("unable to open {} for writing", temporary_path);
    }

    if (destination_exists && destination_status.permissions() != fs::perms::unknown) {
        std::error_code permission_error;
        fs::permissions(temporary_path, destination_status.permissions(), fs::perm_options::replace, permission_error);
        if (permission_error) {
            FError("unable to preserve permissions for {}: {}", path, permission_error.message());
        }
    }

    try {
        std::forward<Writer>(writer)(stream);
        finish_output_file(stream, temporary_path);
    } catch (...) {
        stream.close();
        throw;
    }

    std::error_code ec;
    if (destination_exists) {
        fs::rename(path, backup_path, ec);
        if (ec) {
            FError("unable to preserve existing output {}: {}", path, ec.message());
        }
    }

    fs::rename(temporary_path, path, ec);
    if (ec) {
        const std::string promotion_error = ec.message();

        if (destination_exists) {
            std::error_code rollback_error;
            fs::rename(backup_path, path, rollback_error);
            if (rollback_error) {
                FError("unable to install output {}: {}; restoring the previous output also failed: {}", path,
                    promotion_error, rollback_error.message());
            }
        }
        FError("unable to install output {}: {}", path, promotion_error);
    }

    staged_cleanup.release();

    if (destination_exists) {
        remove_output_backup(backup_path, path);
    }
}

static std::vector<std::vector<uint16_t>> load_atlas_face_styles(const mbsp_t &bsp, const bspxentries_t &bspx)
{
    constexpr size_t max_extended_styles = 16;
    const auto lmstyle = bspx.find("LMSTYLE");
    const auto lmstyle16 = bspx.find("LMSTYLE16");

    // Match BSPX consumers: prefer a usable LMSTYLE16 table, but fall back to
    // LMSTYLE when an older producer left a malformed LMSTYLE16 lump behind.
    const auto lmstyle16_has_valid_cardinality = [&]() {
        if (lmstyle16 == bspx.end()) {
            return false;
        }

        const auto &bytes = lmstyle16->second;
        if (bsp.dfaces.empty()) {
            return bytes.empty();
        }
        if (bsp.dfaces.size() > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) {
            return false;
        }

        const size_t bytes_per_face_denominator = bsp.dfaces.size() * sizeof(uint16_t);
        if (bytes.empty() || bytes.size() % bytes_per_face_denominator != 0) {
            return false;
        }

        const size_t styles_per_face = bytes.size() / bytes_per_face_denominator;
        return styles_per_face > 0 && styles_per_face <= max_extended_styles;
    };

    const bool use_lmstyle16 = lmstyle16 != bspx.end() && (lmstyle == bspx.end() || lmstyle16_has_valid_cardinality());

    std::vector<std::vector<uint16_t>> result(bsp.dfaces.size());

    if (use_lmstyle16) {
        const auto &bytes = lmstyle16->second;
        const size_t bytes_per_entry = sizeof(uint16_t);

        if (bsp.dfaces.empty()) {
            if (!bytes.empty()) {
                FError("LMSTYLE16 has {} bytes but the BSP has no faces", bytes.size());
            }
            return result;
        }

        const size_t bytes_per_face_denominator =
            checked_atlas_product(bsp.dfaces.size(), bytes_per_entry, "LMSTYLE16");
        if (bytes.empty() || bytes.size() % bytes_per_face_denominator != 0) {
            FError("LMSTYLE16 size {} is not a whole number of 16-bit styles for {} faces", bytes.size(),
                bsp.dfaces.size());
        }

        const size_t styles_per_face = bytes.size() / bytes_per_face_denominator;
        if (styles_per_face == 0 || styles_per_face > max_extended_styles) {
            FError("LMSTYLE16 has invalid cardinality of {} styles per face (maximum {})", styles_per_face,
                max_extended_styles);
        }

        for (size_t face_index = 0; face_index < bsp.dfaces.size(); ++face_index) {
            auto &styles = result[face_index];
            styles.reserve(styles_per_face);
            const size_t face_offset =
                checked_atlas_product(face_index, styles_per_face * bytes_per_entry, "LMSTYLE16");
            for (size_t style_index = 0; style_index < styles_per_face; ++style_index) {
                styles.push_back(read_little_u16(bytes, face_offset + (style_index * bytes_per_entry)));
            }
        }
    } else if (lmstyle != bspx.end()) {
        const auto &bytes = lmstyle->second;

        if (bsp.dfaces.empty()) {
            if (!bytes.empty()) {
                FError("LMSTYLE has {} bytes but the BSP has no faces", bytes.size());
            }
            return result;
        }
        if (bytes.empty() || bytes.size() % bsp.dfaces.size() != 0) {
            FError("LMSTYLE size {} is not a whole number of styles for {} faces", bytes.size(), bsp.dfaces.size());
        }

        const size_t styles_per_face = bytes.size() / bsp.dfaces.size();
        if (styles_per_face == 0 || styles_per_face > max_extended_styles) {
            FError("LMSTYLE has invalid cardinality of {} styles per face (maximum {})", styles_per_face,
                max_extended_styles);
        }

        for (size_t face_index = 0; face_index < bsp.dfaces.size(); ++face_index) {
            auto &styles = result[face_index];
            styles.reserve(styles_per_face);
            const size_t face_offset = checked_atlas_product(face_index, styles_per_face, "LMSTYLE");
            for (size_t style_index = 0; style_index < styles_per_face; ++style_index) {
                const uint8_t style = bytes[face_offset + style_index];
                styles.push_back(style == INVALID_LIGHTSTYLE_OLD ? std::numeric_limits<uint16_t>::max() : style);
            }
        }
    } else {
        for (size_t face_index = 0; face_index < bsp.dfaces.size(); ++face_index) {
            auto &styles = result[face_index];
            styles.reserve(MAXLIGHTMAPS);
            for (const uint8_t style : bsp.dfaces[face_index].styles) {
                styles.push_back(style == INVALID_LIGHTSTYLE_OLD ? std::numeric_limits<uint16_t>::max() : style);
            }
        }
    }

    for (size_t face_index = 0; face_index < result.size(); ++face_index) {
        auto &styles = result[face_index];
        const auto first_invalid = std::ranges::find(styles, std::numeric_limits<uint16_t>::max());
        if (std::ranges::find_if(first_invalid, styles.end(),
                [](uint16_t style) { return style != std::numeric_limits<uint16_t>::max(); }) != styles.end()) {
            FError("lightmap styles for face {} contain a style after the invalid terminator", face_index);
        }
        styles.erase(first_invalid, styles.end());

        std::set<uint16_t> unique_styles;
        for (const uint16_t style : styles) {
            if (!unique_styles.insert(style).second) {
                FError("lightmap styles for face {} contain duplicate style {}", face_index, style);
            }
        }
    }

    return result;
}

full_atlas_t build_lightmap_atlas(const mbsp_t &bsp, const bspxentries_t &bspx, const std::vector<uint8_t> &litdata,
    const std::vector<uint32_t> &hdr_litdata, bool use_bspx, bool use_decoupled)
{
    struct face_rect
    {
        const mface_t *face;
        faceextents_t extents;
        std::vector<uint16_t> styles;
        size_t sample_count = 0;
        size_t source_offset = 0;

        size_t atlas = 0;
        size_t x = 0, y = 0;
    };

    constexpr size_t atlas_size = 512;

    if (!bsp.loadversion || !bsp.loadversion->game) {
        FError("lightmap atlas requires a BSP with a valid source version");
    }

    bool is_hdr = false;
    const std::vector<uint32_t> *hdr_lightdata_source = nullptr; // 1 packed uint32 (e5brg9) per sample
    const std::vector<uint8_t> *lightdata_source =
        nullptr; // either greyscale (1 byte per sample) or rgb (3 bytes per sample)
    std::vector<uint32_t> decoded_bspx_hdr;
    bool is_rgb = false;
    bool is_lit = false;

    if (!hdr_litdata.empty()) {
        hdr_lightdata_source = &hdr_litdata;
        is_hdr = true;
    } else if (auto it = bspx.find("LIGHTING_E5BGR9"); it != bspx.end()) {
        const auto &bytes = it->second;
        if (bytes.size() % sizeof(uint32_t) != 0) {
            FError("LIGHTING_E5BGR9 size {} is not a whole number of 32-bit samples", bytes.size());
        }

        decoded_bspx_hdr.resize(bytes.size() / sizeof(uint32_t));
        for (size_t sample_index = 0; sample_index < decoded_bspx_hdr.size(); ++sample_index) {
            decoded_bspx_hdr[sample_index] = read_little_u32(bytes, sample_index * sizeof(uint32_t));
        }
        hdr_lightdata_source = &decoded_bspx_hdr;
        is_hdr = true;
    } else if (!litdata.empty()) {
        if (litdata.size() % 3 != 0) {
            FError("RGB .lit data size {} is not a whole number of samples", litdata.size());
        }
        is_lit = true;
        is_rgb = true;
        lightdata_source = &litdata;
    } else {
        is_lit = false;
        is_rgb = bsp.loadversion->game->has_rgb_lightmap;
        if (is_rgb && bsp.dlightdata.size() % 3 != 0) {
            FError("native RGB lightdata size {} is not a whole number of samples", bsp.dlightdata.size());
        }
        lightdata_source = &bsp.dlightdata;
    }

    struct atlas
    {
        size_t current_x = 0, current_y = 0;
        size_t tallest = 0;
    };

    std::vector<atlas> atlasses;
    std::vector<face_rect> rectangles;
    size_t current_atlas = 0;
    rectangles.reserve(bsp.dfaces.size());

    std::vector<bspx_decoupled_lm_perface> bspx_decoupled;
    if (use_decoupled && (bspx.find("DECOUPLED_LM") != bspx.end())) {
        constexpr size_t serialized_face_size = (sizeof(uint16_t) * 2) + sizeof(int32_t) + (sizeof(float) * 8);
        const auto &decoupled_lm = bspx.at("DECOUPLED_LM");
        const size_t expected_size = checked_atlas_product(bsp.dfaces.size(), serialized_face_size, "DECOUPLED_LM");
        if (decoupled_lm.size() != expected_size) {
            FError("DECOUPLED_LM size {} does not match {} faces (expected {} bytes)", decoupled_lm.size(),
                bsp.dfaces.size(), expected_size);
        }

        bspx_decoupled.reserve(bsp.dfaces.size());
        for (size_t i = 0; i < bsp.dfaces.size(); ++i) {
            bspx_decoupled.push_back(BSPX_DecoupledLM(bspx, static_cast<int>(i)));
        }
    } else {
        use_decoupled = false;
    }

    std::vector<int32_t> bspx_lmoffset;
    std::vector<uint8_t> bspx_lmshift;
    if (use_bspx && !use_decoupled) {
        const auto offset_it = bspx.find("LMOFFSET");
        const auto shift_it = bspx.find("LMSHIFT");
        if (offset_it == bspx.end()) {
            FError("lightmap atlas requested BSPX offsets but LMOFFSET is missing");
        }
        if (shift_it == bspx.end()) {
            FError("lightmap atlas requested BSPX extents but LMSHIFT is missing");
        }

        const size_t expected_offset_size = checked_atlas_product(bsp.dfaces.size(), sizeof(int32_t), "LMOFFSET");
        if (offset_it->second.size() != expected_offset_size) {
            FError("LMOFFSET size {} does not match {} faces (expected {} bytes)", offset_it->second.size(),
                bsp.dfaces.size(), expected_offset_size);
        }
        if (shift_it->second.size() != bsp.dfaces.size()) {
            FError("LMSHIFT size {} does not match {} faces", shift_it->second.size(), bsp.dfaces.size());
        }

        bspx_lmoffset.reserve(bsp.dfaces.size());
        for (size_t face_index = 0; face_index < bsp.dfaces.size(); ++face_index) {
            const uint32_t raw = read_little_u32(offset_it->second, face_index * sizeof(int32_t));
            bspx_lmoffset.push_back(std::bit_cast<int32_t>(raw));
        }
        bspx_lmshift = shift_it->second;
    }

    auto face_styles = load_atlas_face_styles(bsp, bspx);
    std::set<uint16_t> available_styles;

    // make rectangles
    for (size_t face_idx = 0; face_idx < bsp.dfaces.size(); ++face_idx) {
        const auto &face = bsp.dfaces[face_idx];
        int32_t faceofs;

        if (use_decoupled) {
            faceofs = bspx_decoupled[face_idx].offset;
        } else if (!use_bspx) {
            faceofs = face.lightofs;
        } else {
            faceofs = bspx_lmoffset[face_idx];
        }

        auto &styles = face_styles[face_idx];
        if (styles.empty() || faceofs == -1) {
            continue;
        }
        if (faceofs < -1) {
            FError("lightmap atlas face {} has invalid negative light offset {}", face_idx, faceofs);
        }
        if (face.numedges < 3 || face.firstedge < 0 || static_cast<size_t>(face.firstedge) > bsp.dsurfedges.size() ||
            static_cast<size_t>(face.numedges) > bsp.dsurfedges.size() - static_cast<size_t>(face.firstedge)) {
            FError("lightmap atlas face {} has an invalid surfedge range ({}, {})", face_idx, face.firstedge,
                face.numedges);
        }
        if (face.planenum < 0 || static_cast<size_t>(face.planenum) >= bsp.dplanes.size()) {
            FError("lightmap atlas face {} references invalid plane {}", face_idx, face.planenum);
        }
        if (face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= bsp.texinfo.size()) {
            FError("lightmap atlas face {} references invalid texinfo {}", face_idx, face.texinfo);
        }
        for (int edge_index = 0; edge_index < face.numedges; ++edge_index) {
            (void)Face_VertexAtIndex(&bsp, &face, edge_index);
        }

        faceextents_t extents;
        if (use_decoupled) {
            const auto &decoupled = bspx_decoupled[face_idx];
            if (decoupled.lmwidth == 0 || decoupled.lmheight == 0) {
                FError("DECOUPLED_LM face {} has zero-sized extents {}x{}", face_idx, decoupled.lmwidth,
                    decoupled.lmheight);
            }
            extents = {face, bsp, decoupled.lmwidth, decoupled.lmheight, decoupled.world_to_lm_space};
        } else if (use_bspx) {
            const uint8_t shift = bspx_lmshift[face_idx];
            if (shift > 30) {
                FError("LMSHIFT face {} has unsupported shift {}", face_idx, shift);
            }
            extents = {face, bsp, std::ldexp(1.0f, shift)};
        } else {
            extents = {face, bsp, LMSCALE_DEFAULT};
        }

        const int width = extents.width();
        const int height = extents.height();
        if (width <= 0 || height <= 0 || static_cast<size_t>(width) > atlas_size ||
            static_cast<size_t>(height) > atlas_size) {
            FError("lightmap atlas face {} has unsupported extents {}x{} (maximum {}x{})", face_idx, width, height,
                atlas_size, atlas_size);
        }
        const size_t sample_count =
            checked_atlas_product(static_cast<size_t>(width), static_cast<size_t>(height), "face sample");
        const size_t all_style_samples = checked_atlas_product(sample_count, styles.size(), "face style sample");

        size_t source_offset;
        if (is_hdr) {
            const int32_t offset_divisor = bsp.loadversion->game->has_rgb_lightmap ? 3 : 1;
            if (faceofs % offset_divisor != 0) {
                FError("HDR lightmap atlas face {} has unaligned RGB light offset {}", face_idx, faceofs);
            }
            source_offset = static_cast<size_t>(faceofs / offset_divisor);
            if (source_offset > hdr_lightdata_source->size() ||
                all_style_samples > hdr_lightdata_source->size() - source_offset) {
                FError("HDR lightmap atlas face {} needs {} samples at offset {}, but the source has {}", face_idx,
                    all_style_samples, source_offset, hdr_lightdata_source->size());
            }
        } else {
            // Quake stores greyscale offsets in samples while RGB-lightmap
            // formats already store byte offsets. External .lit data is RGB
            // in both cases.
            const size_t offset_multiplier = is_lit && !bsp.loadversion->game->has_rgb_lightmap ? 3 : 1;
            const size_t bytes_per_sample = is_rgb ? 3 : 1;
            source_offset = checked_atlas_product(static_cast<size_t>(faceofs), offset_multiplier, "face offset");
            const size_t source_bytes = checked_atlas_product(all_style_samples, bytes_per_sample, "face sample byte");
            if (source_offset > lightdata_source->size() || source_bytes > lightdata_source->size() - source_offset) {
                FError("lightmap atlas face {} needs {} bytes at offset {}, but the source has {}", face_idx,
                    source_bytes, source_offset, lightdata_source->size());
            }
        }

        available_styles.insert(styles.begin(), styles.end());
        rectangles.push_back(face_rect{&face, std::move(extents), std::move(styles), sample_count, source_offset});
    }

    if (rectangles.empty()) {
        return {};
    }

    // sort faces
    std::sort(rectangles.begin(), rectangles.end(), [](const face_rect &a, const face_rect &b) -> bool {
        int32_t a_height = a.extents.height();
        int32_t b_height = b.extents.height();

        if (a_height == b_height) {
            return b.face > a.face;
        }

        return a_height > b_height;
    });

    // pack
    for (auto &rect : rectangles) {
        while (true) {
            if (current_atlas == atlasses.size()) {
                atlasses.emplace_back();
            }

            atlas &atl = atlasses[current_atlas];

            if (atl.current_x + static_cast<size_t>(rect.extents.width()) > atlas_size) {
                atl.current_x = 0;
                atl.current_y += atl.tallest;
                atl.tallest = 0;
            }

            if (atl.current_y + static_cast<size_t>(rect.extents.height()) > atlas_size) {
                current_atlas++;
                continue;
            }

            atl.tallest = std::max(atl.tallest, (size_t)rect.extents.height());
            rect.x = atl.current_x;
            rect.y = atl.current_y;
            rect.atlas = current_atlas;

            atl.current_x += rect.extents.width();
            break;
        }
    }

    // calculate final atlas texture size
    single_style_atlas_t full_atlas;
    const size_t sqrt_count = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(atlasses.size()))));
    size_t trimmed_width = 0, trimmed_height = 0;

    if (sqrt_count > static_cast<size_t>(std::numeric_limits<int>::max()) / atlas_size) {
        FError("lightmap atlas is too wide to represent");
    }

    for (size_t i = 0; i < atlasses.size(); i++) {
        size_t atlas_x = (i % sqrt_count) * atlas_size;
        size_t atlas_y = (i / sqrt_count) * atlas_size;

        for (auto &rect : rectangles) {
            if (rect.atlas == i) {
                rect.x += atlas_x;
                rect.y += atlas_y;
                trimmed_width = std::max(trimmed_width, rect.x + rect.extents.width());
                trimmed_height = std::max(trimmed_height, rect.y + rect.extents.height());
            }
#if 0
            for (size_t x = 0; x < rect.texture->width; x++) {
                for (size_t y = 0; y < rect.texture->height; y++) {
                    auto &src_pixel = rect.texture->pixels[(y * rect.texture->width) + x];
                    auto &dst_pixel = full_atlas.pixels[((atlas_y + y + rect.y) * full_atlas.width) + (atlas_x + x + rect.x)];
                    dst_pixel = src_pixel;
                }
            }
#endif
        }
    }

    if (trimmed_width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        trimmed_height > static_cast<size_t>(std::numeric_limits<int>::max())) {
        FError("lightmap atlas dimensions {}x{} are too large to represent", trimmed_width, trimmed_height);
    }
    full_atlas.width = static_cast<int>(trimmed_width);
    full_atlas.height = static_cast<int>(trimmed_height);
    const size_t atlas_pixels = checked_atlas_product(full_atlas.width, full_atlas.height, "pixel");
    if (is_hdr)
        full_atlas.e5brg9_samples.resize(atlas_pixels);
    else
        full_atlas.rgba8_samples.resize(atlas_pixels);

    full_atlas_t result;

    // compile all of the styles that are available
    for (const uint16_t style : available_styles) {
        bool any_written = false;

        for (auto &rect : rectangles) {
            const auto style_it = std::ranges::find(rect.styles, style);
            if (style_it == rect.styles.end()) {
                continue;
            }
            const size_t style_index = static_cast<size_t>(style_it - rect.styles.begin());

            if (!is_hdr) {
                const size_t bytes_per_sample = is_rgb ? 3 : 1;
                size_t input_offset = rect.source_offset + (rect.sample_count * bytes_per_sample * style_index);

                for (size_t y = 0; y < rect.extents.height(); y++) {
                    for (size_t x = 0; x < rect.extents.width(); x++) {
                        size_t ox = rect.x + x;
                        size_t oy = rect.y + y;

                        auto &out_pixel = full_atlas.rgba8_samples[(oy * full_atlas.width) + ox];
                        out_pixel[3] = 255;

                        if (is_rgb) {
                            out_pixel[0] = (*lightdata_source)[input_offset++];
                            out_pixel[1] = (*lightdata_source)[input_offset++];
                            out_pixel[2] = (*lightdata_source)[input_offset++];
                        } else {
                            out_pixel[0] = out_pixel[1] = out_pixel[2] = (*lightdata_source)[input_offset++];
                        }
                    }
                }
            } else {
                size_t input_offset = rect.source_offset + (rect.sample_count * style_index);

                for (size_t y = 0; y < rect.extents.height(); y++) {
                    for (size_t x = 0; x < rect.extents.width(); x++) {
                        size_t ox = rect.x + x;
                        size_t oy = rect.y + y;

                        auto &out_pixel = full_atlas.e5brg9_samples[(oy * full_atlas.width) + ox];
                        out_pixel = (*hdr_lightdata_source)[input_offset++];
                    }
                }
            }

            any_written = true;
        }

        if (!any_written) {
            continue;
        }

        // copy out the atlas texture
        result.style_to_lightmap_atlas[style] = full_atlas;

        std::fill(full_atlas.rgba8_samples.begin(), full_atlas.rgba8_samples.end(), qvec4b{});

        std::fill(full_atlas.e5brg9_samples.begin(), full_atlas.e5brg9_samples.end(), uint32_t{});
    }

    auto ExportLightmapUVs = [&full_atlas, &result](const mbsp_t *bsp, const face_rect &face) {
        std::vector<qvec2f> face_lightmap_uvs;

        for (int i = 0; i < face.face->numedges; i++) {
            const int vertnum = Face_VertexAtIndex(bsp, face.face, i);
            const qvec3f &pos = bsp->dvertexes[vertnum];

            auto tc = face.extents.worldToLMCoord(pos);
            tc[0] += face.x;
            tc[1] += face.y;

            // add a half-texel offset (see BuildSurfaceDisplayList() in Quakespasm)
            tc[0] += 0.5;
            tc[1] += 0.5;

            tc[0] /= full_atlas.width;
            tc[1] /= full_atlas.height;

            face_lightmap_uvs.push_back(tc);
        }

        const int face_num = Face_GetNum(bsp, face.face);
        result.facenum_to_lightmap_uvs[face_num] = std::move(face_lightmap_uvs);
        result.facenum_to_lightmap_styles[face_num] = face.styles;
    };

    for (auto &rect : rectangles) {
        ExportLightmapUVs(&bsp, rect);
    }

    return result;
}

static void export_obj_and_lightmaps(const mbsp_t &bsp, const bspxentries_t &bspx, fs::path obj_path,
    const fs::path &lightmaps_path_base, const fs::path &lit_path)
{
    lit_variant_t lit;
    try {
        lit = LoadLitFile(lit_path, bsp);
    } catch (const std::exception &e) {
        FError("couldn't load external lightmap sidecar {}: {}", lit_path, e.what());
    }

    const std::vector<uint8_t> empty_rgb;
    const std::vector<uint32_t> empty_hdr;
    const auto *rgb_lit = std::get_if<lit1_t>(&lit);
    const auto *hdr_lit = std::get_if<lit_hdr>(&lit);
    const auto &rgb_litdata = rgb_lit ? rgb_lit->rgbdata : empty_rgb;
    const auto &hdr_litdata = hdr_lit ? hdr_lit->samples : empty_hdr;

    if (rgb_lit || hdr_lit) {
        logging::print("using external lightmap sidecar {}\n", lit_path);
    }

    const bool use_decoupled = bspx.contains("DECOUPLED_LM");
    const bool use_bspx = !use_decoupled && (bspx.contains("LMOFFSET") || bspx.contains("LMSHIFT"));
    const auto atlas = build_lightmap_atlas(bsp, bspx, rgb_litdata, hdr_litdata, use_bspx, use_decoupled);

    if (atlas.facenum_to_lightmap_uvs.empty()) {
        return;
    }

    // e.g. mapname.bsp.lm
    const std::string stem = lightmaps_path_base.stem().string();

    // write .png's (or .hdr's, if e5bgr9 lightmaps), one per style
    for (const auto &[i, full_atlas] : atlas.style_to_lightmap_atlas) {
        const bool is_hdr = !full_atlas.e5brg9_samples.empty();
        auto lightmaps_path = lightmaps_path_base;
        std::string extension = is_hdr ? ".hdr" : ".png";
        lightmaps_path.replace_filename(stem + "_" + std::to_string(i) + extension);

        write_output_file(lightmaps_path, std::ios_base::binary, [&](std::ofstream &strm) {
            int write_result;
            if (is_hdr) {
                std::vector<float> temp; // rgb components

                // unpack from e5bgr9 to 3x float
                for (uint32_t sample : full_atlas.e5brg9_samples) {
                    qvec3f rgb = HDR_UnpackE5BRG9(sample);
                    temp.push_back(rgb[0]);
                    temp.push_back(rgb[1]);
                    temp.push_back(rgb[2]);
                }

                write_result = stbi_write_hdr_to_func(
                    [](void *context, void *data, int size) {
                        std::ofstream &strm = *((std::ofstream *)context);
                        strm.write((const char *)data, size);
                    },
                    &strm, full_atlas.width, full_atlas.height, 3, temp.data());
            } else {
                if (full_atlas.width > std::numeric_limits<int>::max() / 4) {
                    FError("lightmap atlas {} is too wide for PNG output", lightmaps_path);
                }
                write_result = stbi_write_png_to_func(
                    [](void *context, void *data, int size) {
                        std::ofstream &strm = *((std::ofstream *)context);
                        strm.write((const char *)data, size);
                    },
                    &strm, full_atlas.width, full_atlas.height, 4, full_atlas.rgba8_samples.data(),
                    full_atlas.width * 4);
            }

            if (write_result == 0) {
                FError("failed to encode lightmap atlas {}", lightmaps_path);
            }
        });
        logging::print("wrote {}\n", lightmaps_path);
    }

    auto ExportObjFace = [&atlas](std::ostream &f, const mbsp_t *bsp, int face_num, uint64_t &vertcount) {
        const auto *face = BSP_GetFace(bsp, face_num);

        const auto &tcs = atlas.facenum_to_lightmap_uvs.at(face_num);

        // export the vertices and uvs
        for (int i = 0; i < face->numedges; i++) {
            const int vertnum = Face_VertexAtIndex(bsp, face, i);
            const qvec3f normal = bsp->dplanes[face->planenum].normal;
            const qvec3f &pos = bsp->dvertexes[vertnum];
            ewt::print(f, "v {:.9} {:.9} {:.9}\n", pos[0], pos[1], pos[2]);
            ewt::print(f, "vn {:.9} {:.9} {:.9}\n", normal[0], normal[1], normal[2]);

            qvec2f tc = tcs[i];

            tc[1] = 1.0 - tc[1];

            ewt::print(f, "vt {:.9} {:.9}\n", tc[0], tc[1]);
        }

        f << "f";
        for (int i = 0; i < face->numedges; i++) {
            // .obj vertexes start from 1
            // .obj faces are CCW, quake is CW, so reverse the order
            const uint64_t vertindex = vertcount + static_cast<uint64_t>(face->numedges - 1 - i) + 1;
            ewt::print(f, " {0}/{0}/{0}", vertindex);
        }
        f << '\n';

        vertcount += face->numedges;
    };

    auto ExportObj = [&ExportObjFace, &atlas, &obj_path](const mbsp_t *bsp) {
        write_output_file(obj_path, {}, [&](std::ofstream &objstream) {
            uint64_t vertcount = 0;

            for (const auto &[face_num, unused] : atlas.facenum_to_lightmap_uvs) {
                ExportObjFace(objstream, bsp, face_num, vertcount);
            }
        });
    };

    ExportObj(&bsp);

    logging::print("wrote {}\n", obj_path);
}

void serialize_bsp(const bspdata_t &bspdata, const mbsp_t &bsp, const fs::path &name, bool export_lightmap_atlas)
{
    auto j = Json::Value(Json::objectValue);

    if (!bsp.dmodels.empty()) {
        auto &models = (j["models"] = Json::Value(Json::arrayValue));

        for (auto &src_model : bsp.dmodels) {
            auto &model = models.append(Json::Value(Json::objectValue));

            model["mins"] = to_json(src_model.mins);
            model["maxs"] = to_json(src_model.maxs);
            model["origin"] = to_json(src_model.origin);
            model["headnode"] = to_json(src_model.headnode);
            model["visleafs"] = src_model.visleafs;
            model["firstface"] = src_model.firstface;
            model["numfaces"] = src_model.numfaces;
        }
    }

    if (bsp.dvis.bits.size()) {

        if (bsp.dvis.bit_offsets.size()) {
            auto &visdata = j["visdata"];
            visdata = Json::Value(Json::objectValue);

            auto &pvs = (visdata["pvs"] = Json::Value(Json::arrayValue));
            auto &phs = (visdata["phs"] = Json::Value(Json::arrayValue));

            for (auto &offset : bsp.dvis.bit_offsets) {
                pvs.append(offset[VIS_PVS]);
                phs.append(offset[VIS_PHS]);
            }

            visdata["bits"] = hex_string(bsp.dvis.bits.data(), bsp.dvis.bits.size());
        } else {
            j["visdata"] = hex_string(bsp.dvis.bits.data(), bsp.dvis.bits.size());
        }
    }

    if (bsp.dlightdata.size()) {
        j["lightdata"] = hex_string(bsp.dlightdata.data(), bsp.dlightdata.size());
    }

    if (!bsp.dentdata.empty()) {
        j["entdata"] = bsp.dentdata + '\0';
    }

    if (!bsp.dleafs.empty()) {
        auto &leafs = (j["leafs"] = Json::Value(Json::arrayValue));

        for (auto &src_leaf : bsp.dleafs) {
            auto &leaf = leafs.append(Json::Value(Json::objectValue));

            leaf["contents"] = src_leaf.contents;
            leaf["visofs"] = src_leaf.visofs;
            leaf["mins"] = to_json(src_leaf.mins);
            leaf["maxs"] = to_json(src_leaf.maxs);
            leaf["firstmarksurface"] = src_leaf.firstmarksurface;
            leaf["nummarksurfaces"] = src_leaf.nummarksurfaces;
            leaf["ambient_level"] = to_json(src_leaf.ambient_level);
            leaf["cluster"] = src_leaf.cluster;
            leaf["area"] = src_leaf.area;
            leaf["firstleafbrush"] = src_leaf.firstleafbrush;
            leaf["numleafbrushes"] = src_leaf.numleafbrushes;
        }
    }

    if (!bsp.dplanes.empty()) {
        auto &planes = (j["planes"] = Json::Value(Json::arrayValue));

        for (auto &src_plane : bsp.dplanes) {
            auto &plane = planes.append(Json::Value(Json::objectValue));

            plane["normal"] = to_json(src_plane.normal);
            plane["dist"] = src_plane.dist;
            plane["type"] = src_plane.type;
        }
    }

    if (!bsp.dvertexes.empty()) {
        auto &vertexes = (j["vertexes"] = Json::Value(Json::arrayValue));

        for (auto &src_vertex : bsp.dvertexes) {
            vertexes.append(to_json(src_vertex));
        }
    }

    if (!bsp.dnodes.empty()) {
        auto &nodes = (j["nodes"] = Json::Value(Json::arrayValue));

        for (auto &src_node : bsp.dnodes) {
            auto &node = nodes.append(Json::Value(Json::objectValue));

            node["planenum"] = src_node.planenum;
            node["children"] = json_array({src_node.children[0], src_node.children[1]});
            node["mins"] = to_json(src_node.mins);
            node["maxs"] = to_json(src_node.maxs);
            node["firstface"] = src_node.firstface;
            node["numfaces"] = src_node.numfaces;

            // human-readable plane
            auto &plane = bsp.dplanes.at(src_node.planenum);
            node["plane"] = json_array({plane.normal[0], plane.normal[1], plane.normal[2], plane.dist});
        }
    }

    if (!bsp.texinfo.empty()) {
        auto &texinfos = (j["texinfo"] = Json::Value(Json::arrayValue));

        for (auto &src_texinfo : bsp.texinfo) {
            auto &texinfo = texinfos.append(Json::Value(Json::objectValue));

            texinfo["vecs"] = json_array({json_array({src_texinfo.vecs.at(0, 0), src_texinfo.vecs.at(0, 1),
                                              src_texinfo.vecs.at(0, 2), src_texinfo.vecs.at(0, 3)}),
                json_array({src_texinfo.vecs.at(1, 0), src_texinfo.vecs.at(1, 1), src_texinfo.vecs.at(1, 2),
                    src_texinfo.vecs.at(1, 3)})});

            texinfo["flags"] = bspdata.loadversion->game->id == GAME_QUAKE_II
                                   ? static_cast<int32_t>(src_texinfo.flags.native_q2)
                                   : static_cast<int32_t>(src_texinfo.flags.native_q1);
            texinfo["miptex"] = src_texinfo.miptex;
            texinfo["value"] = src_texinfo.value;
            texinfo["texture"] = src_texinfo.texturename;
            texinfo["nexttexinfo"] = src_texinfo.nexttexinfo;
        }
    }

    if (!bsp.dfaces.empty()) {
        auto &faces = (j["faces"] = Json::Value(Json::arrayValue));

        for (auto &src_face : bsp.dfaces) {
            auto &face = faces.append(Json::Value(Json::objectValue));

            face["planenum"] = src_face.planenum;
            face["side"] = src_face.side;
            face["firstedge"] = src_face.firstedge;
            face["numedges"] = src_face.numedges;
            face["texinfo"] = src_face.texinfo;
            face["styles"] = to_json(src_face.styles);
            face["lightofs"] = src_face.lightofs;

            // for readibility, also output the actual vertices
            auto verts = Json::Value(Json::arrayValue);
            for (int32_t k = 0; k < src_face.numedges; ++k) {
                const int vertex = Face_VertexAtIndex(&bsp, &src_face, k);
                verts.append(to_json(bsp.dvertexes[static_cast<size_t>(vertex)]));
            }
            face["vertices"] = verts;

#if 0
            if (auto lm = get_lightmap_face(bsp, src_face, false)) {
                face["lightmap", serialize_image(lm)});
            }
#endif
        }
    }

    if (!bsp.dclipnodes.empty()) {
        auto &clipnodes = (j["clipnodes"] = Json::Value(Json::arrayValue));

        for (auto &src_clipnodes : bsp.dclipnodes) {
            auto &clipnode = clipnodes.append(Json::Value(Json::objectValue));

            clipnode["planenum"] = src_clipnodes.planenum;
            clipnode["children"] = json_array({src_clipnodes.children[0], src_clipnodes.children[1]});
        }
    }

    if (!bsp.dedges.empty()) {
        auto &edges = (j["edges"] = Json::Value(Json::arrayValue));

        for (auto &src_edge : bsp.dedges) {
            edges.append(to_json(src_edge));
        }
    }

    if (!bsp.dleaffaces.empty()) {
        auto &leaffaces = (j["leaffaces"] = Json::Value(Json::arrayValue));

        for (auto &src_leafface : bsp.dleaffaces) {
            leaffaces.append(src_leafface);
        }
    }

    if (!bsp.dsurfedges.empty()) {
        auto &surfedges = (j["surfedges"] = Json::Value(Json::arrayValue));

        for (auto &src_surfedges : bsp.dsurfedges) {
            surfedges.append(src_surfedges);
        }
    }

    if (!bsp.dbrushsides.empty()) {
        auto &brushsides = (j["brushsides"] = Json::Value(Json::arrayValue));

        for (auto &src_brushside : bsp.dbrushsides) {
            auto &brushside = brushsides.append(Json::Value(Json::objectValue));

            brushside["planenum"] = src_brushside.planenum;
            brushside["texinfo"] = src_brushside.texinfo;
        }
    }

    if (!bsp.dbrushes.empty()) {
        auto &brushes = (j["brushes"] = Json::Value(Json::arrayValue));

        for (auto &src_brush : bsp.dbrushes) {
            auto &brush = brushes.append(Json::Value(Json::objectValue));

            brush["firstside"] = src_brush.firstside;
            brush["numsides"] = src_brush.numsides;
            brush["contents"] = src_brush.contents;
        }
    }

    if (!bsp.dleafbrushes.empty()) {
        auto &leafbrushes = (j["leafbrushes"] = Json::Value(Json::arrayValue));

        for (auto &src_leafbrush : bsp.dleafbrushes) {
            leafbrushes.append(src_leafbrush);
        }
    }

    if (bsp.dtex.textures.size()) {
        auto &textures = (j["textures"] = Json::Value(Json::arrayValue));

        for (auto &src_tex : bsp.dtex.textures) {
            if (src_tex.null_texture) {
                // use json null to indicate offset -1
                textures.append(Json::Value(Json::nullValue));
                continue;
            }
            auto &tex = textures.append(Json::Value(Json::objectValue));

            tex["name"] = src_tex.name;
            tex["width"] = src_tex.width;
            tex["height"] = src_tex.height;

            if (src_tex.data.size() > sizeof(dmiptex_t)) {
                auto &mips = tex["mips"] = Json::Value(Json::arrayValue);
                mips.append(
                    serialize_image(img::load_mip(src_tex.name, src_tex.data, false, bspdata.loadversion->game)));
            }
        }
    }

    if (!bspdata.bspx.entries.empty()) {
        auto &bspxentries = (j["bspxentries"] = Json::Value(Json::arrayValue));

        for (auto &lump : bspdata.bspx.entries) {
            auto &entry = bspxentries.append(Json::Value(Json::objectValue));
            entry["lumpname"] = lump.first;

            if (lump.first == "BRUSHLIST") {
                entry["models"] = serialize_bspxbrushlist(lump.second);
            } else if (lump.first == "DECOUPLED_LM") {
                entry["faces"] = serialize_bspx_decoupled_lm(lump.second);
            } else {
                // unhandled BSPX lump, just write the raw data
                entry["lumpdata"] = hex_string(lump.second.data(), lump.second.size());
            }
        }
    }

    // lightmap atlas
#if 0
    for (int32_t i = 0; i < MAXLIGHTMAPS; i++) {
        if (auto lm = generate_lightmap_atlases(bsp, bspdata.bspx.entries, false); !lm.empty()) {
            j.emplace("lightmaps", std::move(lm));
        }

        if (bspdata.bspx.entries.find("LMOFFSET") != bspdata.bspx.entries.end()) {
            if (auto lm = generate_lightmap_atlases(bsp, bspdata.bspx.entries, true); !lm.empty()) {
                j.emplace("bspx_lightmaps", std::move(lm));
            }
        }
    }
#endif
    if (export_lightmap_atlas) {
        auto lit_path = fs::path(name);
        if (lit_path.extension() == ".json") {
            lit_path.replace_extension();
        }
        lit_path.replace_extension(".lit");

        export_obj_and_lightmaps(bsp, bspdata.bspx.entries, fs::path(name).replace_extension(".geometry.obj"),
            fs::path(name).replace_extension(".lm.png"), lit_path);
    }

    write_output_file(name, {}, [&](std::ofstream &json_stream) { json_stream << std::setw(4) << j; });

    logging::print("wrote {}\n", name);
}
