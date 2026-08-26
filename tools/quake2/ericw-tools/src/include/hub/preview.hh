#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace hub
{
constexpr size_t MAX_PREVIEW_LIGHT_STYLES = 256;
constexpr size_t MAX_PREVIEW_STYLES_PER_FACE = 16;

using light_style_layers_t = std::unordered_map<int, int>;

struct packed_light_styles_t
{
    std::array<uint32_t, 4> words{};
    uint32_t count = 0;
};

[[nodiscard]] inline light_style_layers_t compact_light_styles(std::span<const uint16_t> style_ids)
{
    if (style_ids.size() > MAX_PREVIEW_LIGHT_STYLES) {
        throw std::length_error("preview light-style count exceeds 256 GPU layers");
    }

    light_style_layers_t result;
    result.reserve(style_ids.size());
    for (size_t layer = 0; layer < style_ids.size(); ++layer) {
        if (style_ids[layer] == std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument("65535 is the light-style terminator, not a renderable style");
        }
        if (!result.emplace(style_ids[layer], static_cast<int>(layer)).second) {
            throw std::invalid_argument("preview light-style identifiers must be unique");
        }
    }
    return result;
}

[[nodiscard]] inline packed_light_styles_t pack_face_light_styles(
    std::span<const uint16_t> style_ids, const light_style_layers_t &layers)
{
    if (style_ids.size() > MAX_PREVIEW_STYLES_PER_FACE) {
        throw std::length_error("face light-style count exceeds 16 packed GPU slots");
    }

    packed_light_styles_t result;
    result.count = static_cast<uint32_t>(style_ids.size());
    for (size_t style_index = 0; style_index < style_ids.size(); ++style_index) {
        const auto layer_it = layers.find(style_ids[style_index]);
        if (layer_it == layers.end() || layer_it->second < 0 || layer_it->second > 255) {
            throw std::invalid_argument("face references an unavailable preview light style");
        }
        const uint32_t layer = static_cast<uint32_t>(layer_it->second);
        const size_t packed_index = style_index / 4;
        const uint32_t shift = static_cast<uint32_t>((style_index % 4) * 8);
        result.words[packed_index] |= layer << shift;
    }
    return result;
}

struct preview_paths_t
{
    std::filesystem::path source_map;
    std::filesystem::path source_rad;
    std::filesystem::path output_bsp;
    std::filesystem::path output_rad;
};

[[nodiscard]] inline preview_paths_t make_preview_paths(
    const std::filesystem::path &source_map, const std::filesystem::path &workspace)
{
    if (source_map.empty() || workspace.empty()) {
        throw std::invalid_argument("preview source and workspace paths must not be empty");
    }

    const std::filesystem::path source_filename = source_map.filename();
    if (source_filename.empty() || source_filename == "." || source_filename == "..") {
        throw std::invalid_argument("preview source must name a map file");
    }

    preview_paths_t result;
    result.source_map = source_map;
    result.source_rad = source_map;
    result.source_rad.replace_extension(".rad");
    result.output_bsp = workspace / source_filename;
    result.output_bsp.replace_extension(".bsp");
    result.output_rad = result.output_bsp;
    result.output_rad.replace_extension(".rad");
    return result;
}

[[nodiscard]] inline std::vector<std::string> make_preview_qbsp_arguments(
    const std::vector<std::string> &common, const std::vector<std::string> &qbsp, const preview_paths_t &paths)
{
    std::vector<std::string> result;
    result.reserve(3 + common.size() + qbsp.size());
    result.emplace_back();
    result.insert(result.end(), common.begin(), common.end());
    result.insert(result.end(), qbsp.begin(), qbsp.end());
    result.push_back(paths.source_map.string());
    result.push_back(paths.output_bsp.string());
    return result;
}

[[nodiscard]] inline std::vector<std::string> make_preview_tool_arguments(
    const std::vector<std::string> &common, const std::vector<std::string> &tool, const preview_paths_t &paths)
{
    std::vector<std::string> result;
    result.reserve(2 + common.size() + tool.size());
    result.emplace_back();
    result.insert(result.end(), common.begin(), common.end());
    result.insert(result.end(), tool.begin(), tool.end());
    result.push_back(paths.output_bsp.string());
    return result;
}

[[nodiscard]] inline std::filesystem::path infer_source_gamedir(const std::filesystem::path &source_map)
{
    const auto is_maps_directory = [](const std::filesystem::path &path) {
        const std::string name = path.filename().string();
        constexpr std::string_view maps = "maps";
        return name.size() == maps.size() && std::equal(name.begin(), name.end(), maps.begin(), [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
        });
    };

    for (auto directory = source_map.parent_path(); !directory.empty();) {
        if (is_maps_directory(directory)) {
            return directory.parent_path();
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return source_map.parent_path();
}

[[nodiscard]] inline std::vector<std::string> make_preview_light_arguments(
    const std::vector<std::string> &common, const std::vector<std::string> &light, const preview_paths_t &paths)
{
    std::vector<std::string> result;
    result.reserve(6 + common.size() + light.size());
    result.emplace_back();

    // LIGHT normally infers its resource roots from the input BSP. Since the
    // preview BSP is temporary, seed the original source context first; later
    // user-provided common options retain precedence.
    if (const auto source_directory = paths.source_map.parent_path(); !source_directory.empty()) {
        result.emplace_back("-path");
        result.push_back(source_directory.string());
    }
    if (const auto gamedir = infer_source_gamedir(paths.source_map); !gamedir.empty()) {
        result.emplace_back("-gamedir");
        result.push_back(gamedir.string());
    }

    result.insert(result.end(), common.begin(), common.end());
    result.insert(result.end(), light.begin(), light.end());
    result.push_back(paths.output_bsp.string());
    return result;
}

inline bool copy_map_rad_if_present(const preview_paths_t &paths)
{
    std::error_code error;
    const bool source_exists = std::filesystem::exists(paths.source_rad, error);
    if (error) {
        throw std::filesystem::filesystem_error("could not inspect map-specific .rad file", paths.source_rad, error);
    }
    if (!source_exists) {
        return false;
    }

    std::filesystem::copy_file(
        paths.source_rad, paths.output_rad, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "could not copy map-specific .rad file into preview workspace", paths.source_rad, paths.output_rad, error);
    }
    return true;
}
} // namespace hub
