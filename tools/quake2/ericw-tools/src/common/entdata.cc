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

#include <common/entdata.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <common/bsputils.hh>
#include <common/parser.hh>

#include <fmt/core.h>

entdict_t::entdict_t(std::initializer_list<keyvalue_t> l)
    : keyvalues(l)
{
}

entdict_t::entdict_t() = default;

entdict_t::entdict_t(parser_base_t &parser)
{
    parse(parser);
}

const std::string &entdict_t::get(std::string_view key) const
{
    if (auto it = find(key); it != keyvalues.end()) {
        return it->second;
    }

    static std::string empty;
    return empty;
}

double entdict_t::get_float(std::string_view key) const
{
    const std::string &s = get(key);

    if (s.empty()) {
        return 0;
    }

    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE || !std::isfinite(value)) {
        return 0;
    }
    return value;
}

int32_t entdict_t::get_int(std::string_view key) const
{
    const std::string &s = get(key);

    if (s.empty()) {
        return 0;
    }

    errno = 0;
    char *end = nullptr;
    const long long value = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE || value < std::numeric_limits<int32_t>::lowest() ||
        value > std::numeric_limits<int32_t>::max()) {
        return 0;
    }
    return static_cast<int32_t>(value);
}

int32_t entdict_t::get_vector(std::string_view key, qvec3f &vec) const
{
    const std::string &value = get(key);

    vec = {};
    const char *position = value.c_str();
    int32_t parsed_components = 0;
    for (size_t component = 0; component < vec.size(); ++component) {
        while (std::isspace(static_cast<unsigned char>(*position))) {
            ++position;
        }

        errno = 0;
        char *end = nullptr;
        const float parsed = std::strtof(position, &end);
        if (end == position || errno == ERANGE || !std::isfinite(parsed)) {
            break;
        }

        vec[component] = parsed;
        position = end;
        ++parsed_components;
    }
    return parsed_components;
}

void entdict_t::set(std::string_view key, std::string_view value)
{
    // search for existing key to update
    if (auto it = find(key); it != keyvalues.end()) {
        // found existing key
        it->second = value;
        return;
    }

    // no existing key; add new
    keyvalues.emplace_back(key, value);
}

void entdict_t::remove(std::string_view key)
{
    if (auto it = find(key); it != keyvalues.end()) {
        keyvalues.erase(it);
    }
}

void entdict_t::rename(std::string_view from, std::string_view to)
{
    if (from == to) {
        return;
    }

    const auto it = find(from);
    if (it != end()) {
        auto oldValue = std::move(it->second);
        keyvalues.erase(it);
        if (const auto destination = find(to); destination != end()) {
            destination->second = std::move(oldValue);
        } else {
            keyvalues.emplace_back(to, std::move(oldValue));
        }
    }
}

keyvalues_t::iterator entdict_t::find(std::string_view key)
{
    auto existingIt = keyvalues.end();
    for (auto it = keyvalues.begin(); it != keyvalues.end(); ++it) {
        if (key == it->first) {
            existingIt = it;
            break;
        }
    }
    return existingIt;
}

keyvalues_t::const_iterator entdict_t::find(std::string_view key) const
{
    auto existingIt = keyvalues.end();
    for (auto it = keyvalues.begin(); it != keyvalues.end(); ++it) {
        if (key == it->first) {
            existingIt = it;
            break;
        }
    }
    return existingIt;
}

bool entdict_t::has(std::string_view key) const
{
    return find(key) != end();
}

void entdict_t::parse(parser_base_t &parser)
{
    /* parse the opening brace */
    if (!parser.parse_token())
        return;
    if (parser.token != "{")
        FError("found {} when expecting {{", parser.token);

    /* go through all the keys in this entity */
    while (1) {
        /* parse key */
        if (!parser.parse_token())
            FError("EOF without closing brace");

        if (parser.token == "}")
            break;

        std::string keystr = parser.token;

        /* parse value */
        if (!parser.parse_token())
            FError("EOF without closing brace");

        if (parser.token == "}")
            FError("closing brace without data");

        // Trim in two bulk operations so a maliciously long padded key cannot
        // force quadratic repeated front erases.
        const auto first = std::find_if_not(
            keystr.begin(), keystr.end(), [](unsigned char character) { return std::isspace(character); });
        const auto last = std::find_if_not(keystr.rbegin(), keystr.rend(), [](unsigned char character) {
            return std::isspace(character);
        }).base();
        const size_t leading_whitespace = static_cast<size_t>(std::distance(keystr.begin(), first));
        const size_t content_end = static_cast<size_t>(std::distance(keystr.begin(), last));
        keystr.erase(content_end);
        keystr.erase(0, leading_whitespace);

        set(keystr, parser.token);
    }
}
void EntData_ParseInto(parser_t &parser, std::vector<entdict_t> &vector)
{
    /* go through all the entities */
    while (1) {
        /* parse the opening brace */
        if (parser.at_end() || !parser.parse_token(PARSE_PEEK))
            break;

        // emplace a new entdict_t out of the parser
        vector.emplace_back(parser);
    }
}

std::vector<entdict_t> EntData_Parse(parser_t &parser)
{
    std::vector<entdict_t> result;

    EntData_ParseInto(parser, result);

    return result;
}

std::vector<entdict_t> EntData_Parse(const mbsp_t &bsp)
{
    parser_t parser{bsp.dentdata, {bsp.file.string()}};
    return EntData_Parse(parser);
}

/*
 * ================
 * EntData_Write
 * ================
 */
std::string EntData_Write(const std::vector<entdict_t> &ents)
{
    std::string out;
    for (const auto &ent : ents) {
        out += "{\n";
        for (const auto &epair : ent) {
            fmt::format_to(std::back_inserter(out), "\"{}\" \"{}\"\n", epair.first, epair.second);
        }
        out += "}\n";
    }
    return out;
}
