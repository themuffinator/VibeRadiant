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

#include <common/cmdlib.hh>
#include <common/mathlib.hh>
#include <common/bspfile.hh>
#include <common/fs.hh>
#include <common/imglib.hh>
#include <common/log.hh>
#include <common/settings.hh>
#include <common/numeric_cast.hh>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <limits.h>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/core.h>

#include <atomic>
#include <mutex>

static std::vector<qvec3b> make_palette(std::initializer_list<uint8_t> bytes)
{
    Q_assert((bytes.size() % 3) == 0);

    std::vector<qvec3b> result;
    result.reserve(bytes.size() / 3);

    for (const uint8_t *it = bytes.begin(); it < bytes.end(); it += 3) {
        result.emplace_back(it[0], it[1], it[2]);
    }

    return result;
}

// clang-format off
/**
 * Keep up to date with contents_t enum.
 */
static constexpr const char *bitflag_names[] = {
    "SOLID", // bit 0
    "SKY", // bit 1
    "DETAIL_WALL", // bit 2
    "WINDOW", // bit 3
    "ILLUSIONARY_VISBLOCKER", // bit 4
    "AUX", // bit 5
    "LAVA", // bit 6
    "SLIME", // bit 7
    "WATER", // bit 8
    "MIST", // bit 9
    "ORIGIN", // bit 10
    "PLAYERCLIP", // bit 11
    "MONSTERCLIP", // bit 12
    "AREAPORTAL", // bit 13
    "NO_WATERJUMP", // bit 14
    "PROJECTILECLIP", // bit 15
    "MIRROR_INSIDE", // bit 16
    "MIRROR_INSIDE_SET", // bit 17
    "SUPPRESS_CLIPPING_SAME_TYPE", // bit 18
    "CURRENT_0", // bit 19
    "CURRENT_90", // bit 20
    "CURRENT_180", // bit 21
    "CURRENT_270", // bit 22
    "CURRENT_UP", // bit 23
    "CURRENT_DOWN", // bit 24
    "TRANSLUCENT", // bit 25
    "LADDER", // bit 26
    "MONSTER", // bit 27
    "DEADMONSTER", // bit 28
    "DETAIL", // bit 29
    "Q2_UNUSED_7", // bit 30
    "Q2_UNUSED_8", // bit 31
    "Q2_UNUSED_9", // bit 32
    "Q2_UNUSED_10", // bit 33
    "Q2_UNUSED_11", // bit 34
    "Q2_UNUSED_12", // bit 35
    "Q2_UNUSED_30", // bit 36
    "Q2_UNUSED_31", // bit 37
    "INVALID_BIT_38", // bit 38
    "INVALID_BIT_39", // bit 39
    "INVALID_BIT_40", // bit 40
    "INVALID_BIT_41", // bit 41
    "INVALID_BIT_42", // bit 42
    "INVALID_BIT_43", // bit 43
    "INVALID_BIT_44", // bit 44
    "INVALID_BIT_45", // bit 45
    "INVALID_BIT_46", // bit 46
    "INVALID_BIT_47", // bit 47
    "INVALID_BIT_48", // bit 48
    "INVALID_BIT_49", // bit 49
    "INVALID_BIT_50", // bit 50
    "INVALID_BIT_51", // bit 51
    "INVALID_BIT_52", // bit 52
    "INVALID_BIT_53", // bit 53
    "INVALID_BIT_54", // bit 54
    "INVALID_BIT_55", // bit 55
    "INVALID_BIT_56", // bit 56
    "INVALID_BIT_57", // bit 57
    "INVALID_BIT_58", // bit 58
    "INVALID_BIT_59", // bit 59
    "INVALID_BIT_60", // bit 60
    "INVALID_BIT_61", // bit 61
    "INVALID_BIT_62", // bit 62
    "INVALID_BIT_63" // bit 63
};
// clang-format on

std::string get_contents_display(contents_t bits)
{
    if (bits == EWT_VISCONTENTS_EMPTY) {
        return "EMPTY";
    }

    std::string s;

    for (uint32_t i = 0; i < std::size(bitflag_names); i++) {
        if (bits & nth_bit<contents_int_t>(i)) {
            if (!s.empty()) {
                s += " | ";
            }

            s += bitflag_names[i];
        }
    }

    return s;
}

Json::Value get_contents_json(contents_t bits)
{
    Json::Value result = Json::Value(Json::arrayValue);

    for (uint32_t i = 0; i < std::size(bitflag_names); i++) {
        if (bits & nth_bit<contents_int_t>(i)) {
            result.append(bitflag_names[i]);
        }
    }

    return result;
}

contents_int_t set_contents_json(const Json::Value &json)
{
    contents_int_t result = EWT_VISCONTENTS_EMPTY;

    if (!json.isArray()) {
        return result;
    }

    for (const Json::Value &entry : json) {
        if (!entry.isString())
            continue;

        const std::string entry_str = entry.asString();

        for (uint32_t i = 0; i < std::size(bitflag_names); i++) {
            if (!Q_strcasecmp(entry_str.c_str(), bitflag_names[i]))
                result |= nth_bit<contents_int_t>(i);
        }
    }

    return result;
}

template<gameid_t ID>
struct gamedef_q1_like_t : public gamedef_t
{
public:
    explicit gamedef_q1_like_t(const char *friendly_name = "quake", const char *base_dir = "ID1")
        : gamedef_t(friendly_name, base_dir)
    {
        this->id = ID;
    }

    bool surf_is_lightmapped(
        const surfflags_t &flags, const char *texname, bool light_nodraw, bool lightgrid_enabled) const override
    {
        /* don't save lightmaps for "trigger" texture */
        if (!Q_strcasecmp(texname, "trigger"))
            return false;

        /* don't save lightmaps for "skip" texture */
        if (!Q_strcasecmp(texname, "skip"))
            return false;

        return !(flags.native_q1 & TEX_SPECIAL);
    }

    bool surf_is_emissive(const surfflags_t &flags, const char *texname) const override
    {
        /* don't save lightmaps for "trigger" texture */
        if (!Q_strcasecmp(texname, "trigger"))
            return false;

        return true;
    }

    bool surf_is_subdivided(const surfflags_t &flags) const override { return !(flags.native_q1 & TEX_SPECIAL); }

    bool surfflags_are_valid(const surfflags_t &flags) const override
    {
        // Q1 only supports TEX_SPECIAL
        return (flags.native_q1 & ~TEX_SPECIAL) == 0;
    }

    bool surfflags_may_phong(const surfflags_t &a, const surfflags_t &b) const override
    {
        return (a.native_q1 & TEX_SPECIAL) == (b.native_q1 & TEX_SPECIAL);
    }

    int32_t surfflags_from_string(std::string_view str) const override
    {
        if (string_iequals(str, "special")) {
            return TEX_SPECIAL;
        }

        return 0;
    }

    contentflags_t create_contents_from_native(int32_t native) const override
    {
        switch (native) {
            case CONTENTS_SOLID: return contentflags_t::make(EWT_VISCONTENTS_SOLID);
            case CONTENTS_SKY: return contentflags_t::make(EWT_VISCONTENTS_SKY);
            case CONTENTS_LAVA: return contentflags_t::make(EWT_VISCONTENTS_LAVA);
            case CONTENTS_SLIME: return contentflags_t::make(EWT_VISCONTENTS_SLIME);
            case CONTENTS_WATER: return contentflags_t::make(EWT_VISCONTENTS_WATER);
            case CONTENTS_EMPTY: return contentflags_t::make(EWT_VISCONTENTS_EMPTY);
        }

        if (allows_hl_contents) {
            // clang-format off
            switch (native) {
                case HL_CONTENTS_CURRENT_0: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_0);
                case HL_CONTENTS_CURRENT_90: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_90);
                case HL_CONTENTS_CURRENT_180: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_180);
                case HL_CONTENTS_CURRENT_270: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_270);
                case HL_CONTENTS_CURRENT_UP: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_UP);
                case HL_CONTENTS_CURRENT_DOWN: return contentflags_t::make(EWT_VISCONTENTS_WATER | EWT_CFLAG_CURRENT_DOWN);
            }
            // clang-format on
        }

        throw std::invalid_argument(fmt::format("create_contents_from_native: unknown Q1 contents {}", native));
    }

    int32_t contents_to_native(contentflags_t contents) const override
    {
        if (contents.flags & EWT_VISCONTENTS_SOLID) {
            return CONTENTS_SOLID;
        } else if (contents.flags & EWT_VISCONTENTS_SKY) {
            return CONTENTS_SKY;
        } else if (contents.flags & EWT_VISCONTENTS_DETAIL_WALL) {
            throw std::invalid_argument("EWT_VISCONTENTS_DETAIL_WALL not representable in Q1");
        } else if (contents.flags & EWT_VISCONTENTS_WINDOW) {
            throw std::invalid_argument("EWT_VISCONTENTS_WINDOW not representable in Q1");
        } else if (contents.flags & EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER) {
            throw std::invalid_argument("EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER not representable in Q1");
        } else if (contents.flags & EWT_VISCONTENTS_AUX) {
            throw std::invalid_argument("EWT_VISCONTENTS_AUX not representable in Q1");
        } else if (contents.flags & EWT_VISCONTENTS_LAVA) {
            return CONTENTS_LAVA;
        } else if (contents.flags & EWT_VISCONTENTS_SLIME) {
            return CONTENTS_SLIME;
        } else if (contents.flags & EWT_VISCONTENTS_WATER) {
            if (allows_hl_contents) {
                if (contents.flags & EWT_CFLAG_CURRENT_0)
                    return HL_CONTENTS_CURRENT_0;
                if (contents.flags & EWT_CFLAG_CURRENT_90)
                    return HL_CONTENTS_CURRENT_90;
                if (contents.flags & EWT_CFLAG_CURRENT_180)
                    return HL_CONTENTS_CURRENT_180;
                if (contents.flags & EWT_CFLAG_CURRENT_270)
                    return HL_CONTENTS_CURRENT_270;
                if (contents.flags & EWT_CFLAG_CURRENT_UP)
                    return HL_CONTENTS_CURRENT_UP;
                if (contents.flags & EWT_CFLAG_CURRENT_DOWN)
                    return HL_CONTENTS_CURRENT_DOWN;
            }

            return CONTENTS_WATER;
        } else if (contents.flags & EWT_VISCONTENTS_MIST) {
            throw std::invalid_argument("EWT_VISCONTENTS_MIST not representable in Q1");
        }
        return CONTENTS_EMPTY;
    }

    int32_t contents_from_string(std::string_view str) const override
    {
        // Q1 doesn't get contents from files
        return 0;
    }

    contentflags_t contents_remap_for_export(contentflags_t contents, remap_type_t type) const override
    {
        /*
         * This is for func_detail_wall.. we want to write a solid leaf that has faces,
         * because it may be possible to see inside (fence textures).
         *
         * Normally solid leafs are not written and just referenced as leaf 0.
         */
        if (contents.flags & EWT_VISCONTENTS_WINDOW) {
            // Q1 represents detail fences as solid leaves with visible faces.
            // Preserve any other representable contents carried by the leaf.
            contents = contentflags_t::make((contents.flags & ~EWT_VISCONTENTS_WINDOW) | EWT_VISCONTENTS_SOLID);
        }
        if (contents.flags & EWT_VISCONTENTS_DETAIL_WALL) {
            contents = contentflags_t::make((contents.flags & ~EWT_VISCONTENTS_DETAIL_WALL) | EWT_VISCONTENTS_SOLID);
        }

        if (contents.flags & (EWT_VISCONTENTS_MIST | EWT_VISCONTENTS_AUX)) {
            // Clear Q2-only mist and aux. detail_illusionary on its own becomes CONTENTS_EMPTY,
            // detail_illusionary in water becomes CONTENTS_WATER, etc.
            contents = contentflags_t::make(contents.flags & ~(EWT_VISCONTENTS_MIST | EWT_VISCONTENTS_AUX));
        }
        if (contents.flags & EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER) {
            // this exports as empty
            contents = contentflags_t::make(contents.flags & ~EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER);
        }

        return contents;
    }

    std::span<const aabb3d> get_hull_sizes() const override
    {
        static constexpr aabb3d hulls[] = {
            {{0, 0, 0}, {0, 0, 0}}, {{-16, -16, -32}, {16, 16, 24}}, {{-32, -32, -64}, {32, 32, 24}}};

        return hulls;
    }

    contentflags_t face_get_contents(
        const std::string &texname, const surfflags_t &flags, contentflags_t, bool transwater) const override
    {
        // check for strong content indicators
        if (!Q_strcasecmp(texname.data(), "origin")) {
            return contentflags_t::make(EWT_INVISCONTENTS_ORIGIN);
        } else if (!Q_strcasecmp(texname.data(), "hint") || !Q_strcasecmp(texname.data(), "hintskip")) {
            return contentflags_t::make(EWT_VISCONTENTS_EMPTY);
        } else if (!Q_strcasecmp(texname.data(), "clip")) {
            return contentflags_t::make(EWT_INVISCONTENTS_PLAYERCLIP);
        } else if ((texname[0] == '*') || (texname[0] == '!' && allows_hl_contents)) {
            // non-Q2: -transwater implies liquids are detail and translucent
            contents_int_t liquid_flags = 0;
            if (transwater) {
                liquid_flags = EWT_CFLAG_DETAIL | EWT_CFLAG_TRANSLUCENT;
            }

            // HL water with current
            if (allows_hl_contents) {
                contents_int_t current_flag = 0;

                if (string_istarts_with(texname, "!cur_0")) {
                    current_flag = EWT_CFLAG_CURRENT_0;
                } else if (string_istarts_with(texname, "!cur_90")) {
                    current_flag = EWT_CFLAG_CURRENT_90;
                } else if (string_istarts_with(texname, "!cur_180")) {
                    current_flag = EWT_CFLAG_CURRENT_180;
                } else if (string_istarts_with(texname, "!cur_270")) {
                    current_flag = EWT_CFLAG_CURRENT_270;
                } else if (string_istarts_with(texname, "!cur_up")) {
                    current_flag = EWT_CFLAG_CURRENT_UP;
                } else if (string_istarts_with(texname, "!cur_dwn")) {
                    current_flag = EWT_CFLAG_CURRENT_DOWN;
                }

                if (current_flag) {
                    return contentflags_t::make(EWT_VISCONTENTS_WATER | liquid_flags | current_flag);
                }
            }

            if (!Q_strncasecmp(texname.data() + 1, "lava", 4)) {
                return contentflags_t::make(EWT_VISCONTENTS_LAVA | liquid_flags);
            } else if (!Q_strncasecmp(texname.data() + 1, "slime", 5)) {
                return contentflags_t::make(EWT_VISCONTENTS_SLIME | liquid_flags);
            } else {
                return contentflags_t::make(EWT_VISCONTENTS_WATER | liquid_flags);
            }
        } else if (!Q_strncasecmp(texname.data(), "sky", 3)) {
            return contentflags_t::make(EWT_VISCONTENTS_SKY);
        }

        // and anything else is assumed to be a regular solid.
        return contentflags_t::make(EWT_VISCONTENTS_SOLID);
    }

    void init_filesystem(const fs::path &map_or_bsp, const settings::common_settings &options) const override
    {
        img::clear();
        // Q1-like games don't care about the local
        // filesystem.
        // they do care about the palette though.
        fs::clear();

        for (auto &path : options.paths.values()) {
            fs::addArchive(path, true);
        }

        // certain features like '-add additional.map' search relative to the map we're compiling
        // so add the map directory to the search path
        auto map_or_bsp_dir = map_or_bsp.parent_path();
        if (!map_or_bsp_dir.empty()) {
            fs::addArchive(map_or_bsp_dir);
        }

        img::init_palette(this, options.filepriority.value() == settings::search_priority_t::LOOSE);
    }

    const std::vector<qvec3b> &get_default_palette() const override
    {
        static constexpr std::initializer_list<uint8_t> palette_bytes{0, 0, 0, 15, 15, 15, 31, 31, 31, 47, 47, 47, 63,
            63, 63, 75, 75, 75, 91, 91, 91, 107, 107, 107, 123, 123, 123, 139, 139, 139, 155, 155, 155, 171, 171, 171,
            187, 187, 187, 203, 203, 203, 219, 219, 219, 235, 235, 235, 15, 11, 7, 23, 15, 11, 31, 23, 11, 39, 27, 15,
            47, 35, 19, 55, 43, 23, 63, 47, 23, 75, 55, 27, 83, 59, 27, 91, 67, 31, 99, 75, 31, 107, 83, 31, 115, 87,
            31, 123, 95, 35, 131, 103, 35, 143, 111, 35, 11, 11, 15, 19, 19, 27, 27, 27, 39, 39, 39, 51, 47, 47, 63, 55,
            55, 75, 63, 63, 87, 71, 71, 103, 79, 79, 115, 91, 91, 127, 99, 99, 139, 107, 107, 151, 115, 115, 163, 123,
            123, 175, 131, 131, 187, 139, 139, 203, 0, 0, 0, 7, 7, 0, 11, 11, 0, 19, 19, 0, 27, 27, 0, 35, 35, 0, 43,
            43, 7, 47, 47, 7, 55, 55, 7, 63, 63, 7, 71, 71, 7, 75, 75, 11, 83, 83, 11, 91, 91, 11, 99, 99, 11, 107, 107,
            15, 7, 0, 0, 15, 0, 0, 23, 0, 0, 31, 0, 0, 39, 0, 0, 47, 0, 0, 55, 0, 0, 63, 0, 0, 71, 0, 0, 79, 0, 0, 87,
            0, 0, 95, 0, 0, 103, 0, 0, 111, 0, 0, 119, 0, 0, 127, 0, 0, 19, 19, 0, 27, 27, 0, 35, 35, 0, 47, 43, 0, 55,
            47, 0, 67, 55, 0, 75, 59, 7, 87, 67, 7, 95, 71, 7, 107, 75, 11, 119, 83, 15, 131, 87, 19, 139, 91, 19, 151,
            95, 27, 163, 99, 31, 175, 103, 35, 35, 19, 7, 47, 23, 11, 59, 31, 15, 75, 35, 19, 87, 43, 23, 99, 47, 31,
            115, 55, 35, 127, 59, 43, 143, 67, 51, 159, 79, 51, 175, 99, 47, 191, 119, 47, 207, 143, 43, 223, 171, 39,
            239, 203, 31, 255, 243, 27, 11, 7, 0, 27, 19, 0, 43, 35, 15, 55, 43, 19, 71, 51, 27, 83, 55, 35, 99, 63, 43,
            111, 71, 51, 127, 83, 63, 139, 95, 71, 155, 107, 83, 167, 123, 95, 183, 135, 107, 195, 147, 123, 211, 163,
            139, 227, 179, 151, 171, 139, 163, 159, 127, 151, 147, 115, 135, 139, 103, 123, 127, 91, 111, 119, 83, 99,
            107, 75, 87, 95, 63, 75, 87, 55, 67, 75, 47, 55, 67, 39, 47, 55, 31, 35, 43, 23, 27, 35, 19, 19, 23, 11, 11,
            15, 7, 7, 187, 115, 159, 175, 107, 143, 163, 95, 131, 151, 87, 119, 139, 79, 107, 127, 75, 95, 115, 67, 83,
            107, 59, 75, 95, 51, 63, 83, 43, 55, 71, 35, 43, 59, 31, 35, 47, 23, 27, 35, 19, 19, 23, 11, 11, 15, 7, 7,
            219, 195, 187, 203, 179, 167, 191, 163, 155, 175, 151, 139, 163, 135, 123, 151, 123, 111, 135, 111, 95, 123,
            99, 83, 107, 87, 71, 95, 75, 59, 83, 63, 51, 67, 51, 39, 55, 43, 31, 39, 31, 23, 27, 19, 15, 15, 11, 7, 111,
            131, 123, 103, 123, 111, 95, 115, 103, 87, 107, 95, 79, 99, 87, 71, 91, 79, 63, 83, 71, 55, 75, 63, 47, 67,
            55, 43, 59, 47, 35, 51, 39, 31, 43, 31, 23, 35, 23, 15, 27, 19, 11, 19, 11, 7, 11, 7, 255, 243, 27, 239,
            223, 23, 219, 203, 19, 203, 183, 15, 187, 167, 15, 171, 151, 11, 155, 131, 7, 139, 115, 7, 123, 99, 7, 107,
            83, 0, 91, 71, 0, 75, 55, 0, 59, 43, 0, 43, 31, 0, 27, 15, 0, 11, 7, 0, 0, 0, 255, 11, 11, 239, 19, 19, 223,
            27, 27, 207, 35, 35, 191, 43, 43, 175, 47, 47, 159, 47, 47, 143, 47, 47, 127, 47, 47, 111, 47, 47, 95, 43,
            43, 79, 35, 35, 63, 27, 27, 47, 19, 19, 31, 11, 11, 15, 43, 0, 0, 59, 0, 0, 75, 7, 0, 95, 7, 0, 111, 15, 0,
            127, 23, 7, 147, 31, 7, 163, 39, 11, 183, 51, 15, 195, 75, 27, 207, 99, 43, 219, 127, 59, 227, 151, 79, 231,
            171, 95, 239, 191, 119, 247, 211, 139, 167, 123, 59, 183, 155, 55, 199, 195, 55, 231, 227, 87, 127, 191,
            255, 171, 231, 255, 215, 255, 255, 103, 0, 0, 139, 0, 0, 179, 0, 0, 215, 0, 0, 255, 0, 0, 255, 243, 147,
            255, 247, 199, 255, 255, 255, 159, 91, 83};

        static const auto palette = make_palette(palette_bytes);
        return palette;
    }
};

struct gamedef_h2_t : public gamedef_q1_like_t<GAME_HEXEN_II>
{
    gamedef_h2_t()
        : gamedef_q1_like_t("hexen2", "DATA1")
    {
    }

    std::span<const aabb3d> get_hull_sizes() const override
    {
        static constexpr aabb3d hulls[] = {{{0, 0, 0}, {0, 0, 0}}, {{-16, -16, -32}, {16, 16, 24}},
            {{-24, -24, -20}, {24, 24, 20}}, {{-16, -16, -16}, {16, 16, 12}},
            {{-8, -8, -8}, {8, 8, 8}}, // {{-40, -40, -42}, {40, 40, 42}} = original game
            {{-28, -28, -40}, {28, 28, 40}}};

        return hulls;
    }

    const std::vector<qvec3b> &get_default_palette() const override
    {
        static constexpr std::initializer_list<uint8_t> palette_bytes{0, 0, 0, 0, 0, 0, 8, 8, 8, 16, 16, 16, 24, 24, 24,
            32, 32, 32, 40, 40, 40, 48, 48, 48, 56, 56, 56, 64, 64, 64, 72, 72, 72, 80, 80, 80, 84, 84, 84, 88, 88, 88,
            96, 96, 96, 104, 104, 104, 112, 112, 112, 120, 120, 120, 128, 128, 128, 136, 136, 136, 148, 148, 148, 156,
            156, 156, 168, 168, 168, 180, 180, 180, 184, 184, 184, 196, 196, 196, 204, 204, 204, 212, 212, 212, 224,
            224, 224, 232, 232, 232, 240, 240, 240, 252, 252, 252, 8, 8, 12, 16, 16, 20, 24, 24, 28, 28, 32, 36, 36, 36,
            44, 44, 44, 52, 48, 52, 60, 56, 56, 68, 64, 64, 72, 76, 76, 88, 92, 92, 104, 108, 112, 128, 128, 132, 152,
            152, 156, 176, 168, 172, 196, 188, 196, 220, 32, 24, 20, 40, 32, 28, 48, 36, 32, 52, 44, 40, 60, 52, 44, 68,
            56, 52, 76, 64, 56, 84, 72, 64, 92, 76, 72, 100, 84, 76, 108, 92, 84, 112, 96, 88, 120, 104, 96, 128, 112,
            100, 136, 116, 108, 144, 124, 112, 20, 24, 20, 28, 32, 28, 32, 36, 32, 40, 44, 40, 44, 48, 44, 48, 56, 48,
            56, 64, 56, 64, 68, 64, 68, 76, 68, 84, 92, 84, 104, 112, 104, 120, 128, 120, 140, 148, 136, 156, 164, 152,
            172, 180, 168, 188, 196, 184, 48, 32, 8, 60, 40, 8, 72, 48, 16, 84, 56, 20, 92, 64, 28, 100, 72, 36, 108,
            80, 44, 120, 92, 52, 136, 104, 60, 148, 116, 72, 160, 128, 84, 168, 136, 92, 180, 144, 100, 188, 152, 108,
            196, 160, 116, 204, 168, 124, 16, 20, 16, 20, 28, 20, 24, 32, 24, 28, 36, 28, 32, 44, 32, 36, 48, 36, 40,
            56, 40, 44, 60, 44, 48, 68, 48, 52, 76, 52, 60, 84, 60, 68, 92, 64, 76, 100, 72, 84, 108, 76, 92, 116, 84,
            100, 128, 92, 24, 12, 8, 32, 16, 8, 40, 20, 8, 52, 24, 12, 60, 28, 12, 68, 32, 12, 76, 36, 16, 84, 44, 20,
            92, 48, 24, 100, 56, 28, 112, 64, 32, 120, 72, 36, 128, 80, 44, 144, 92, 56, 168, 112, 72, 192, 132, 88, 24,
            4, 4, 36, 4, 4, 48, 0, 0, 60, 0, 0, 68, 0, 0, 80, 0, 0, 88, 0, 0, 100, 0, 0, 112, 0, 0, 132, 0, 0, 152, 0,
            0, 172, 0, 0, 192, 0, 0, 212, 0, 0, 232, 0, 0, 252, 0, 0, 16, 12, 32, 28, 20, 48, 32, 28, 56, 40, 36, 68,
            52, 44, 80, 60, 56, 92, 68, 64, 104, 80, 72, 116, 88, 84, 128, 100, 96, 140, 108, 108, 152, 120, 116, 164,
            132, 132, 176, 144, 144, 188, 156, 156, 200, 172, 172, 212, 36, 20, 4, 52, 24, 4, 68, 32, 4, 80, 40, 0, 100,
            48, 4, 124, 60, 4, 140, 72, 4, 156, 88, 8, 172, 100, 8, 188, 116, 12, 204, 128, 12, 220, 144, 16, 236, 160,
            20, 252, 184, 56, 248, 200, 80, 248, 220, 120, 20, 16, 4, 28, 24, 8, 36, 32, 8, 44, 40, 12, 52, 48, 16, 56,
            56, 16, 64, 64, 20, 68, 72, 24, 72, 80, 28, 80, 92, 32, 84, 104, 40, 88, 116, 44, 92, 128, 52, 92, 140, 52,
            92, 148, 56, 96, 160, 64, 60, 16, 16, 72, 24, 24, 84, 28, 28, 100, 36, 36, 112, 44, 44, 124, 52, 48, 140,
            64, 56, 152, 76, 64, 44, 20, 8, 56, 28, 12, 72, 32, 16, 84, 40, 20, 96, 44, 28, 112, 52, 32, 124, 56, 40,
            140, 64, 48, 24, 20, 16, 36, 28, 20, 44, 36, 28, 56, 44, 32, 64, 52, 36, 72, 60, 44, 80, 68, 48, 92, 76, 52,
            100, 84, 60, 112, 92, 68, 120, 100, 72, 132, 112, 80, 144, 120, 88, 152, 128, 96, 160, 136, 104, 168, 148,
            112, 36, 24, 12, 44, 32, 16, 52, 40, 20, 60, 44, 20, 72, 52, 24, 80, 60, 28, 88, 68, 28, 104, 76, 32, 148,
            96, 56, 160, 108, 64, 172, 116, 72, 180, 124, 80, 192, 132, 88, 204, 140, 92, 216, 156, 108, 60, 20, 92,
            100, 36, 116, 168, 72, 164, 204, 108, 192, 4, 84, 4, 4, 132, 4, 0, 180, 0, 0, 216, 0, 4, 4, 144, 16, 68,
            204, 36, 132, 224, 88, 168, 232, 216, 4, 4, 244, 72, 0, 252, 128, 0, 252, 172, 24, 252, 252, 252};

        static const auto palette = make_palette(palette_bytes);
        return palette;
    }
};

struct gamedef_hl_t : public gamedef_q1_like_t<GAME_HALF_LIFE>
{
    gamedef_hl_t()
        : gamedef_q1_like_t("halflife", "VALVE")
    {
        has_rgb_lightmap = true;
        allows_hl_contents = true;
    }

    std::span<const aabb3d> get_hull_sizes() const override
    {
        static constexpr aabb3d hulls[] = {{{0, 0, 0}, {0, 0, 0}}, {{-16, -16, -36}, {16, 16, 36}},
            {{-32, -32, -32}, {32, 32, 32}}, {{-16, -16, -18}, {16, 16, 18}}};

        return hulls;
    }

    const std::vector<qvec3b> &get_default_palette() const override
    {
        static const std::vector<qvec3b> palette;
        return palette;
    }
};

struct gamedef_q2_t : public gamedef_t
{
    gamedef_q2_t()
        : gamedef_t("quake2", "baseq2")
    {
        this->id = GAME_QUAKE_II;
        has_rgb_lightmap = true;
        allow_contented_bmodels = true;
        max_entity_key = 256;
    }

    bool surf_is_lightmapped(
        const surfflags_t &flags, const char *texname, bool light_nodraw, bool lightgrid_enabled) const override
    {
        /* don't save lightmaps for "trigger" texture even if light_nodraw is set */
        if (std::string_view(texname).ends_with("/trigger"))
            return false;

        // Q2RTX should light nodraw faces
        if (light_nodraw && flags.is_nodraw()) {
            return true;
        }

        // The only reason to lightmap sky faces in Q2 is to light models floating over sky.
        // If lightgrid is in use, this reason is no longer relevant, so skip lightmapping.
        if (lightgrid_enabled && (flags.native_q2 & Q2_SURF_SKY)) {
            return false;
        }

        return !(flags.native_q2 & (Q2_SURF_NODRAW | Q2_SURF_SKIP));
    }

    bool surf_is_emissive(const surfflags_t &flags, const char *texname) const override { return true; }

    bool surf_is_subdivided(const surfflags_t &flags) const override { return !(flags.native_q2 & Q2_SURF_SKY); }

    bool surfflags_are_valid(const surfflags_t &flags) const override
    {
        // no rules in Quake II baby
        return true;
    }

    bool surfflags_may_phong(const surfflags_t &a, const surfflags_t &b) const override
    {
        // these are the bits we'll require to match in order to allow phonging `a` and `b`
        auto mask = [](const surfflags_t &flags) {
            return flags.native_q2 &
                   (Q2_SURF_SKY | Q2_SURF_WARP | Q2_SURF_TRANS33 | Q2_SURF_TRANS66 | Q2_SURF_FLOWING | Q2_SURF_NODRAW);
        };

        return mask(a) == mask(b);
    }

    static constexpr const char *surf_bitflag_names[] = {"LIGHT", "SLICK", "SKY", "WARP", "TRANS33", "TRANS66",
        "FLOWING", "NODRAW", "HINT", "512", "1024", "2048", "4096", "8192", "16384", "32768", "65536", "131072",
        "262144", "524288", "1048576", "2097152", "4194304", "8388608", "16777216", "ALPHATEST"};

    int32_t surfflags_from_string(std::string_view str) const override
    {
        for (size_t i = 0; i < std::size(surf_bitflag_names); i++) {
            if (string_iequals(str, surf_bitflag_names[i])) {
                return nth_bit(i);
            }
        }

        return 0;
    }

    contentflags_t create_contents_from_native(int32_t native) const override
    {
        contents_int_t result = 0;

        // visible contents
        if (native & Q2_CONTENTS_SOLID)
            result |= EWT_VISCONTENTS_SOLID;
        if (native & Q2_CONTENTS_WINDOW)
            result |= EWT_VISCONTENTS_WINDOW;
        if (native & Q2_CONTENTS_AUX)
            result |= EWT_VISCONTENTS_AUX;
        if (native & Q2_CONTENTS_LAVA)
            result |= EWT_VISCONTENTS_LAVA;
        if (native & Q2_CONTENTS_SLIME)
            result |= EWT_VISCONTENTS_SLIME;
        if (native & Q2_CONTENTS_WATER)
            result |= EWT_VISCONTENTS_WATER;
        if (native & Q2_CONTENTS_MIST)
            result |= EWT_VISCONTENTS_MIST;

        // invisible contents
        if (native & Q2_CONTENTS_AREAPORTAL)
            result |= EWT_INVISCONTENTS_AREAPORTAL;
        if (native & Q2_CONTENTS_PLAYERCLIP)
            result |= EWT_INVISCONTENTS_PLAYERCLIP;
        if (native & Q2_CONTENTS_MONSTERCLIP)
            result |= EWT_INVISCONTENTS_MONSTERCLIP;
        if (native & Q2_CONTENTS_PROJECTILECLIP)
            result |= EWT_INVISCONTENTS_PROJECTILECLIP;
        if (native & Q2_CONTENTS_ORIGIN)
            result |= EWT_INVISCONTENTS_ORIGIN;
        if (native & Q2_CONTENTS_NO_WATERJUMP)
            result |= EWT_INVISCONTENTS_NO_WATERJUMP;

        // contents flags
        if (native & Q2_CONTENTS_CURRENT_0)
            result |= EWT_CFLAG_CURRENT_0;
        if (native & Q2_CONTENTS_CURRENT_90)
            result |= EWT_CFLAG_CURRENT_90;
        if (native & Q2_CONTENTS_CURRENT_180)
            result |= EWT_CFLAG_CURRENT_180;
        if (native & Q2_CONTENTS_CURRENT_270)
            result |= EWT_CFLAG_CURRENT_270;
        if (native & Q2_CONTENTS_CURRENT_UP)
            result |= EWT_CFLAG_CURRENT_UP;
        if (native & Q2_CONTENTS_CURRENT_DOWN)
            result |= EWT_CFLAG_CURRENT_DOWN;
        if (native & Q2_CONTENTS_DETAIL)
            result |= EWT_CFLAG_DETAIL;
        if (native & Q2_CONTENTS_TRANSLUCENT)
            result |= EWT_CFLAG_TRANSLUCENT;
        if (native & Q2_CONTENTS_LADDER)
            result |= EWT_CFLAG_LADDER;

        // disallowed flags
        if (native & Q2_CONTENTS_MONSTER)
            result |= EWT_CFLAG_MONSTER;
        if (native & Q2_CONTENTS_DEADMONSTER)
            result |= EWT_CFLAG_DEADMONSTER;

        // other unused flags which are illegal
        if (native & Q2_CONTENTS_UNUSED_7)
            result |= EWT_CFLAG_Q2_UNUSED_7;
        if (native & Q2_CONTENTS_UNUSED_8)
            result |= EWT_CFLAG_Q2_UNUSED_8;
        if (native & Q2_CONTENTS_UNUSED_9)
            result |= EWT_CFLAG_Q2_UNUSED_9;
        if (native & Q2_CONTENTS_UNUSED_10)
            result |= EWT_CFLAG_Q2_UNUSED_10;
        if (native & Q2_CONTENTS_UNUSED_11)
            result |= EWT_CFLAG_Q2_UNUSED_11;
        if (native & Q2_CONTENTS_UNUSED_12)
            result |= EWT_CFLAG_Q2_UNUSED_12;
        if (native & Q2_CONTENTS_UNUSED_30)
            result |= EWT_CFLAG_Q2_UNUSED_30;
        if (native & Q2_CONTENTS_UNUSED_31)
            result |= EWT_CFLAG_Q2_UNUSED_31;

        return contentflags_t::make(result);
    }

    int32_t contents_to_native(contentflags_t contents) const override
    {
        int32_t result = 0;

        if (contents.flags & EWT_VISCONTENTS_SOLID)
            result |= Q2_CONTENTS_SOLID;
        if (contents.flags & EWT_VISCONTENTS_SKY)
            throw std::invalid_argument("sky not a contents in Q2");
        if (contents.flags & EWT_VISCONTENTS_DETAIL_WALL)
            throw std::invalid_argument("detail wall not a contents in Q2");
        if (contents.flags & EWT_VISCONTENTS_WINDOW)
            result |= Q2_CONTENTS_WINDOW;
        if (contents.flags & EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER)
            throw std::invalid_argument("illusionary visblocker not a contents in Q2");
        if (contents.flags & EWT_VISCONTENTS_AUX)
            result |= Q2_CONTENTS_AUX;
        if (contents.flags & EWT_VISCONTENTS_LAVA)
            result |= Q2_CONTENTS_LAVA;
        if (contents.flags & EWT_VISCONTENTS_SLIME)
            result |= Q2_CONTENTS_SLIME;
        if (contents.flags & EWT_VISCONTENTS_WATER)
            result |= Q2_CONTENTS_WATER;
        if (contents.flags & EWT_VISCONTENTS_MIST)
            result |= Q2_CONTENTS_MIST;
        if (contents.flags & EWT_INVISCONTENTS_ORIGIN)
            result |= Q2_CONTENTS_ORIGIN;
        if (contents.flags & EWT_INVISCONTENTS_NO_WATERJUMP)
            result |= Q2_CONTENTS_NO_WATERJUMP;
        if (contents.flags & EWT_INVISCONTENTS_PLAYERCLIP)
            result |= Q2_CONTENTS_PLAYERCLIP;
        if (contents.flags & EWT_INVISCONTENTS_MONSTERCLIP)
            result |= Q2_CONTENTS_MONSTERCLIP;
        if (contents.flags & EWT_INVISCONTENTS_PROJECTILECLIP)
            result |= Q2_CONTENTS_PROJECTILECLIP;
        if (contents.flags & EWT_INVISCONTENTS_AREAPORTAL)
            result |= Q2_CONTENTS_AREAPORTAL;
        if (contents.flags & EWT_CFLAG_DETAIL)
            result |= Q2_CONTENTS_DETAIL;

        // cflags
        if (contents.flags & EWT_CFLAG_CURRENT_0)
            result |= Q2_CONTENTS_CURRENT_0;
        if (contents.flags & EWT_CFLAG_CURRENT_90)
            result |= Q2_CONTENTS_CURRENT_90;
        if (contents.flags & EWT_CFLAG_CURRENT_180)
            result |= Q2_CONTENTS_CURRENT_180;
        if (contents.flags & EWT_CFLAG_CURRENT_270)
            result |= Q2_CONTENTS_CURRENT_270;
        if (contents.flags & EWT_CFLAG_CURRENT_UP)
            result |= Q2_CONTENTS_CURRENT_UP;
        if (contents.flags & EWT_CFLAG_CURRENT_DOWN)
            result |= Q2_CONTENTS_CURRENT_DOWN;
        if (contents.flags & EWT_CFLAG_TRANSLUCENT)
            result |= Q2_CONTENTS_TRANSLUCENT;
        if (contents.flags & EWT_CFLAG_LADDER)
            result |= Q2_CONTENTS_LADDER;
        if (contents.flags & EWT_CFLAG_MONSTER)
            result |= Q2_CONTENTS_MONSTER;
        if (contents.flags & EWT_CFLAG_DEADMONSTER)
            result |= Q2_CONTENTS_DEADMONSTER;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_7)
            result |= Q2_CONTENTS_UNUSED_7;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_8)
            result |= Q2_CONTENTS_UNUSED_8;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_9)
            result |= Q2_CONTENTS_UNUSED_9;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_10)
            result |= Q2_CONTENTS_UNUSED_10;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_11)
            result |= Q2_CONTENTS_UNUSED_11;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_12)
            result |= Q2_CONTENTS_UNUSED_12;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_30)
            result |= Q2_CONTENTS_UNUSED_30;
        if (contents.flags & EWT_CFLAG_Q2_UNUSED_31)
            result |= Q2_CONTENTS_UNUSED_31;

        return result;
    }

    inline int32_t get_content_type(contentflags_t contents) const
    {
        return contents.flags & (EWT_ALL_VISIBLE_CONTENTS | EWT_ALL_INVISCONTENTS);
    }

    static constexpr const char *bitflag_names[] = {"SOLID", "WINDOW", "AUX", "LAVA", "SLIME", "WATER", "MIST", "128",
        "256", "512", "1024", "2048", "4096", "8192", "16384", "AREAPORTAL", "PLAYERCLIP", "MONSTERCLIP", "CURRENT_0",
        "CURRENT_90", "CURRENT_180", "CURRENT_270", "CURRENT_UP", "CURRENT_DOWN", "ORIGIN", "MONSTER", "DEADMONSTER",
        "DETAIL", "TRANSLUCENT", "LADDER", "1073741824", "2147483648"};

    int32_t contents_from_string(std::string_view str) const override
    {
        for (size_t i = 0; i < std::size(bitflag_names); i++) {
            if (string_iequals(str, bitflag_names[i])) {
                return nth_bit(i);
            }
        }

        return 0;
    }

    contentflags_t contents_remap_for_export(contentflags_t contents, remap_type_t type) const override
    {
        if (contents.flags & EWT_VISCONTENTS_DETAIL_WALL) {
            contents_int_t result = contents.flags;
            result &= (~EWT_VISCONTENTS_DETAIL_WALL);
            result |= EWT_VISCONTENTS_SOLID;
            return contentflags_t::make(result);
        }

        if (contents.flags & EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER) {
            contents_int_t result = contents.flags;
            result &= (~EWT_VISCONTENTS_ILLUSIONARY_VISBLOCKER);
            result |= EWT_VISCONTENTS_MIST;
            return contentflags_t::make(result);
        }

        return contents;
    }

    std::span<const aabb3d> get_hull_sizes() const override { return {}; }

    contentflags_t face_get_contents(
        const std::string &texname, const surfflags_t &flags, contentflags_t contents, bool transwater) const override
    {
        // hints and skips are never detail, and have no content
        if (flags.native_q2 & (Q2_SURF_HINT | Q2_SURF_SKIP)) {
            return contentflags_t::make(EWT_VISCONTENTS_EMPTY);
        }

        contents_int_t surf_contents = contents.flags;

        // if we don't have a declared content type, assume solid.
        if (!get_content_type(contents)) {
            surf_contents |= EWT_VISCONTENTS_SOLID;
        }

        // if we have TRANS33 or TRANS66 or ALPHATEST, we have to be marked as WINDOW,
        // so unset SOLID, give us WINDOW, and give us TRANSLUCENT
        if (flags.native_q2 & (Q2_SURF_TRANS33 | Q2_SURF_TRANS66 | Q2_SURF_ALPHATEST)) {
            surf_contents |= EWT_CFLAG_TRANSLUCENT;

            if (surf_contents & EWT_VISCONTENTS_SOLID) {
                surf_contents = (surf_contents & ~EWT_VISCONTENTS_SOLID) | EWT_VISCONTENTS_WINDOW;
            }
        }

        // translucent objects are automatically classified as detail
        if (surf_contents & EWT_CFLAG_TRANSLUCENT) {
            surf_contents |= EWT_CFLAG_DETAIL;
        }

        // MIST and AUX are forced to be detail because:
        // - you can see out of AUX if you go inside it, since the inside faces are omitted, so it doesn't make sense
        //   to be visblocking
        // - MIST is typically used for small details that the mapper doesn't want the player to collide with.
        //
        //   If the mapper really wants visblocking mist, we have a separate feature for that,
        //   func_illusionary_visblocker (but this feature is likely to let the player
        //   see into the void when their camera is right on top of the bordering faces, at least in the
        //   Q2 remaster engine, so it's not recommended.)
        if (surf_contents & (EWT_VISCONTENTS_MIST | EWT_VISCONTENTS_AUX)) {
            surf_contents |= EWT_CFLAG_DETAIL;
        }

        if (surf_contents & (EWT_INVISCONTENTS_MONSTERCLIP | EWT_INVISCONTENTS_PLAYERCLIP)) {
            surf_contents |= EWT_CFLAG_DETAIL;
        }

        return contentflags_t::make(surf_contents);
    }

private:
    void discoverArchives(const fs::path &base) const
    {
        fs::directory_iterator it(base);

        std::set<std::string, natural_case_insensitive_less> packs;

        for (auto &entry : it) {
            if (string_iequals(entry.path().extension().generic_string(), ".pak")) {
                packs.insert(entry.path().generic_string());
            }
        }

        for (auto &pak : packs) {
            fs::addArchive(pak);
        }
    }

public:
    inline void addArchive(const fs::path &path) const
    {
        fs::addArchive(path, true);

        if (fs::is_directory(path)) {
            discoverArchives(path);
        }
    }

    void init_filesystem(const fs::path &source, const settings::common_settings &options) const override
    {
        img::clear();
        fs::clear();

        if (options.defaultpaths.value()) {
            constexpr const char *MAPS_FOLDER = "maps";

            // detect gamedir (mod directory path)
            fs::path gamedir, basedir;

            // pull in from settings
            if (options.gamedir.is_changed()) {
                gamedir = options.gamedir.value();
            }
            if (options.basedir.is_changed()) {
                basedir = options.basedir.value();
            }

            // figure out the gamedir first
            if (!gamedir.is_absolute()) {
                if (!gamedir.empty() && basedir.is_absolute()) {
                    // we passed in a relative gamedir. probably meant to
                    // be derived from basedir.
                    gamedir = basedir.parent_path() / gamedir;
                }

                // no gamedir, so calculate it from the input
                if (gamedir.empty()) {
                    // expand canonicals, and fetch parent of source file
                    if (auto paths = fs::splitArchivePath(source)) {
                        // if the source is an archive, use the parent
                        // of that folder as the mod directory
                        // pak0.pak/maps/source.map -> C:/Quake/ID1
                        gamedir = fs::canonical(paths.archive).parent_path();
                    } else {
                        // maps/*/source.map -> C:/Quake/ID1/maps
                        // this is weak because the source may not exist yet
                        bool found_maps_folder = false;
                        fs::path olddir = gamedir = source;

                        // NOTE: parent_path() of C:/ is C:/ and this is considered non-empty
                        // its relative_path() (the part after the drive letter) is empty, though
                        while (!gamedir.relative_path().empty()) {
                            gamedir = fs::weakly_canonical(gamedir).parent_path();

                            if (string_iequals(gamedir.filename().generic_string(), MAPS_FOLDER)) {
                                found_maps_folder = true;
                                break;
                            }
                        }

                        if (!found_maps_folder) {
                            logging::print(
                                "WARNING: '{}' is not a child of '{}'; gamedir can't be automatically determined.\n",
                                source, MAPS_FOLDER);

                            gamedir = olddir;
                        }

                        // C:/Quake/ID1/maps -> C:/Quake/ID1
                        gamedir = gamedir.parent_path();
                    }
                }
            }

            if (!exists(gamedir)) {
                logging::print("WARNING: failed to find gamedir '{}'\n", gamedir);
            } else {
                logging::print("using gamedir: '{}'\n", gamedir);
            }

            // now find base dir, if we haven't set it yet
            if (!basedir.is_absolute()) {
                if (!basedir.empty() && gamedir.is_absolute()) {
                    // we passed in a relative basedir. probably meant to
                    // be derived from gamedir.
                    basedir = gamedir.parent_path() / basedir;
                }

                // no basedir, so calculate it from gamedir
                if (basedir.empty()) {
                    basedir = gamedir.parent_path() / default_base_dir;
                }
            }

            std::error_code ec;
            if (!exists(basedir)) {
                logging::print("WARNING: failed to find basedir '{}'\n", basedir);
            } else if (!equivalent(gamedir, basedir, ec)) {
                addArchive(basedir);
                logging::print("using basedir: '{}'\n", basedir);
            }

            if (exists(gamedir)) {
                addArchive(gamedir);
            }
        }

        // add secondary paths
        for (auto &path : options.paths.values()) {
            addArchive(path);
        }

        // load palette
        img::init_palette(this, options.filepriority.value() == settings::search_priority_t::LOOSE);
    }

    const std::vector<qvec3b> &get_default_palette() const override
    {
        static constexpr std::initializer_list<uint8_t> palette_bytes{0, 0, 0, 15, 15, 15, 31, 31, 31, 47, 47, 47, 63,
            63, 63, 75, 75, 75, 91, 91, 91, 107, 107, 107, 123, 123, 123, 139, 139, 139, 155, 155, 155, 171, 171, 171,
            187, 187, 187, 203, 203, 203, 219, 219, 219, 235, 235, 235, 99, 75, 35, 91, 67, 31, 83, 63, 31, 79, 59, 27,
            71, 55, 27, 63, 47, 23, 59, 43, 23, 51, 39, 19, 47, 35, 19, 43, 31, 19, 39, 27, 15, 35, 23, 15, 27, 19, 11,
            23, 15, 11, 19, 15, 7, 15, 11, 7, 95, 95, 111, 91, 91, 103, 91, 83, 95, 87, 79, 91, 83, 75, 83, 79, 71, 75,
            71, 63, 67, 63, 59, 59, 59, 55, 55, 51, 47, 47, 47, 43, 43, 39, 39, 39, 35, 35, 35, 27, 27, 27, 23, 23, 23,
            19, 19, 19, 143, 119, 83, 123, 99, 67, 115, 91, 59, 103, 79, 47, 207, 151, 75, 167, 123, 59, 139, 103, 47,
            111, 83, 39, 235, 159, 39, 203, 139, 35, 175, 119, 31, 147, 99, 27, 119, 79, 23, 91, 59, 15, 63, 39, 11, 35,
            23, 7, 167, 59, 43, 159, 47, 35, 151, 43, 27, 139, 39, 19, 127, 31, 15, 115, 23, 11, 103, 23, 7, 87, 19, 0,
            75, 15, 0, 67, 15, 0, 59, 15, 0, 51, 11, 0, 43, 11, 0, 35, 11, 0, 27, 7, 0, 19, 7, 0, 123, 95, 75, 115, 87,
            67, 107, 83, 63, 103, 79, 59, 95, 71, 55, 87, 67, 51, 83, 63, 47, 75, 55, 43, 67, 51, 39, 63, 47, 35, 55,
            39, 27, 47, 35, 23, 39, 27, 19, 31, 23, 15, 23, 15, 11, 15, 11, 7, 111, 59, 23, 95, 55, 23, 83, 47, 23, 67,
            43, 23, 55, 35, 19, 39, 27, 15, 27, 19, 11, 15, 11, 7, 179, 91, 79, 191, 123, 111, 203, 155, 147, 215, 187,
            183, 203, 215, 223, 179, 199, 211, 159, 183, 195, 135, 167, 183, 115, 151, 167, 91, 135, 155, 71, 119, 139,
            47, 103, 127, 23, 83, 111, 19, 75, 103, 15, 67, 91, 11, 63, 83, 7, 55, 75, 7, 47, 63, 7, 39, 51, 0, 31, 43,
            0, 23, 31, 0, 15, 19, 0, 7, 11, 0, 0, 0, 139, 87, 87, 131, 79, 79, 123, 71, 71, 115, 67, 67, 107, 59, 59,
            99, 51, 51, 91, 47, 47, 87, 43, 43, 75, 35, 35, 63, 31, 31, 51, 27, 27, 43, 19, 19, 31, 15, 15, 19, 11, 11,
            11, 7, 7, 0, 0, 0, 151, 159, 123, 143, 151, 115, 135, 139, 107, 127, 131, 99, 119, 123, 95, 115, 115, 87,
            107, 107, 79, 99, 99, 71, 91, 91, 67, 79, 79, 59, 67, 67, 51, 55, 55, 43, 47, 47, 35, 35, 35, 27, 23, 23,
            19, 15, 15, 11, 159, 75, 63, 147, 67, 55, 139, 59, 47, 127, 55, 39, 119, 47, 35, 107, 43, 27, 99, 35, 23,
            87, 31, 19, 79, 27, 15, 67, 23, 11, 55, 19, 11, 43, 15, 7, 31, 11, 7, 23, 7, 0, 11, 0, 0, 0, 0, 0, 119, 123,
            207, 111, 115, 195, 103, 107, 183, 99, 99, 167, 91, 91, 155, 83, 87, 143, 75, 79, 127, 71, 71, 115, 63, 63,
            103, 55, 55, 87, 47, 47, 75, 39, 39, 63, 35, 31, 47, 27, 23, 35, 19, 15, 23, 11, 7, 7, 155, 171, 123, 143,
            159, 111, 135, 151, 99, 123, 139, 87, 115, 131, 75, 103, 119, 67, 95, 111, 59, 87, 103, 51, 75, 91, 39, 63,
            79, 27, 55, 67, 19, 47, 59, 11, 35, 47, 7, 27, 35, 0, 19, 23, 0, 11, 15, 0, 0, 255, 0, 35, 231, 15, 63, 211,
            27, 83, 187, 39, 95, 167, 47, 95, 143, 51, 95, 123, 51, 255, 255, 255, 255, 255, 211, 255, 255, 167, 255,
            255, 127, 255, 255, 83, 255, 255, 39, 255, 235, 31, 255, 215, 23, 255, 191, 15, 255, 171, 7, 255, 147, 0,
            239, 127, 0, 227, 107, 0, 211, 87, 0, 199, 71, 0, 183, 59, 0, 171, 43, 0, 155, 31, 0, 143, 23, 0, 127, 15,
            0, 115, 7, 0, 95, 0, 0, 71, 0, 0, 47, 0, 0, 27, 0, 0, 239, 0, 0, 55, 55, 255, 255, 0, 0, 0, 0, 255, 43, 43,
            35, 27, 27, 23, 19, 19, 15, 235, 151, 127, 195, 115, 83, 159, 87, 51, 123, 63, 27, 235, 211, 199, 199, 171,
            155, 167, 139, 119, 135, 107, 87, 159, 91, 83};
        static const auto palette = make_palette(palette_bytes);
        return palette;
    }
};

// Game definitions, used for the bsp versions below
static const gamedef_q1_like_t<GAME_QUAKE> gamedef_q1;
static const gamedef_h2_t gamedef_h2;
static const gamedef_hl_t gamedef_hl;
static const gamedef_q2_t gamedef_q2;

const std::initializer_list<const gamedef_t *> &gamedef_list()
{
    static constexpr std::initializer_list<const gamedef_t *> gamedefs{
        &gamedef_q1, &gamedef_h2, &gamedef_hl, &gamedef_q2};
    return gamedefs;
}

const bspversion_t bspver_generic{MBSPIDENT, std::nullopt, "mbsp", "generic BSP", {}};
const bspversion_t bspver_q1{BSPVERSION, std::nullopt, "bsp29", "Quake BSP",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp29_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp29_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp29_dclipnode_t)},
        {"leafs", sizeof(bsp29_dleaf_t)},
        {"marksurfaces", sizeof(uint16_t)},
        {"edges", sizeof(bsp29_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelq1_t)},
    },
    &gamedef_q1, &bspver_bsp2};
const bspversion_t bspver_bsp2{BSP2VERSION, std::nullopt, "bsp2", "Quake BSP2",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp2_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp2_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp2_dclipnode_t)},
        {"leafs", sizeof(bsp2_dleaf_t)},
        {"marksurfaces", sizeof(uint32_t)},
        {"edges", sizeof(bsp2_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelq1_t)},
    },
    &gamedef_q1};
const bspversion_t bspver_bsp2rmq{BSP2RMQVERSION, std::nullopt, "bsp2rmq", "Quake BSP2-RMQ",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp2rmq_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp2_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp2_dclipnode_t)},
        {"leafs", sizeof(bsp2rmq_dleaf_t)},
        {"marksurfaces", sizeof(uint32_t)},
        {"edges", sizeof(bsp2_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelq1_t)},
    },
    &gamedef_q1};
/* Hexen II doesn't use a separate version, but we can still use a separate tag/name for it */
const bspversion_t bspver_h2{BSPVERSION, std::nullopt, "hexen2", "Hexen II BSP",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp29_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp29_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp29_dclipnode_t)},
        {"leafs", sizeof(bsp29_dleaf_t)},
        {"marksurfaces", sizeof(uint16_t)},
        {"edges", sizeof(bsp29_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelh2_t)},
    },
    &gamedef_h2, &bspver_h2bsp2};
const bspversion_t bspver_h2bsp2{BSP2VERSION, std::nullopt, "hexen2bsp2", "Hexen II BSP2",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp2_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp2_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp2_dclipnode_t)},
        {"leafs", sizeof(bsp2_dleaf_t)},
        {"marksurfaces", sizeof(uint32_t)},
        {"edges", sizeof(bsp2_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelh2_t)},
    },
    &gamedef_h2};
const bspversion_t bspver_h2bsp2rmq{BSP2RMQVERSION, std::nullopt, "hexen2bsp2rmq", "Hexen II BSP2-RMQ",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"texture", sizeof(uint8_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(bsp2rmq_dnode_t)},
        {"texinfos", sizeof(texinfo_t)},
        {"faces", sizeof(bsp2_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"clipnodes", sizeof(bsp2_dclipnode_t)},
        {"leafs", sizeof(bsp2rmq_dleaf_t)},
        {"marksurfaces", sizeof(uint32_t)},
        {"edges", sizeof(bsp2_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(dmodelh2_t)},
    },
    &gamedef_h2};
const bspversion_t bspver_hl{BSPHLVERSION, std::nullopt, "hl", "Half-Life BSP", bspver_q1.lumps, &gamedef_hl};
const bspversion_t bspver_q2{Q2_BSPIDENT, Q2_BSPVERSION, "q2bsp", "Quake II BSP",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(q2_dnode_t)},
        {"texinfos", sizeof(q2_texinfo_t)},
        {"faces", sizeof(q2_dface_t)},
        {"lighting", sizeof(uint8_t)},
        {"leafs", sizeof(q2_dleaf_t)},
        {"leaffaces", sizeof(uint16_t)},
        {"leafbrushes", sizeof(uint16_t)},
        {"edges", sizeof(bsp29_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(q2_dmodel_t)},
        {"brushes", sizeof(dbrush_t)},
        {"brushsides", sizeof(q2_dbrushside_t)},
        {"pop", sizeof(uint8_t)},
        {"areas", sizeof(darea_t)},
        {"areaportals", sizeof(dareaportal_t)},
    },
    &gamedef_q2, &bspver_qbism};
const bspversion_t bspver_qbism{Q2_QBISMIDENT, Q2_BSPVERSION, "qbism", "Quake II Qbism BSP",
    {
        {"entities", sizeof(char)},
        {"planes", sizeof(dplane_t)},
        {"vertexes", sizeof(qvec3f)},
        {"visibility", sizeof(uint8_t)},
        {"nodes", sizeof(q2_dnode_qbism_t)},
        {"texinfos", sizeof(q2_texinfo_t)},
        {"faces", sizeof(q2_dface_qbism_t)},
        {"lighting", sizeof(uint8_t)},
        {"leafs", sizeof(q2_dleaf_qbism_t)},
        {"leaffaces", sizeof(uint32_t)},
        {"leafbrushes", sizeof(uint32_t)},
        {"edges", sizeof(bsp2_dedge_t)},
        {"surfedges", sizeof(int32_t)},
        {"models", sizeof(q2_dmodel_t)},
        {"brushes", sizeof(dbrush_t)},
        {"brushsides", sizeof(q2_dbrushside_qbism_t)},
        {"pop", sizeof(uint8_t)},
        {"areas", sizeof(darea_t)},
        {"areaportals", sizeof(dareaportal_t)},
    },
    &gamedef_q2};

static bool BSPVersionSupported(int32_t ident, std::optional<int32_t> version, const bspversion_t **out_version)
{
    for (const bspversion_t *bspver : bspversions) {
        if (bspver->ident == ident && bspver->version == version) {
            if (bspver->game->id == GAME_HEXEN_II) {
                // HACK: don't detect as Hexen II here, it's done later (isHexen2).
                // Since the Hexen II bspversion_t's have the same ident/version as Quake
                // we need to assume Quake.
                continue;
            }
            *out_version = bspver;
            return true;
        }
    }

    return false;
}

/*
 * =========================================================================
 * BSP Format Conversion
 * =========================================================================
 */

// move structured data if the input and output
// are of the same type
template<typename T>
inline void CopyArray(T &in, T &out)
{
    out = in;
}

// convert structured data if we're different types
template<typename T, typename F, typename = std::enable_if_t<!std::is_same_v<T, F>>>
inline void CopyArray(std::vector<F> &from, std::vector<T> &to)
{
    to.reserve(from.size());

    for (auto &v : from) {
        if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<F>)
            to.push_back(numeric_cast<T>(v));
        else
            to.push_back(static_cast<T>(v));
    }
}

// move structured data if the input and output
// are of the same type
template<typename T, typename F>
inline void CopyArray(F &in, T &out)
{
    out = in;
}

// convert structured data if we're different types
// with numeric casting for arrays
template<typename T, typename F, size_t N, typename = std::enable_if_t<!std::is_same_v<T, F>>>
inline void CopyArray(std::vector<std::array<F, N>> &from, std::vector<std::array<T, N>> &to)
{
    to.reserve(from.size());

    for (auto &v : from) {
        if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<F>)
            to.push_back(array_cast<std::array<T, N>>(v));
        else
            to.push_back(v);
    }
}

// Convert from a Q1-esque format to Generic
template<typename T>
inline void ConvertQ1BSPToGeneric(T &bsp, mbsp_t &mbsp)
{
    CopyArray(bsp.dentdata, mbsp.dentdata);
    CopyArray(bsp.dplanes, mbsp.dplanes);
    CopyArray(bsp.dtex, mbsp.dtex);
    CopyArray(bsp.dvertexes, mbsp.dvertexes);
    CopyArray(bsp.dvisdata, mbsp.dvis.bits);
    CopyArray(bsp.dnodes, mbsp.dnodes);
    CopyArray(bsp.texinfo, mbsp.texinfo);
    CopyArray(bsp.dfaces, mbsp.dfaces);
    CopyArray(bsp.dlightdata, mbsp.dlightdata);
    CopyArray(bsp.dclipnodes, mbsp.dclipnodes);
    CopyArray(bsp.dleafs, mbsp.dleafs);
    CopyArray(bsp.dmarksurfaces, mbsp.dleaffaces);
    CopyArray(bsp.dedges, mbsp.dedges);
    CopyArray(bsp.dsurfedges, mbsp.dsurfedges);
    if (std::holds_alternative<dmodelh2_vector>(bsp.dmodels)) {
        CopyArray(std::get<dmodelh2_vector>(bsp.dmodels), mbsp.dmodels);
    } else {
        CopyArray(std::get<dmodelq1_vector>(bsp.dmodels), mbsp.dmodels);
    }
}

// Convert from a Q2-esque format to Generic
template<typename T>
inline void ConvertQ2BSPToGeneric(T &bsp, mbsp_t &mbsp)
{
    CopyArray(bsp.dentdata, mbsp.dentdata);
    CopyArray(bsp.dplanes, mbsp.dplanes);
    CopyArray(bsp.dvertexes, mbsp.dvertexes);
    CopyArray(bsp.dvis, mbsp.dvis);
    CopyArray(bsp.dnodes, mbsp.dnodes);
    CopyArray(bsp.texinfo, mbsp.texinfo);
    CopyArray(bsp.dfaces, mbsp.dfaces);
    CopyArray(bsp.dlightdata, mbsp.dlightdata);
    CopyArray(bsp.dleafs, mbsp.dleafs);
    CopyArray(bsp.dleaffaces, mbsp.dleaffaces);
    CopyArray(bsp.dleafbrushes, mbsp.dleafbrushes);
    CopyArray(bsp.dedges, mbsp.dedges);
    CopyArray(bsp.dsurfedges, mbsp.dsurfedges);
    CopyArray(bsp.dmodels, mbsp.dmodels);
    CopyArray(bsp.dbrushes, mbsp.dbrushes);
    CopyArray(bsp.dbrushsides, mbsp.dbrushsides);
    CopyArray(bsp.dareas, mbsp.dareas);
    CopyArray(bsp.dareaportals, mbsp.dareaportals);
}

// Convert from a Q1-esque format to Generic
template<typename T>
inline T ConvertGenericToQ1BSP(mbsp_t &mbsp, const bspversion_t *to_version)
{
    T bsp{};

    // copy or convert data
    CopyArray(mbsp.dentdata, bsp.dentdata);
    CopyArray(mbsp.dplanes, bsp.dplanes);
    CopyArray(mbsp.dtex, bsp.dtex);
    CopyArray(mbsp.dvertexes, bsp.dvertexes);
    CopyArray(mbsp.dvis.bits, bsp.dvisdata);
    CopyArray(mbsp.dnodes, bsp.dnodes);
    CopyArray(mbsp.texinfo, bsp.texinfo);
    CopyArray(mbsp.dfaces, bsp.dfaces);
    CopyArray(mbsp.dlightdata, bsp.dlightdata);
    CopyArray(mbsp.dclipnodes, bsp.dclipnodes);
    CopyArray(mbsp.dleafs, bsp.dleafs);
    CopyArray(mbsp.dleaffaces, bsp.dmarksurfaces);
    CopyArray(mbsp.dedges, bsp.dedges);
    CopyArray(mbsp.dsurfedges, bsp.dsurfedges);
    if (to_version->game->id == GAME_HEXEN_II) {
        CopyArray(mbsp.dmodels, bsp.dmodels.template emplace<dmodelh2_vector>());
    } else {
        CopyArray(mbsp.dmodels, bsp.dmodels.template emplace<dmodelq1_vector>());
    }

    return bsp;
}

// Convert from a Q2-esque format to Generic
template<typename T>
inline T ConvertGenericToQ2BSP(mbsp_t &mbsp, const bspversion_t *to_version)
{
    T bsp{};

    // copy or convert data
    CopyArray(mbsp.dentdata, bsp.dentdata);
    CopyArray(mbsp.dplanes, bsp.dplanes);
    CopyArray(mbsp.dvertexes, bsp.dvertexes);
    CopyArray(mbsp.dvis, bsp.dvis);
    CopyArray(mbsp.dnodes, bsp.dnodes);
    CopyArray(mbsp.texinfo, bsp.texinfo);
    CopyArray(mbsp.dfaces, bsp.dfaces);
    CopyArray(mbsp.dlightdata, bsp.dlightdata);
    CopyArray(mbsp.dleafs, bsp.dleafs);
    CopyArray(mbsp.dleaffaces, bsp.dleaffaces);
    CopyArray(mbsp.dleafbrushes, bsp.dleafbrushes);
    CopyArray(mbsp.dedges, bsp.dedges);
    CopyArray(mbsp.dsurfedges, bsp.dsurfedges);
    CopyArray(mbsp.dmodels, bsp.dmodels);
    CopyArray(mbsp.dbrushes, bsp.dbrushes);
    CopyArray(mbsp.dbrushsides, bsp.dbrushsides);
    CopyArray(mbsp.dareas, bsp.dareas);
    CopyArray(mbsp.dareaportals, bsp.dareaportals);

    return bsp;
}

/*
 * =========================================================================
 * ConvertBSPFormat
 * - BSP is assumed to be in CPU byte order already
 * - No checks are done here (yet) for overflow of values when down-converting
 * =========================================================================
 */
bool ConvertBSPFormat(bspdata_t *bspdata, const bspversion_t *to_version)
{
    if (!bspdata || !bspdata->version || !to_version) {
        return false;
    }

    if (bspdata->version == to_version)
        return true;

    if (to_version == &bspver_generic) {
        // Conversions to bspver_generic
        mbsp_t mbsp{};

        mbsp.file = bspdata->file;

        if (std::holds_alternative<bsp29_t>(bspdata->bsp)) {
            ConvertQ1BSPToGeneric(std::get<bsp29_t>(bspdata->bsp), mbsp);
        } else if (std::holds_alternative<q2bsp_t>(bspdata->bsp)) {
            ConvertQ2BSPToGeneric(std::get<q2bsp_t>(bspdata->bsp), mbsp);
        } else if (std::holds_alternative<q2bsp_qbism_t>(bspdata->bsp)) {
            ConvertQ2BSPToGeneric(std::get<q2bsp_qbism_t>(bspdata->bsp), mbsp);
        } else if (std::holds_alternative<bsp2rmq_t>(bspdata->bsp)) {
            ConvertQ1BSPToGeneric(std::get<bsp2rmq_t>(bspdata->bsp), mbsp);
        } else if (std::holds_alternative<bsp2_t>(bspdata->bsp)) {
            ConvertQ1BSPToGeneric(std::get<bsp2_t>(bspdata->bsp), mbsp);
        } else {
            return false;
        }

        bspdata->loadversion = mbsp.loadversion = bspdata->version;
        bspdata->version = to_version;

        bspdata->bsp = std::move(mbsp);
        return true;
    } else if (bspdata->version == &bspver_generic) {
        // Conversions from bspver_generic
        mbsp_t &mbsp = std::get<mbsp_t>(bspdata->bsp);

        try {
            if (to_version == &bspver_q1 || to_version == &bspver_h2 || to_version == &bspver_hl) {
                bspdata->bsp = ConvertGenericToQ1BSP<bsp29_t>(mbsp, to_version);
            } else if (to_version == &bspver_q2) {
                bspdata->bsp = ConvertGenericToQ2BSP<q2bsp_t>(mbsp, to_version);
            } else if (to_version == &bspver_qbism) {
                bspdata->bsp = ConvertGenericToQ2BSP<q2bsp_qbism_t>(mbsp, to_version);
            } else if (to_version == &bspver_bsp2rmq || to_version == &bspver_h2bsp2rmq) {
                bspdata->bsp = ConvertGenericToQ1BSP<bsp2rmq_t>(mbsp, to_version);
            } else if (to_version == &bspver_bsp2 || to_version == &bspver_h2bsp2) {
                bspdata->bsp = ConvertGenericToQ1BSP<bsp2_t>(mbsp, to_version);
            } else {
                return false;
            }
        } catch (const std::overflow_error &e) {
            logging::print("LIMITS EXCEEDED ON {}\n", e.what());
            return false;
        }

        bspdata->version = to_version;
        return true;
    }

    return false;
}

static bool isHexen2(
    const std::vector<uint8_t> &file_data, const std::vector<lump_t> &lumps, const bspversion_t *bspversion)
{
    const lump_t &models_lump = lumps[LUMP_MODELS];

    // With no models there is no Hexen II-only structure to distinguish.
    // Preserve the declared Quake format instead of guessing.
    if (models_lump.filelen == 0) {
        return false;
    }

    if (0 != (models_lump.filelen % sizeof(dmodelh2_t))) {
        // definitely not H2
        return false;
    }
    if (0 != (models_lump.filelen % sizeof(dmodelq1_t))) {
        // definitely not Q1
        return true;
    }

    // models lump is divisible by both the Q1 model size (64 bytes) and the H2 model size (80 bytes).

    const int bytes_per_face = bspversion->lumps.begin()[LUMP_FACES].size;
    const int bytes_per_node = bspversion->lumps.begin()[LUMP_NODES].size;
    const int bytes_per_leaf = bspversion->lumps.begin()[LUMP_LEAFS].size;
    const int bytes_per_clipnode = bspversion->lumps.begin()[LUMP_CLIPNODES].size;

    const int faces_count = lumps[LUMP_FACES].filelen / bytes_per_face;
    const int nodes_count = lumps[LUMP_NODES].filelen / bytes_per_node;
    const int leafs_count = lumps[LUMP_LEAFS].filelen / bytes_per_leaf;
    const int clipnodes_count = lumps[LUMP_CLIPNODES].filelen / bytes_per_clipnode;

    // assume H2, and do some basic validation
    const int models_h2_count = models_lump.filelen / sizeof(dmodelh2_t);
    imemstream models_stream(file_data.data() + models_lump.fileofs, models_lump.filelen);
    models_stream >> endianness<std::endian::little>;

    for (int i = 0; i < models_h2_count; ++i) {
        dmodelh2_t model;
        models_stream >= model;
        if (!models_stream) {
            return false;
        }

        // visleafs
        if (model.visleafs < 0 || model.visleafs > leafs_count)
            return false;

        // numfaces, firstface
        if (model.numfaces < 0)
            return false;
        if (model.firstface < 0 || model.firstface > faces_count)
            return false;
        if (model.numfaces > faces_count - model.firstface)
            return false;

        // headnode[0]
        if (model.headnode[0] >= nodes_count)
            return false;

        // clipnode headnodes
        for (int j = 1; j < 8; ++j) {
            if (model.headnode[j] >= clipnodes_count)
                return false;
        }
    }

    // passed all checks, assume H2
    return true;
}

struct lump_reader
{
    std::istream &s;
    const bspversion_t *version;
    const std::vector<lump_t> &lumps;

    // read structured lump data from stream into vector
    template<typename T>
    void read(size_t lump_num, std::vector<T> &buffer)
    {
        Q_assert(version->lumps.size() > lump_num);
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        const lump_t &lump = lumps[lump_num];
        size_t length;

        Q_assert(!buffer.size());

        if (lumpspec.size > 1) {
            if (sizeof(T) != lumpspec.size)
                FError("odd {} value size ({} != {})", lumpspec.name, sizeof(T), lumpspec.size);
            else if (lump.filelen % lumpspec.size)
                FError("odd {} lump size ({} not multiple of {})", lumpspec.name, lump.filelen, lumpspec.size);

            buffer.reserve(length = (lump.filelen / lumpspec.size));
        } else {
            buffer.resize(length = lump.filelen);
        }

        if (!lump.filelen)
            return;

        s.seekg(lump.fileofs);
        if (!s) {
            FError("unable to seek to {} lump at offset {}", lumpspec.name, lump.fileofs);
        }

        if (lumpspec.size > 1) {
            for (size_t i = 0; i < length; i++) {
                T &val = buffer.emplace_back();
                s >= val;
                if (!s) {
                    FError("{} lump is truncated at entry {} of {}", lumpspec.name, i, length);
                }
            }
        } else {
            const auto requested = static_cast<std::streamsize>(length);
            s.read(reinterpret_cast<char *>(buffer.data()), requested);
            if (!s || s.gcount() != requested) {
                FError("{} lump is truncated (expected {} bytes)", lumpspec.name, length);
            }
        }
    }

    // read string from stream
    void read(size_t lump_num, std::string &buffer)
    {
        Q_assert(version->lumps.size() > lump_num);
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        const lump_t &lump = lumps[lump_num];

        Q_assert(lumpspec.size == 1);
        Q_assert(!buffer.size());

        buffer.resize(lump.filelen);

        if (!lump.filelen)
            return;

        s.seekg(lump.fileofs);
        if (!s) {
            FError("unable to seek to {} lump at offset {}", lumpspec.name, lump.fileofs);
        }

        const auto requested = static_cast<std::streamsize>(lump.filelen);
        s.read(reinterpret_cast<char *>(buffer.data()), requested);
        if (!s || s.gcount() != requested) {
            FError("{} lump is truncated (expected {} bytes)", lumpspec.name, lump.filelen);
        }

        // the last byte is required to be '\0' which was added when the .bsp was
        // written. chop it off now, since we want the std::string to
        // be the logical string (we'll add the null terminator again when saving the .bsp)
        if (buffer[lump.filelen - 1] == 0) {
            buffer.resize(lump.filelen - 1);
        }
        // TODO: warn about bad .bsp if missing \0?
    }

    // read structured lump data from stream into struct
    template<typename T, typename = std::enable_if_t<std::is_member_function_pointer_v<decltype(&T::stream_read)>>>
    void read(size_t lump_num, T &buffer)
    {
        Q_assert(version->lumps.size() > lump_num);
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        const lump_t &lump = lumps[lump_num];

        if (!lump.filelen)
            return;

        Q_assert(lumpspec.size == 1);

        s.seekg(lump.fileofs);
        if (!s) {
            FError("unable to seek to {} lump at offset {}", lumpspec.name, lump.fileofs);
        }

        buffer.stream_read(s, lump);
        if (!s) {
            FError("{} lump is malformed or truncated", lumpspec.name);
        }
    }
};

template<typename T>
inline void ReadQ1BSP(lump_reader &reader, T &bsp)
{
    reader.read(LUMP_ENTITIES, bsp.dentdata);
    reader.read(LUMP_PLANES, bsp.dplanes);
    reader.read(LUMP_TEXTURES, bsp.dtex);
    reader.read(LUMP_VERTEXES, bsp.dvertexes);
    reader.read(LUMP_VISIBILITY, bsp.dvisdata);
    reader.read(LUMP_NODES, bsp.dnodes);
    reader.read(LUMP_TEXINFO, bsp.texinfo);
    reader.read(LUMP_FACES, bsp.dfaces);
    reader.read(LUMP_LIGHTING, bsp.dlightdata);
    reader.read(LUMP_CLIPNODES, bsp.dclipnodes);
    reader.read(LUMP_LEAFS, bsp.dleafs);
    reader.read(LUMP_MARKSURFACES, bsp.dmarksurfaces);
    reader.read(LUMP_EDGES, bsp.dedges);
    reader.read(LUMP_SURFEDGES, bsp.dsurfedges);
    if (reader.version->game->id == GAME_HEXEN_II) {
        reader.read(LUMP_MODELS, bsp.dmodels.template emplace<dmodelh2_vector>());
    } else {
        reader.read(LUMP_MODELS, bsp.dmodels.template emplace<dmodelq1_vector>());
    }
}

template<typename T>
inline void ReadQ2BSP(lump_reader &reader, T &bsp)
{
    reader.read(Q2_LUMP_ENTITIES, bsp.dentdata);
    reader.read(Q2_LUMP_PLANES, bsp.dplanes);
    reader.read(Q2_LUMP_VERTEXES, bsp.dvertexes);
    reader.read(Q2_LUMP_VISIBILITY, bsp.dvis);
    reader.read(Q2_LUMP_NODES, bsp.dnodes);
    reader.read(Q2_LUMP_TEXINFO, bsp.texinfo);
    reader.read(Q2_LUMP_FACES, bsp.dfaces);
    reader.read(Q2_LUMP_LIGHTING, bsp.dlightdata);
    reader.read(Q2_LUMP_LEAFS, bsp.dleafs);
    reader.read(Q2_LUMP_LEAFFACES, bsp.dleaffaces);
    reader.read(Q2_LUMP_LEAFBRUSHES, bsp.dleafbrushes);
    reader.read(Q2_LUMP_EDGES, bsp.dedges);
    reader.read(Q2_LUMP_SURFEDGES, bsp.dsurfedges);
    reader.read(Q2_LUMP_MODELS, bsp.dmodels);
    reader.read(Q2_LUMP_BRUSHES, bsp.dbrushes);
    reader.read(Q2_LUMP_BRUSHSIDES, bsp.dbrushsides);
    reader.read(Q2_LUMP_AREAS, bsp.dareas);
    reader.read(Q2_LUMP_AREAPORTALS, bsp.dareaportals);
}

void bspdata_t::bspxentries::transfer(std::string_view xname, std::vector<uint8_t> &xdata)
{
    entries.insert_or_assign(std::string(xname), std::move(xdata));
}

void bspdata_t::bspxentries::transfer(std::string_view xname, std::vector<uint8_t> &&xdata)
{
    entries.insert_or_assign(std::string(xname), std::move(xdata));
}

static bool byte_range_within_file(size_t offset, size_t length, size_t file_size)
{
    return offset <= file_size && length <= file_size - offset;
}

static void validate_bsp_lumps(const std::vector<lump_t> &lumps, const bspversion_t *version, size_t file_size)
{
    if (lumps.size() != version->lumps.size()) {
        FError("header contains {} lumps, expected {}", lumps.size(), version->lumps.size());
    }

    for (size_t i = 0; i < lumps.size(); ++i) {
        const lump_t &lump = lumps[i];
        const lumpspec_t &spec = version->lumps.begin()[i];

        if (lump.fileofs < 0 || lump.filelen < 0) {
            FError("invalid {} lump range: offset {}, length {}", spec.name, lump.fileofs, lump.filelen);
        }

        const auto offset = static_cast<size_t>(lump.fileofs);
        const auto length = static_cast<size_t>(lump.filelen);
        if (!byte_range_within_file(offset, length, file_size)) {
            FError("{} lump range (offset {}, length {}) exceeds {} byte file", spec.name, offset, length, file_size);
        }
    }
}

/*
 * =============
 * LoadBSPFile
 * =============
 */
void LoadBSPFile(fs::path &filename, bspdata_t *bspdata)
{
    if (!bspdata) {
        FError("LoadBSPFile requires a valid output object");
    }

    int i;

    logging::funcprint("'{}'\n", filename);

    const fs::path requested_filename = filename;

    /* load the file header */
    fs::data file_data = fs::load(filename);

    if (!file_data) {
        FError("Unable to load \"{}\"\n", filename);
    }

    constexpr size_t q1_header_size = sizeof(int32_t) * (1 + (BSP_LUMPS * 2));
    constexpr size_t q2_header_size = sizeof(int32_t) * (2 + (Q2_HEADER_LUMPS * 2));

    if (file_data->size() < sizeof(int32_t)) {
        FError("BSP file is too small to contain a version identifier ({} bytes)", file_data->size());
    }

    filename = fs::resolveArchivePath(filename);

    imemstream stream(file_data->data(), file_data->size());

    stream >> endianness<std::endian::little>;

    /* transfer the header data to this */
    std::vector<lump_t> lumps;

    /* check for IBSP */
    bspversion_t temp_version{};
    stream >= temp_version.ident;
    if (!stream) {
        FError("unable to read BSP version identifier");
    }
    stream.seekg(0);

    if (temp_version.ident == Q2_BSPIDENT || temp_version.ident == Q2_QBISMIDENT) {
        if (file_data->size() < q2_header_size) {
            FError("truncated Quake II BSP header ({} bytes, expected at least {})", file_data->size(), q2_header_size);
        }

        q2_dheader_t q2header;
        stream >= q2header;

        if (!stream) {
            FError("unable to read Quake II BSP header");
        }

        temp_version.version = q2header.version;
        std::copy(q2header.lumps.begin(), q2header.lumps.end(), std::back_inserter(lumps));
    } else {
        if (file_data->size() < q1_header_size) {
            FError("truncated Quake BSP header ({} bytes, expected at least {})", file_data->size(), q1_header_size);
        }

        dheader_t q1header;
        stream >= q1header;

        if (!stream) {
            FError("unable to read Quake BSP header");
        }

        temp_version.version = std::nullopt;
        std::copy(q1header.lumps.begin(), q1header.lumps.end(), std::back_inserter(lumps));
    }

    /* check the file version */
    const bspversion_t *detected_version = nullptr;
    if (!BSPVersionSupported(temp_version.ident, temp_version.version, &detected_version)) {
        logging::print("BSP is version {}\n", temp_version);
        Error("Sorry, this bsp version is not supported.");
    } else {
        validate_bsp_lumps(lumps, detected_version, file_data->size());

        // special case handling for Hexen II
        if (detected_version->game->id == GAME_QUAKE && isHexen2(*file_data, lumps, detected_version)) {
            if (detected_version == &bspver_q1) {
                detected_version = &bspver_h2;
            } else if (detected_version == &bspver_bsp2) {
                detected_version = &bspver_h2bsp2;
            } else if (detected_version == &bspver_bsp2rmq) {
                detected_version = &bspver_h2bsp2rmq;
            }
        }

        logging::print("BSP is version {}\n", *detected_version);
    }

    lump_reader reader{stream, detected_version, lumps};
    decltype(bspdata->bsp) parsed_bsp;

    /* copy the data */
    if (detected_version == &bspver_q2) {
        ReadQ2BSP(reader, parsed_bsp.emplace<q2bsp_t>());
    } else if (detected_version == &bspver_qbism) {
        ReadQ2BSP(reader, parsed_bsp.emplace<q2bsp_qbism_t>());
    } else if (detected_version == &bspver_q1 || detected_version == &bspver_h2 || detected_version == &bspver_hl) {
        ReadQ1BSP(reader, parsed_bsp.emplace<bsp29_t>());
    } else if (detected_version == &bspver_bsp2rmq || detected_version == &bspver_h2bsp2rmq) {
        ReadQ1BSP(reader, parsed_bsp.emplace<bsp2rmq_t>());
    } else if (detected_version == &bspver_bsp2 || detected_version == &bspver_h2bsp2) {
        ReadQ1BSP(reader, parsed_bsp.emplace<bsp2_t>());
    } else {
        FError("Unknown format");
    }

    bspdata->file = requested_filename;
    bspdata->version = detected_version;
    bspdata->loadversion = nullptr;
    bspdata->bsp = std::move(parsed_bsp);

    // BSPX data is optional and belongs exclusively to the file just loaded.
    // Clear entries from a previous use of this bspdata_t before probing, and
    // stage any new entries so malformed metadata cannot leave a partial set.
    bspdata->bspx.entries.clear();
    bspdata_t::bspxentries parsed_bspx;

    size_t bspxofs;

    // detect BSPX
    /*bspx header is positioned exactly+4align at the end of the last lump position (regardless of order)*/
    for (i = 0, bspxofs = 0; i < lumps.size(); i++) {
        const size_t lump_end = static_cast<size_t>(lumps[i].fileofs) + static_cast<size_t>(lumps[i].filelen);
        bspxofs = std::max(bspxofs, lump_end);
    }

    if (bspxofs > std::numeric_limits<size_t>::max() - 3) {
        logging::print("WARNING: invalid BSPX offset\n");
        return;
    }
    bspxofs = (bspxofs + 3) & ~3;

    /*okay, so that's where it *should* be if it exists */
    constexpr size_t bspx_header_size = sizeof(uint32_t) * 2;
    constexpr size_t bspx_lump_size = 24 + (sizeof(uint32_t) * 2);
    if (byte_range_within_file(bspxofs, bspx_header_size, file_data->size())) {
        stream.seekg(bspxofs);

        bspx_header_t bspx;
        stream >= bspx;

        if (!stream || memcmp(bspx.id.data(), "BSPX", 4)) {
            logging::print("WARNING: invalid BSPX header\n");
            return;
        }

        const size_t lump_headers_offset = bspxofs + bspx_header_size;
        const size_t max_lump_headers = (file_data->size() - lump_headers_offset) / bspx_lump_size;
        if (bspx.numlumps > max_lump_headers) {
            logging::print(
                "WARNING: invalid BSPX lump count {} (at most {} headers fit)\n", bspx.numlumps, max_lump_headers);
            return;
        }

        for (size_t i = 0; i < bspx.numlumps; i++) {
            bspx_lump_t xlump;

            if (!(stream >= xlump)) {
                logging::print("WARNING: invalid BSPX lump at index {}\n", i);
                return;
            }

            if (!byte_range_within_file(xlump.fileofs, xlump.filelen, file_data->size())) {
                logging::print("WARNING: invalid BSPX lump at index {}\n", i);
                return;
            }

            const auto name_end = std::find(xlump.lumpname.begin(), xlump.lumpname.end(), '\0');
            if (name_end == xlump.lumpname.end()) {
                logging::print("WARNING: unterminated BSPX lump name at index {}\n", i);
                return;
            }
            const std::string_view name(xlump.lumpname.data(), std::distance(xlump.lumpname.begin(), name_end));
            if (name.empty() || parsed_bspx.entries.contains(std::string(name))) {
                logging::print("WARNING: invalid or duplicate BSPX lump name at index {}\n", i);
                return;
            }
            parsed_bspx.transfer(name, std::vector<uint8_t>(file_data->begin() + xlump.fileofs,
                                           file_data->begin() + xlump.fileofs + xlump.filelen));
        }

        bspdata->bspx.entries = std::move(parsed_bspx.entries);
    }
}

/* ========================================================================= */
#include <fstream>

static void validate_bspx_for_write(const bspdata_t &bspdata)
{
    if (bspdata.bspx.entries.size() > std::numeric_limits<uint32_t>::max()) {
        FError("too many BSPX lumps ({})", bspdata.bspx.entries.size());
    }

    for (const auto &[name, data] : bspdata.bspx.entries) {
        if (name.empty() || name.find('\0') != std::string::npos) {
            FError("BSPX lump names must be non-empty strings without embedded nulls");
        }
        if (name.size() >= bspx_lump_t{}.lumpname.size()) {
            FError("BSPX lump name '{}' is too long (maximum is 23 bytes)", name);
        }
        if (data.size() > std::numeric_limits<uint32_t>::max() ||
            data.size() > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            FError("BSPX lump '{}' is too large ({} bytes)", name, data.size());
        }
    }
}

static void validate_bsp_for_write(const bspdata_t &bspdata)
{
    const bspversion_t *version = bspdata.version;
    bool matches_version = false;

    if (version == &bspver_q1 || version == &bspver_h2 || version == &bspver_hl) {
        matches_version = std::holds_alternative<bsp29_t>(bspdata.bsp);
    } else if (version == &bspver_bsp2rmq || version == &bspver_h2bsp2rmq) {
        matches_version = std::holds_alternative<bsp2rmq_t>(bspdata.bsp);
    } else if (version == &bspver_bsp2 || version == &bspver_h2bsp2) {
        matches_version = std::holds_alternative<bsp2_t>(bspdata.bsp);
    } else if (version == &bspver_q2) {
        matches_version = std::holds_alternative<q2bsp_t>(bspdata.bsp);
    } else if (version == &bspver_qbism) {
        matches_version = std::holds_alternative<q2bsp_qbism_t>(bspdata.bsp);
    }

    if (!matches_version) {
        FError("BSP data does not match the requested output format '{}'", version->short_name);
    }

    const bool expects_hexen2_models = version->game->id == GAME_HEXEN_II;
    const auto validate_q1_models = [&](const auto &bsp) {
        const bool has_expected_models = expects_hexen2_models ? std::holds_alternative<dmodelh2_vector>(bsp.dmodels)
                                                               : std::holds_alternative<dmodelq1_vector>(bsp.dmodels);
        if (!has_expected_models) {
            FError("BSP model layout does not match the requested output format '{}'", version->short_name);
        }
    };

    if (const auto *bsp = std::get_if<bsp29_t>(&bspdata.bsp)) {
        validate_q1_models(*bsp);
    } else if (const auto *bsp = std::get_if<bsp2rmq_t>(&bspdata.bsp)) {
        validate_q1_models(*bsp);
    } else if (const auto *bsp = std::get_if<bsp2_t>(&bspdata.bsp)) {
        validate_q1_models(*bsp);
    }

    validate_bspx_for_write(bspdata);
}

struct bspfile_t
{
    const bspversion_t *version;

    // which one is used depends on version
    union
    {
        dheader_t q1header;
        q2_dheader_t q2header;
    };

    std::ofstream stream;

private:
    int32_t checked_official_position(std::string_view context)
    {
        const std::streampos position = stream.tellp();
        if (position == std::streampos(-1)) {
            FError("unable to determine output position while writing {}", context);
        }

        const std::streamoff offset = static_cast<std::streamoff>(position);
        if (offset < 0 || static_cast<uint64_t>(offset) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            FError("{} exceeds the 2 GiB official BSP file limit", context);
        }

        return static_cast<int32_t>(offset);
    }

    uint32_t checked_bspx_position(std::string_view context)
    {
        const std::streampos position = stream.tellp();
        if (position == std::streampos(-1)) {
            FError("unable to determine output position while writing {}", context);
        }

        const std::streamoff offset = static_cast<std::streamoff>(position);
        if (offset < 0 || static_cast<uint64_t>(offset) > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            FError("{} exceeds the 4 GiB BSPX file limit", context);
        }

        return static_cast<uint32_t>(offset);
    }

    void finish_official_lump(lump_t &lump, int32_t start, std::string_view name)
    {
        if (!stream) {
            FError("error writing {} lump", name);
        }

        const int32_t end = checked_official_position(fmt::format("{} lump", name));
        if (end < start) {
            FError("output position moved backwards while writing {} lump", name);
        }
        lump.fileofs = start;
        lump.filelen = end - start;

        if (lump.filelen % 4) {
            const int32_t padding = 4 - (lump.filelen % 4);
            if (end > std::numeric_limits<int32_t>::max() - padding) {
                FError("padding {} lump would exceed the 2 GiB official BSP file limit", name);
            }
            stream <= padding_n(padding);
            if (!stream) {
                FError("error padding {} lump", name);
            }
        }
    }

    // write structured lump data from vector
    template<typename T>
    inline void write_lump(size_t lump_num, const std::vector<T> &data)
    {
        Q_assert(version->lumps.size() > lump_num);
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        lump_t *lumps;

        if (version->version.has_value()) {
            lumps = q2header.lumps.data();
        } else {
            lumps = q1header.lumps.data();
        }

        lump_t &lump = lumps[lump_num];

        if (lumpspec.size != 0 &&
            data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / lumpspec.size) {
            FError("{} lump is too large ({} entries)", lumpspec.name, data.size());
        }

        const size_t expected_size = lumpspec.size * data.size();
        const int32_t start = checked_official_position(fmt::format("{} lump", lumpspec.name));

        for (auto &v : data)
            stream <= v;

        finish_official_lump(lump, start, lumpspec.name);

        if (static_cast<size_t>(lump.filelen) != expected_size) {
            FError("{} lump serialized to {} bytes, expected {}", lumpspec.name, lump.filelen, expected_size);
        }
    }

    // this is only here to satisfy std::visit
    constexpr void write_lump(size_t, const std::monostate &) { }

    // write structured string data
    inline void write_lump(size_t lump_num, const std::string &data)
    {
        Q_assert(version->lumps.size() > lump_num);
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        lump_t *lumps;

        Q_assert(lumpspec.size == 1);

        if (version->version.has_value()) {
            lumps = q2header.lumps.data();
        } else {
            lumps = q1header.lumps.data();
        }

        lump_t &lump = lumps[lump_num];

        if (data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 1 ||
            data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()) - 1) {
            FError("{} lump is too large ({} bytes)", lumpspec.name, data.size());
        }

        const int32_t start = checked_official_position(fmt::format("{} lump", lumpspec.name));

        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.put('\0');

        finish_official_lump(lump, start, lumpspec.name);

        if (static_cast<size_t>(lump.filelen) != data.size() + 1) {
            FError("{} lump serialized to {} bytes, expected {}", lumpspec.name, lump.filelen, data.size() + 1);
        }
    }

    // write structured lump data
    template<typename T, typename = std::enable_if_t<std::is_member_function_pointer_v<decltype(&T::stream_write)>>>
    inline void write_lump(size_t lump_num, const T &data)
    {
        const lumpspec_t &lumpspec = version->lumps.begin()[lump_num];
        lump_t *lumps;

        Q_assert(lumpspec.size == 1);

        if (version->version.has_value()) {
            lumps = q2header.lumps.data();
        } else {
            lumps = q1header.lumps.data();
        }

        lump_t &lump = lumps[lump_num];

        const int32_t start = checked_official_position(fmt::format("{} lump", lumpspec.name));

        data.stream_write(stream);

        finish_official_lump(lump, start, lumpspec.name);
    }

public:
    inline void write_bsp(const mbsp_t &) { FError("Can't write generic BSP"); }
    inline void write_bsp(const std::monostate &) { FError("No BSP to write"); }

    template<typename T, typename std::enable_if_t<std::is_base_of_v<q1bsp_tag_t, T>, int> = 0>
    inline void write_bsp(const T &bsp)
    {
        write_lump(LUMP_PLANES, bsp.dplanes);
        write_lump(LUMP_LEAFS, bsp.dleafs);
        write_lump(LUMP_VERTEXES, bsp.dvertexes);
        write_lump(LUMP_NODES, bsp.dnodes);
        write_lump(LUMP_TEXINFO, bsp.texinfo);
        write_lump(LUMP_FACES, bsp.dfaces);
        write_lump(LUMP_CLIPNODES, bsp.dclipnodes);
        write_lump(LUMP_MARKSURFACES, bsp.dmarksurfaces);
        write_lump(LUMP_SURFEDGES, bsp.dsurfedges);
        write_lump(LUMP_EDGES, bsp.dedges);
        std::visit([this](auto &&arg) { this->write_lump(LUMP_MODELS, arg); }, bsp.dmodels);

        write_lump(LUMP_LIGHTING, bsp.dlightdata);
        write_lump(LUMP_VISIBILITY, bsp.dvisdata);
        write_lump(LUMP_ENTITIES, bsp.dentdata);
        write_lump(LUMP_TEXTURES, bsp.dtex);
    }

    template<typename T, typename std::enable_if_t<std::is_base_of_v<q2bsp_tag_t, T>, int> = 0>
    inline void write_bsp(const T &bsp)
    {
        write_lump(Q2_LUMP_PLANES, bsp.dplanes);
        write_lump(Q2_LUMP_LEAFS, bsp.dleafs);
        write_lump(Q2_LUMP_VERTEXES, bsp.dvertexes);
        write_lump(Q2_LUMP_NODES, bsp.dnodes);
        write_lump(Q2_LUMP_TEXINFO, bsp.texinfo);
        write_lump(Q2_LUMP_FACES, bsp.dfaces);
        write_lump(Q2_LUMP_LEAFFACES, bsp.dleaffaces);
        write_lump(Q2_LUMP_SURFEDGES, bsp.dsurfedges);
        write_lump(Q2_LUMP_EDGES, bsp.dedges);
        write_lump(Q2_LUMP_MODELS, bsp.dmodels);
        write_lump(Q2_LUMP_LEAFBRUSHES, bsp.dleafbrushes);
        write_lump(Q2_LUMP_BRUSHES, bsp.dbrushes);
        write_lump(Q2_LUMP_BRUSHSIDES, bsp.dbrushsides);
        write_lump(Q2_LUMP_AREAS, bsp.dareas);
        write_lump(Q2_LUMP_AREAPORTALS, bsp.dareaportals);

        write_lump(Q2_LUMP_LIGHTING, bsp.dlightdata);
        write_lump(Q2_LUMP_VISIBILITY, bsp.dvis);
        write_lump(Q2_LUMP_ENTITIES, bsp.dentdata);
    }

    inline void write_bspx(const bspdata_t &bspdata)
    {
        if (!bspdata.bspx.entries.size())
            return;

        const uint32_t bspx_start = checked_bspx_position("BSPX header");
        if ((bspx_start % 4) != 0) {
            FError("BSPX header is misaligned");
        }
        constexpr uint64_t bspx_header_size = sizeof(uint32_t) * 2;
        constexpr uint64_t bspx_lump_header_size = 24 + (sizeof(uint32_t) * 2);
        const uint64_t header_table_size =
            bspx_header_size + static_cast<uint64_t>(bspdata.bspx.entries.size()) * bspx_lump_header_size;
        if (header_table_size > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - bspx_start) {
            FError("BSPX lump table exceeds the 4 GiB BSPX file limit");
        }

        stream <= bspx_header_t(static_cast<uint32_t>(bspdata.bspx.entries.size()));

        auto bspxheader = stream.tellp();

        // write dummy lump headers
        for ([[maybe_unused]] auto &_ : bspdata.bspx.entries) {
            stream <= bspx_lump_t{};
        }
        if (!stream) {
            FError("unable to write BSPX lump table");
        }

        std::vector<bspx_lump_t> xlumps;
        xlumps.reserve(bspdata.bspx.entries.size());

        for (auto &x : bspdata.bspx.entries) {
            bspx_lump_t &lump = xlumps.emplace_back();
            const uint32_t offset = checked_bspx_position(fmt::format("BSPX lump '{}'", x.first));
            const size_t padding = (4 - (x.second.size() % 4)) % 4;
            if (x.second.size() > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - offset ||
                padding > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - offset - x.second.size()) {
                FError("BSPX lump '{}' exceeds the 4 GiB BSPX file limit", x.first);
            }

            lump.filelen = static_cast<uint32_t>(x.second.size());
            lump.fileofs = static_cast<uint32_t>(offset);
            memcpy(lump.lumpname.data(), x.first.data(), x.first.size());

            stream.write(
                reinterpret_cast<const char *>(x.second.data()), static_cast<std::streamsize>(x.second.size()));
            if (!stream) {
                FError("unable to write BSPX lump '{}'", x.first);
            }

            if (padding) {
                stream <= padding_n(padding);
                if (!stream) {
                    FError("unable to pad BSPX lump '{}'", x.first);
                }
            }
        }

        stream.seekp(bspxheader);
        if (!stream) {
            FError("unable to seek to BSPX lump headers");
        }

        for (auto &lump : xlumps)
            stream <= lump;

        if (!stream) {
            FError("unable to finalize BSPX lump headers");
        }
    }
};

namespace
{
struct staged_bsp_cleanup_t
{
    explicit staged_bsp_cleanup_t(fs::path path)
        : path(std::move(path))
    {
    }

    ~staged_bsp_cleanup_t()
    {
        if (active) {
            std::error_code ignored;
            if (fs::remove(path, ignored) || !ignored) {
                return;
            }

            // The staged file may already have inherited read-only
            // destination permissions when a later rename fails. Restore
            // owner-write on this private file so Windows can remove it and a
            // handled failure does not permanently block the next attempt.
            ignored.clear();
            fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ignored);
            if (!ignored) {
                fs::remove(path, ignored);
            }
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

static void remove_bsp_backup(const fs::path &backup_path, const fs::path &output_path)
{
    std::error_code error;
    if (fs::remove(backup_path, error) || !error) {
        return;
    }

    // A replaced read-only file cannot be removed on Windows until its write
    // permission is restored. This changes only the private backup, never the
    // newly installed output.
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
} // namespace

/*
 * =============
 * WriteBSPFile
 * Swaps the bsp file in place, so it should not be referenced again
 * =============
 */
void WriteBSPFile(const fs::path &filename, bspdata_t *bspdata)
{
    if (!bspdata || !bspdata->version) {
        FError("WriteBSPFile requires BSP data with a valid format");
    }

    // Reject inconsistent in-memory metadata before truncating the destination
    // or selecting a header union member from the wrong format family.
    validate_bsp_for_write(*bspdata);

    const fs::file_status destination_status = checked_output_status(filename, "output path");
    const bool destination_exists = fs::exists(destination_status);
    if (destination_exists && !fs::is_regular_file(destination_status)) {
        FError("output path {} exists and is not a regular file", filename);
    }

    fs::path temporary_path = filename;
    temporary_path += ".tmp";
    ensure_output_path_available(temporary_path, "temporary output path");

    fs::path backup_path = filename;
    backup_path += ".previous.tmp";
    ensure_output_path_available(backup_path, "backup output path");

    staged_bsp_cleanup_t staged_cleanup(temporary_path);

    bspfile_t bspfile{};

    bspfile.version = bspdata->version;

    // headers are union'd, so this sets both
    bspfile.q2header.ident = bspfile.version->ident;

    if (bspfile.version->version.has_value()) {
        bspfile.q2header.version = bspfile.version->version.value();
    }

    logging::print("Writing {} as {}\n", filename, *bspdata->version);
    bspfile.stream.open(temporary_path, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);

    if (!bspfile.stream)
        FError("unable to open {} for writing", temporary_path);

    if (destination_exists && destination_status.permissions() != fs::perms::unknown) {
        std::error_code error;
        fs::permissions(temporary_path, destination_status.permissions(), fs::perm_options::replace, error);
        if (error) {
            FError("unable to preserve permissions for {}: {}", filename, error.message());
        }
    }

    bspfile.stream << endianness<std::endian::little>;

    /* Save header space, updated after adding the lumps */
    if (bspfile.version->version.has_value()) {
        bspfile.stream <= bspfile.q2header;
    } else {
        bspfile.stream <= bspfile.q1header;
    }
    if (!bspfile.stream) {
        FError("unable to reserve BSP header in {}", temporary_path);
    }

    std::visit([&bspfile](auto &&arg) { bspfile.write_bsp(arg); }, bspdata->bsp);

    /*BSPX lumps are at a 4-byte alignment after the last of any official lump*/
    bspfile.write_bspx(*bspdata);

    bspfile.stream.seekp(0);

    // write the real header
    if (bspfile.version->version.has_value()) {
        bspfile.stream <= bspfile.q2header;
    } else {
        bspfile.stream <= bspfile.q1header;
    }

    bspfile.stream.flush();
    if (!bspfile.stream) {
        FError("error finalizing BSP file {}", temporary_path);
    }

    bspfile.stream.close();
    if (!bspfile.stream) {
        FError("error closing BSP file {}", temporary_path);
    }

    std::error_code error;
    if (destination_exists) {
        fs::rename(filename, backup_path, error);
        if (error) {
            FError("unable to preserve existing output {}: {}", filename, error.message());
        }
    }

    fs::rename(temporary_path, filename, error);
    if (error) {
        const std::error_code promotion_error = error;

        if (destination_exists) {
            std::error_code rollback_error;
            fs::rename(backup_path, filename, rollback_error);
            if (rollback_error) {
                FError("unable to install output {}: {}; restoring the previous output also failed: {}", filename,
                    promotion_error.message(), rollback_error.message());
            }
        }
        FError("unable to install output {}: {}", filename, promotion_error.message());
    }

    staged_cleanup.release();

    if (destination_exists) {
        remove_bsp_backup(backup_path, filename);
    }
}

/* ========================================================================= */

inline void PrintLumpSize(const lumpspec_t &lump, size_t count)
{
    logging::print("{:7} {:<12} {:10}\n", count, lump.name, count * lump.size);
}

template<typename T>
inline void PrintQ1BSPLumps(const std::initializer_list<lumpspec_t> &lumpspec, const T &bsp)
{
    if (std::holds_alternative<dmodelh2_vector>(bsp.dmodels))
        PrintLumpSize(lumpspec.begin()[LUMP_MODELS], std::get<dmodelh2_vector>(bsp.dmodels).size());
    else
        PrintLumpSize(lumpspec.begin()[LUMP_MODELS], std::get<dmodelq1_vector>(bsp.dmodels).size());

    PrintLumpSize(lumpspec.begin()[LUMP_PLANES], bsp.dplanes.size());
    PrintLumpSize(lumpspec.begin()[LUMP_VERTEXES], bsp.dvertexes.size());
    PrintLumpSize(lumpspec.begin()[LUMP_NODES], bsp.dnodes.size());
    PrintLumpSize(lumpspec.begin()[LUMP_TEXINFO], bsp.texinfo.size());
    PrintLumpSize(lumpspec.begin()[LUMP_FACES], bsp.dfaces.size());
    PrintLumpSize(lumpspec.begin()[LUMP_CLIPNODES], bsp.dclipnodes.size());
    PrintLumpSize(lumpspec.begin()[LUMP_LEAFS], bsp.dleafs.size());
    PrintLumpSize(lumpspec.begin()[LUMP_MARKSURFACES], bsp.dmarksurfaces.size());
    PrintLumpSize(lumpspec.begin()[LUMP_EDGES], bsp.dedges.size());
    PrintLumpSize(lumpspec.begin()[LUMP_SURFEDGES], bsp.dsurfedges.size());

    logging::print("{:7} {:<12} {:10}\n", bsp.dtex.textures.size(), "textures", bsp.dtex.stream_size());
    logging::print("{:7} {:<12} {:10}\n", "", "lightdata", bsp.dlightdata.size());
    logging::print("{:7} {:<12} {:10}\n", "", "visdata", bsp.dvisdata.size());
    logging::print("{:7} {:<12} {:10}\n", "", "entdata", bsp.dentdata.size() + 1); // include the null terminator
}

template<typename T>
inline void PrintQ2BSPLumps(const std::initializer_list<lumpspec_t> &lumpspec, const T &bsp)
{
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_MODELS], bsp.dmodels.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_PLANES], bsp.dplanes.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_VERTEXES], bsp.dvertexes.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_NODES], bsp.dnodes.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_TEXINFO], bsp.texinfo.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_FACES], bsp.dfaces.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_LEAFS], bsp.dleafs.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_LEAFFACES], bsp.dleaffaces.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_LEAFBRUSHES], bsp.dleafbrushes.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_EDGES], bsp.dedges.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_SURFEDGES], bsp.dsurfedges.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_BRUSHES], bsp.dbrushes.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_BRUSHSIDES], bsp.dbrushsides.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_AREAS], bsp.dareas.size());
    PrintLumpSize(lumpspec.begin()[Q2_LUMP_AREAPORTALS], bsp.dareaportals.size());

    logging::print("{:7} {:<12} {:10}\n", "", "lightdata", bsp.dlightdata.size());
    logging::print("{:7} {:<12} {:10}\n", "", "visdata", bsp.dvis.bits.size());
    logging::print("{:7} {:<12} {:10}\n", "", "entdata", bsp.dentdata.size() + 1); // include the null terminator
}

/*
 * =============
 * PrintBSPFileSizes
 * Dumps info about the bsp data
 * =============
 */
void PrintBSPFileSizes(const bspdata_t *bspdata)
{
    const auto &lumpspec = bspdata->version->lumps;

    logging::print("\n{:7} {:<12} {:10}\n", "count", "lump name", "byte size");

    if (std::holds_alternative<q2bsp_t>(bspdata->bsp)) {
        PrintQ2BSPLumps(lumpspec, std::get<q2bsp_t>(bspdata->bsp));
    } else if (std::holds_alternative<q2bsp_qbism_t>(bspdata->bsp)) {
        PrintQ2BSPLumps(lumpspec, std::get<q2bsp_qbism_t>(bspdata->bsp));
    } else if (std::holds_alternative<bsp29_t>(bspdata->bsp)) {
        PrintQ1BSPLumps(lumpspec, std::get<bsp29_t>(bspdata->bsp));
    } else if (std::holds_alternative<bsp2rmq_t>(bspdata->bsp)) {
        PrintQ1BSPLumps(lumpspec, std::get<bsp2rmq_t>(bspdata->bsp));
    } else if (std::holds_alternative<bsp2_t>(bspdata->bsp)) {
        PrintQ1BSPLumps(lumpspec, std::get<bsp2_t>(bspdata->bsp));
    } else {
        Error("Unsupported BSP version: {}", *bspdata->version);
    }

    if (bspdata->bspx.entries.size()) {
        logging::print("\n{:<16} {:10}\n", "BSPX lump name", "byte size");

        for (auto &x : bspdata->bspx.entries) {
            logging::print("{:<16} {:10}\n", x.first, x.second.size());
        }
    }
}
