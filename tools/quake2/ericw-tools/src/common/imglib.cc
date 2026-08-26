#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>
#include <common/fs.hh>
#include <common/imglib.hh>
#include <common/entdata.h>
#include <common/json.hh>
#include <common/log.hh>
#include <common/settings.hh>

#define STB_IMAGE_IMPLEMENTATION
#include "../../extern/stb_image.h"

/*
============================================================================
PALETTE
============================================================================
*/

namespace img
{
namespace
{
bool is_safe_texture_name(std::string_view name)
{
    if (name.empty() || name.find('\0') != std::string_view::npos || name.front() == '/' || name.front() == '\\' ||
        name.find(':') != std::string_view::npos) {
        return false;
    }

    size_t component_start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i != name.size() && name[i] != '/' && name[i] != '\\') {
            continue;
        }

        if (name.substr(component_start, i - component_start) == "..") {
            return false;
        }
        component_start = i + 1;
    }

    return true;
}

fs::path texture_path(std::string_view name, bool use_game_prefix)
{
    const fs::path relative_name{name};
    if (use_game_prefix) {
        return fs::path{"textures"} / relative_name;
    }
    return relative_name;
}

bool byte_range_within_data(uint64_t offset, uint64_t length, size_t data_size)
{
    const uint64_t size = static_cast<uint64_t>(data_size);
    return offset <= size && length <= size - offset;
}

std::optional<size_t> checked_pixel_count(uint32_t width, uint32_t height)
{
    const uint64_t count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        count > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }
    return static_cast<size_t>(count);
}
} // namespace

// current palette
std::vector<qvec3b> palette;

/*
============================================================================
PCX IMAGE
Only used for palette here.
============================================================================
*/
struct pcx_t
{
    int8_t manufacturer;
    int8_t version;
    int8_t encoding;
    int8_t bits_per_pixel;
    uint16_t xmin, ymin, xmax, ymax;
    uint16_t hres, vres;
    padding<49> palette_reserved;
    int8_t color_planes;
    uint16_t bytes_per_line;
    uint16_t palette_type;
    padding<58> filler;

    auto stream_data()
    {
        return std::tie(manufacturer, version, encoding, bits_per_pixel, xmin, ymin, xmax, ymax, hres, vres,
            palette_reserved, color_planes, bytes_per_line, palette_type, filler);
    }
};

static bool LoadPCXPalette(const fs::path &filename, std::vector<qvec3b> &palette, bool prefer_loose)
{
    auto file = fs::load(filename, prefer_loose);

    constexpr size_t serialized_header_size = 128;
    constexpr size_t palette_bytes = sizeof(qvec3b) * 256;
    if (!file || file->size() < serialized_header_size + palette_bytes) {
        logging::funcprint("Failed to load '{}'.\n", filename);
        return false;
    }

    imemstream stream(file->data(), file->size(), std::ios_base::in | std::ios_base::binary);
    stream >> endianness<std::endian::little>;

    // Parse the PCX file
    pcx_t pcx{};
    stream >= pcx;

    if (!stream || pcx.manufacturer != 0x0a || pcx.version != 5 || pcx.encoding != 1 || pcx.bits_per_pixel != 8) {
        logging::funcprint("Failed to load '{}'. Unsupported PCX file.\n", filename);
        return false;
    }

    std::vector<qvec3b> parsed_palette(256);

    stream.seekg(static_cast<std::streamoff>(file->size() - palette_bytes));
    if (!stream) {
        logging::funcprint("Failed to seek to the palette in '{}'.\n", filename);
        return false;
    }
    stream.read(reinterpret_cast<char *>(parsed_palette.data()), static_cast<std::streamsize>(palette_bytes));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(palette_bytes)) {
        logging::funcprint("Failed to read the palette in '{}'.\n", filename);
        return false;
    }

    palette = std::move(parsed_palette);
    return true;
}

void init_palette(const gamedef_t *game)
{
    init_palette(game, false);
}

void init_palette(const gamedef_t *game, bool prefer_loose)
{
    palette.clear();

    // Load game-specific palette palette
    if (game->id == GAME_QUAKE_II) {
        constexpr const char *colormap = "pics/colormap.pcx";

        if (LoadPCXPalette(colormap, palette, prefer_loose)) {
            return;
        }
    }

    logging::print("INFO: using built-in palette.\n");

    auto &pal = game->get_default_palette();

    std::copy(pal.begin(), pal.end(), std::back_inserter(palette));
}

static void convert_paletted_to_32_bit(
    const std::vector<uint8_t> &pixels, std::vector<qvec4b> &output, const std::vector<qvec3b> &pal)
{
    if (pal.size() != 256) {
        FError("palette size {} != 256", pal.size());
    }
    output.resize(pixels.size());

    for (size_t i = 0; i < pixels.size(); i++) {
        // Last palette index is transparent color
        output[i] = qvec4b(pal[pixels[i]], pixels[i] == 255 ? 0 : 255);
    }
}

/*
============================================================================
WAL IMAGE
============================================================================
*/
struct q2_miptex_t
{
    std::array<char, 32> name;
    uint32_t width, height;
    std::array<uint32_t, MIPLEVELS> offsets; // four mip maps stored
    std::array<char, 32> animname; // next frame in animation chain
    int32_t flags;
    int32_t contents;
    int32_t value;

    auto stream_data() { return std::tie(name, width, height, offsets, animname, flags, contents, value); }
};

std::optional<texture> load_wal(std::string_view name, const fs::data &file, bool meta_only, const gamedef_t *game)
{
    if (!file) {
        logging::funcprint("Failed to load WAL {}. No file data.\n", name);
        return std::nullopt;
    }

    imemstream stream(file->data(), file->size(), std::ios_base::in | std::ios_base::binary);
    stream >> endianness<std::endian::little>;

    // Parse WAL
    q2_miptex_t mt{};
    stream >= mt;
    if (!stream) {
        logging::funcprint("Failed to load WAL {}. Header incomplete.\n", name);
        return std::nullopt;
    }

    texture tex;

    tex.meta.extension = ext::WAL;

    // note: this is a bit of a hack, but the name stored in
    // the .wal is ignored. it's extraneous and well-formed wals
    // will all match up anyways.
    tex.meta.name = name;
    tex.meta.width = tex.width = mt.width;
    tex.meta.height = tex.height = mt.height;
    tex.meta.contents_native = mt.contents;
    tex.meta.flags = {.native_q2 = static_cast<q2_surf_flags_t>(mt.flags)};
    tex.meta.value = mt.value;
    const auto animation_end = std::find(mt.animname.begin(), mt.animname.end(), '\0');
    tex.meta.animation.assign(mt.animname.begin(), animation_end);

    if (!meta_only) {
        constexpr uint64_t serialized_header_size = 32 + (sizeof(uint32_t) * 6) + 32 + (sizeof(int32_t) * 3);
        const auto pixel_count = checked_pixel_count(mt.width, mt.height);
        if (!pixel_count || *pixel_count == 0 || mt.offsets[0] < serialized_header_size ||
            !byte_range_within_data(mt.offsets[0], pixel_count.value_or(0), file->size())) {
            logging::funcprint("WAL pixel data overrun for {}\n", name);
            return std::nullopt;
        }

        stream.seekg(static_cast<std::streamoff>(mt.offsets[0]));
        if (!stream) {
            logging::funcprint("Failed to seek to WAL pixel data for {}\n", name);
            return std::nullopt;
        }

        std::vector<uint8_t> pixels(*pixel_count);
        if (!pixels.empty()) {
            stream.read(reinterpret_cast<char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (!stream || stream.gcount() != static_cast<std::streamsize>(pixels.size())) {
                logging::funcprint("WAL pixel data is truncated for {}\n", name);
                return std::nullopt;
            }
        }
        convert_paletted_to_32_bit(pixels, tex.pixels, palette);
    }

    return tex;
}

/*
============================================================================
Quake/Half Life MIP
============================================================================
*/

std::optional<texture> load_mip(std::string_view name, const fs::data &file, bool meta_only, const gamedef_t *game)
{
    if (!file) {
        logging::funcprint("Failed to load mip {}. No file data.\n", name);
        return std::nullopt;
    }

    imemstream stream(file->data(), file->size());
    stream >> endianness<std::endian::little>;

    // read header
    dmiptex_t header{};
    stream >= header;

    // must be able to at least read the header
    if (!stream) {
        logging::funcprint("Failed to fully load mip {}. Header incomplete.\n", name);
        return std::nullopt;
    }

    texture tex;

    tex.meta.extension = ext::MIP;

    // note: this is a bit of a hack, but the name stored in
    // the mip is ignored. it's extraneous and well-formed mips
    // will all match up anyways.
    tex.meta.name = name;
    tex.meta.width = tex.width = header.width;
    tex.meta.height = tex.height = header.height;

    if (!meta_only) {
        // miptex only has meta
        if (header.offsets[0] <= 0) {
            return tex;
        }

        // convert the data into RGBA.
        // sanity check
        constexpr uint64_t serialized_header_size = 16 + (sizeof(uint32_t) * 2) + (sizeof(int32_t) * MIPLEVELS);
        const auto pixel_count = checked_pixel_count(header.width, header.height);
        const uint64_t pixel_offset = static_cast<uint64_t>(header.offsets[0]);
        if (!pixel_count || *pixel_count == 0 || pixel_offset < serialized_header_size ||
            !byte_range_within_data(pixel_offset, pixel_count.value_or(0), file->size())) {
            logging::funcprint("mip offset0 overrun for {}\n", name);
            return std::nullopt;
        }

        // fetch the full data for the first mip
        stream.seekg(static_cast<std::streamoff>(pixel_offset));
        if (!stream) {
            logging::funcprint("mip offset0 seek failed for {}\n", name);
            return std::nullopt;
        }

        std::vector<uint8_t> pixels(*pixel_count);
        if (!pixels.empty()) {
            stream.read(reinterpret_cast<char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (!stream || stream.gcount() != static_cast<std::streamsize>(pixels.size())) {
                logging::funcprint("mip pixel data is truncated for {}\n", name);
                return std::nullopt;
            }
        }

        // Half Life will have a palette of 256 colors in a specific spot
        // so use that instead of game-specific palette.
        // FIXME: to support these palettes in other games we'd need to
        // maybe pass through the archive it's loaded from. if it's a WAD3
        // we can safely make the next assumptions, but WAD2s might have wildly
        // different data after the mips...
        if (game->id == GAME_HALF_LIFE) {
            bool valid_mip_palette = true;

            const uint64_t mip3_size =
                static_cast<uint64_t>(header.width >> 3) * static_cast<uint64_t>(header.height >> 3);
            constexpr uint64_t palette_size = sizeof(uint16_t) + (sizeof(qvec3b) * 256);

            if (header.offsets[3] <= 0) {
                logging::funcprint("mip palette needs offset3 to work, for {}\n", name);
                valid_mip_palette = false;
            } else if (static_cast<uint64_t>(header.offsets[3]) < serialized_header_size ||
                       !byte_range_within_data(
                           static_cast<uint64_t>(header.offsets[3]), mip3_size + palette_size, file->size())) {
                logging::funcprint("mip palette overrun for {}\n", name);
                valid_mip_palette = false;
            }

            if (valid_mip_palette) {
                std::vector<qvec3b> mip_palette(256);
                const uint64_t palette_offset = static_cast<uint64_t>(header.offsets[3]) + mip3_size;
                stream.seekg(static_cast<std::streamoff>(palette_offset));
                if (!stream) {
                    logging::funcprint("mip palette seek failed for {}\n", name);
                    valid_mip_palette = false;
                }

                uint16_t num_colors = 0;
                if (valid_mip_palette) {
                    stream >= num_colors;
                }

                if (!valid_mip_palette || !stream || num_colors != 256) {
                    logging::funcprint("mip palette color num should be 256 for {}\n", name);
                    valid_mip_palette = false;
                } else {
                    const auto palette_bytes = static_cast<std::streamsize>(mip_palette.size() * sizeof(qvec3b));
                    stream.read(reinterpret_cast<char *>(mip_palette.data()), palette_bytes);
                    if (!stream || stream.gcount() != palette_bytes) {
                        logging::funcprint("mip palette is truncated for {}\n", name);
                        valid_mip_palette = false;
                    }
                }

                if (valid_mip_palette) {
                    convert_paletted_to_32_bit(pixels, tex.pixels, mip_palette);
                    return tex;
                }
            }
        }

        convert_paletted_to_32_bit(pixels, tex.pixels, palette);
    }

    return tex;
}

std::optional<texture> load_stb(std::string_view name, const fs::data &file, bool meta_only, const gamedef_t *game)
{
    if (!file || file->empty() || file->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        logging::funcprint("Failed to load image {}. Invalid input size.\n", name);
        return std::nullopt;
    }

    int x = 0;
    int y = 0;
    int channels_in_file = 0;

    if (meta_only) {
        if (!stbi_info_from_memory(file->data(), static_cast<int>(file->size()), &x, &y, &channels_in_file) || x <= 0 ||
            y <= 0) {
            logging::funcprint("Failed to read image metadata for {}: {}\n", name, stbi_failure_reason());
            return std::nullopt;
        }

        texture tex;
        tex.meta.extension = ext::STB;
        tex.meta.name = name;
        tex.meta.width = tex.width = static_cast<uint32_t>(x);
        tex.meta.height = tex.height = static_cast<uint32_t>(y);
        return tex;
    }

    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgba_data(
        stbi_load_from_memory(file->data(), static_cast<int>(file->size()), &x, &y, &channels_in_file, 4),
        &stbi_image_free);

    if (!rgba_data) {
        logging::funcprint("stbi error: {}\n", stbi_failure_reason());
        return {};
    }

    if (x <= 0 || y <= 0) {
        logging::funcprint("Image {} has invalid dimensions {}x{}.\n", name, x, y);
        return std::nullopt;
    }

    texture tex;
    tex.meta.extension = ext::STB;
    tex.meta.name = name;
    tex.meta.width = tex.width = static_cast<uint32_t>(x);
    tex.meta.height = tex.height = static_cast<uint32_t>(y);

    const uint64_t pixel_count = static_cast<uint64_t>(x) * static_cast<uint64_t>(y);
    if (pixel_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        pixel_count > tex.pixels.max_size() ||
        pixel_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max() / sizeof(qvec4b))) {
        logging::funcprint("Image dimensions are too large for {}.\n", name);
        return std::nullopt;
    }

    const size_t num_pixels = static_cast<size_t>(pixel_count);

    static_assert(sizeof(qvec4b) == 4);
    static_assert(std::is_trivially_copyable_v<qvec4b>);
    tex.pixels.resize(num_pixels);
    std::memcpy(tex.pixels.data(), rgba_data.get(), num_pixels * sizeof(qvec4b));

    return tex;
}

// texture cache
std::unordered_map<std::string, texture, case_insensitive_hash, case_insensitive_equal> textures;

const texture *find(std::string_view str)
{
    auto it = textures.find(str);

    if (it == textures.end()) {
        return nullptr;
    }

    return &it->second;
}

void clear()
{
    palette.clear();
    textures.clear();
}

qvec3b calculate_average(const std::vector<qvec4b> &pixels)
{
    qvec3d avg{};
    size_t n = 0;

    for (auto &pixel : pixels) {
        // FIXME: is this valid for transparent averages?
        if (pixel[3] >= 127) {
            avg += pixel.xyz();
            n++;
        }
    }

    if (n == 0) {
        return {};
    }

    return avg /= n;
}

std::tuple<std::optional<img::texture>, fs::resolve_result, fs::data> load_texture(std::string_view name,
    bool meta_only, const gamedef_t *game, const settings::common_settings &options, bool no_prefix, bool mip_only)
{
    // Empty texture slots are valid sentinels in some BSPs, not malformed
    // authored paths, so leave them quiet.
    if (name.empty()) {
        return {std::nullopt, {}, {}};
    }
    if (!is_safe_texture_name(name)) {
        logging::funcprint("WARNING: refusing unsafe texture path '{}'.\n", name);
        return {std::nullopt, {}, {}};
    }

    constexpr extension_info_t mip_extensions[] = {{"", ext::MIP, load_mip}};
    std::span<const extension_info_t> exts;
    if (mip_only) {
        exts = mip_extensions;
    } else {
        exts = img::extension_list;
    }

    const auto try_path = [&](bool prefix_external,
                              bool external_only =
                                  false) -> std::tuple<std::optional<img::texture>, fs::resolve_result, fs::data> {
        for (auto &ext : exts) {
            // The second pass is exclusively the new prefix-free external
            // lookup. Do not repeat legacy embedded/WAD MIP candidates there.
            if (external_only && ext.id == ext::MIP) {
                continue;
            }

            // MIP/WAD names have always been unprefixed, including during the
            // traditional first pass.
            const bool use_game_prefix = prefix_external && ext.id != ext::MIP;
            fs::path p = texture_path(name, use_game_prefix);
            p += ext.suffix;

            if (auto pos = fs::where(p, options.filepriority.value() == settings::search_priority_t::LOOSE)) {
                if (auto data = fs::load(pos)) {
                    if (auto texture = ext.loader(name, data, meta_only, game)) {
                        return {texture, pos, data};
                    }
                }
            }
        }

        return {std::nullopt, {}, {}};
    };

    // Exhaust the conventional game layout and legacy unprefixed MIP/WAD
    // lookup before trying the new prefix-free external fallback. This keeps
    // existing project precedence stable when both old and new layouts exist.
    if (!no_prefix && !mip_only) {
        auto result = try_path(true);
        if (std::get<0>(result)) {
            return result;
        }

        return try_path(false, true);
    }

    return try_path(false);
}

std::optional<texture_meta> load_wal_meta(std::string_view name, const fs::data &file, const gamedef_t *game)
{
    if (auto tex = load_wal(name, file, true, game)) {
        return tex->meta;
    }

    return std::nullopt;
}

// see .wal_json section in qbsp.rst for format documentation
std::optional<texture_meta> load_wal_json_meta(std::string_view name, const fs::data &file, const gamedef_t *game)
{
    if (!file || file->empty()) {
        logging::funcprint("{}, missing or empty JSON metadata.\n", name);
        return std::nullopt;
    }

    try {
        Json::Value json = parse_json(file->data(), file->data() + file->size());
        if (!json.isObject()) {
            logging::funcprint("{}, JSON metadata root must be an object.\n", name);
            return std::nullopt;
        }

        texture_meta meta{};

        meta.name = name;

        {
            fs::path wal = fs::path(name).replace_extension(".wal");

            if (auto wal_file = fs::load(wal))
                if (auto wal_meta = load_wal_meta(wal.string(), wal_file, game))
                    meta = *wal_meta;
        }

        if (json.isMember("width")) {
            if (!json["width"].isUInt() || json["width"].asUInt() == 0) {
                logging::funcprint("{}, invalid metadata width.\n", name);
                return std::nullopt;
            }
            meta.width = json["width"].asUInt();
        }

        if (json.isMember("height")) {
            if (!json["height"].isUInt() || json["height"].asUInt() == 0) {
                logging::funcprint("{}, invalid metadata height.\n", name);
                return std::nullopt;
            }
            meta.height = json["height"].asUInt();
        }

        if (json.isMember("value") && json["value"].isInt()) {
            meta.value = json["value"].as<int32_t>();
        }

        if (json.isMember("contents")) {
            auto &contents = json["contents"];

            if (contents.isInt()) {
                meta.contents_native = contents.as<int32_t>();
            } else if (contents.isString()) {
                meta.contents_native = game->contents_from_string(contents.as<std::string>());
            } else if (contents.isArray()) {
                int native = 0;
                for (auto &content : contents) {
                    if (content.isInt()) {
                        native |= content.as<int32_t>();
                    } else if (content.isString()) {
                        native |= game->contents_from_string(content.as<std::string>());
                    }
                }
                meta.contents_native = native;
            }
        }

        // this only makes sense for q2
        if (json.isMember("flags") && game->id == GAME_QUAKE_II) {
            auto &flags = json["flags"];

            int32_t intflags = 0;
            if (flags.isInt()) {
                intflags = flags.as<int32_t>();
            } else if (flags.isString()) {
                intflags = game->surfflags_from_string(flags.as<std::string>());
            } else if (flags.isArray()) {
                for (auto &flag : flags) {
                    if (flag.isInt()) {
                        intflags |= flag.as<int32_t>();
                    } else if (flag.isString()) {
                        intflags |= game->surfflags_from_string(flag.as<std::string>());
                    }
                }
            }
            meta.flags.native_q2 = static_cast<q2_surf_flags_t>(intflags);
        }

        if (json.isMember("animation") && json["animation"].isString()) {
            meta.animation = json["animation"].as<std::string>();
        }

        if (json.isMember("color")) {
            auto &color = json["color"];

            if (!color.isArray() || color.size() != 3) {
                logging::funcprint("{}, metadata color must contain exactly three byte values.\n", name);
                return std::nullopt;
            }

            qvec3b color_vec;
            for (Json::ArrayIndex i = 0; i < 3; ++i) {
                if (!color[i].isUInt() || color[i].asUInt() > std::numeric_limits<uint8_t>::max()) {
                    logging::funcprint("{}, metadata color component {} is outside [0, 255].\n", name, i);
                    return std::nullopt;
                }
                color_vec[i] = static_cast<uint8_t>(color[i].asUInt());
            }

            meta.color_override = {color_vec};
        }

        return meta;
    } catch (const Json::Exception &e) {
        logging::funcprint("{}, invalid JSON: {}\n", name, e.what());
        return std::nullopt;
    }
}

std::tuple<std::optional<img::texture_meta>, fs::resolve_result, fs::data> load_texture_meta(
    std::string_view name, const gamedef_t *game, const settings::common_settings &options)
{
    if (name.empty()) {
        return {std::nullopt, {}, {}};
    }
    if (!is_safe_texture_name(name)) {
        logging::funcprint("WARNING: refusing unsafe texture metadata path '{}'.\n", name);
        return {std::nullopt, {}, {}};
    }

    const auto try_path =
        [&](bool use_game_prefix) -> std::tuple<std::optional<img::texture_meta>, fs::resolve_result, fs::data> {
        for (auto &ext : img::meta_extension_list) {
            fs::path p = texture_path(name, use_game_prefix);
            p += ext.suffix;

            if (auto pos = fs::where(p, options.filepriority.value() == settings::search_priority_t::LOOSE)) {
                if (auto data = fs::load(pos)) {
                    if (auto texture = ext.loader(name, data, game)) {
                        return {texture, pos, data};
                    }
                }
            }
        }

        return {std::nullopt, {}, {}};
    };

    // Quake II conventionally stores WAL metadata under "textures/". Keep
    // that complete location authoritative, while allowing prefix-free asset
    // roots as a fallback. Q1-like metadata remains unprefixed as before.
    if (game->id == GAME_QUAKE_II) {
        auto result = try_path(true);
        if (std::get<0>(result)) {
            return result;
        }
    }

    return try_path(false);
}

/*
// Add empty to keep texture index in case of load problems...
auto &tex = img::textures.emplace(miptex.name, img::texture{}).first->second;

// try to load it externally first
auto [texture, _0, _1] = img::load_texture(miptex.name, false, bsp->loadversion->game, options);

if (texture) {
    tex = std::move(texture.value());
} else {
    if (miptex.data.size() <= sizeof(dmiptex_t)) {
        logging::funcprint("WARNING: can't find texture {}\n", miptex.name);
        continue;
    }

    auto loaded_tex = img::load_mip(miptex.name, miptex.data, false, bsp->loadversion->game);

    if (!loaded_tex) {
        logging::funcprint("WARNING: Texture {} is invalid\n", miptex.name);
        continue;
    }

    tex = std::move(loaded_tex.value());
}

tex.meta.averageColor = img::calculate_average(tex.pixels);
*/

qvec3b increase_saturation(const qvec3b &color)
{
    constexpr qvec3f luminance_weights{0.2126f, 0.7152f, 0.0722f};
    const qvec3f input = qvec3f(color);
    const float luminance = qv::dot(input, luminance_weights);
    const qvec3f chroma = input - qvec3f(luminance);

    // Aim for twice the chroma, reducing that factor only when a component
    // would leave the representable RGB gamut. Scaling around luminance keeps
    // brightness stable, unlike the previous square-and-renormalize method.
    float chroma_scale = 2.0f;
    for (size_t component = 0; component < 3; ++component) {
        if (chroma[component] > 0.0f) {
            chroma_scale = std::min(chroma_scale, (255.0f - luminance) / chroma[component]);
        } else if (chroma[component] < 0.0f) {
            chroma_scale = std::min(chroma_scale, -luminance / chroma[component]);
        }
    }

    qvec3b result;
    for (size_t component = 0; component < 3; ++component) {
        const float adjusted = luminance + chroma[component] * chroma_scale;
        result[component] = static_cast<uint8_t>(std::lround(std::clamp(adjusted, 0.0f, 255.0f)));
    }
    return result;
}

// Load the specified texture from the BSP
static void AddTextureName(std::string_view textureName, const mbsp_t *bsp, const settings::common_settings &options)
{
    if (textureName.empty()) {
        return;
    }

    if (img::find(textureName)) {
        return;
    }

    // always add entry
    auto &tex = img::textures.emplace(textureName, img::texture{}).first->second;

    // find texture & meta
    auto [texture, _0, _1] = img::load_texture(textureName, false, bsp->loadversion->game, options);

    if (!texture) {
        logging::funcprint("WARNING: can't find pixel data for {}\n", textureName);
    } else {
        tex = std::move(texture.value());
    }

    auto [texture_meta, __0, __1] = img::load_texture_meta(textureName, bsp->loadversion->game, options);

    if (!texture_meta) {
        logging::funcprint("WARNING: can't find meta data for {}\n", textureName);
    } else {
        tex.meta = std::move(texture_meta.value());
    }

    if (tex.meta.color_override) {
        tex.averageColor = *tex.meta.color_override;
    } else {
        tex.averageColor = img::calculate_average(tex.pixels);

        if (options.tex_saturation_boost.value() > 0.0f) {
            tex.averageColor =
                mix(tex.averageColor, increase_saturation(tex.averageColor), options.tex_saturation_boost.value());
        }
    }

    if (tex.meta.width && tex.meta.height) {
        tex.width_scale = (float)tex.width / (float)tex.meta.width;
        tex.height_scale = (float)tex.height / (float)tex.meta.height;
    }
}

// Load all of the referenced textures from the BSP texinfos into
// the texture cache.
static void LoadTextures(const mbsp_t *bsp, const settings::common_settings &options)
{
    // gather all loadable textures...
    for (auto &texinfo : bsp->texinfo) {
        AddTextureName(texinfo.texturename, bsp, options);
    }

    // gather textures used by _project_texture.
    // FIXME: I'm sure we can resolve this so we don't parse entdata twice.
    auto entdicts = EntData_Parse(*bsp);
    for (auto &entdict : entdicts) {
        if (entdict.get("classname").find("light") == 0) {
            const auto &tex = entdict.get("_project_texture");
            if (!tex.empty()) {
                AddTextureName(tex.c_str(), bsp, options);
            }
        }
    }
}

// Load all of the paletted textures from the BSP into
// the texture cache.
static void ConvertTextures(const mbsp_t *bsp, const settings::common_settings &options)
{
    if (!bsp->dtex.textures.size()) {
        return;
    }

    for (auto &miptex : bsp->dtex.textures) {
        // A -1 BSP texture-lump offset is represented as a null texture. It
        // reserves an index but is not an image to cache or diagnose.
        if (miptex.null_texture || miptex.name.empty()) {
            continue;
        }

        if (img::find(miptex.name)) {
            logging::funcprint("WARNING: Texture {} duplicated\n", miptex.name);
            continue;
        }

        // always add entry
        auto &tex = img::textures.emplace(miptex.name, img::texture{}).first->second;

        // if the miptex entry isn't a dummy, use it as our base
        if (miptex.data.size() >= sizeof(dmiptex_t)) {
            if (auto loaded_tex = img::load_mip(miptex.name, miptex.data, false, bsp->loadversion->game)) {
                tex = std::move(loaded_tex.value());
            }
        }

        // find replacement texture
        if (auto [texture, _0, _1] = img::load_texture(miptex.name, false, bsp->loadversion->game, options); texture) {
            tex.width = texture->width;
            tex.height = texture->height;
            tex.pixels = std::move(texture->pixels);
        }

        if (!tex.pixels.size() || !tex.width || !tex.meta.width) {
            logging::funcprint("WARNING: invalid size data for {}\n", miptex.name);
            continue;
        }

        if (tex.meta.color_override) {
            tex.averageColor = *tex.meta.color_override;
        } else {
            tex.averageColor = img::calculate_average(tex.pixels);

            if (options.tex_saturation_boost.value() > 0.0f) {
                tex.averageColor =
                    mix(tex.averageColor, increase_saturation(tex.averageColor), options.tex_saturation_boost.value());
            }
        }

        if (tex.meta.width && tex.meta.height) {
            tex.width_scale = (float)tex.width / (float)tex.meta.width;
            tex.height_scale = (float)tex.height / (float)tex.meta.height;
        }
    }
}

void load_textures(const mbsp_t *bsp, const settings::common_settings &options)
{
    logging::funcheader();

    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        LoadTextures(bsp, options);
    } else if (bsp->dtex.textures.size() > 0) {
        ConvertTextures(bsp, options);
    } else {
        logging::print("WARNING: failed to load or convert textures.\n");
    }
}
} // namespace img
