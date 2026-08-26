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
#include <common/cmdlib.hh>
#include <common/bspfile.hh>
#include <common/log.hh>
#include <common/settings.hh>

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <common/json.hh>
#include "common/fs.hh"

#include "common/polylib.hh"
#include "common/bsputils.hh"

static void PrintBSPTextureUsage(const mbsp_t &bsp)
{
    std::unordered_map<std::string, double> areas;

    for (auto &face : bsp.dfaces) {
        const char *name = Face_TextureName(&bsp, &face);

        if (!name || !*name) {
            continue;
        }

        auto points = Face_Points(&bsp, &face);
        polylib::winding_t w(points.begin(), points.end());
        double area = w.area();

        areas[name] += area;
    }

    std::vector<std::tuple<std::string, double>> areasVec;

    for (auto &area : areas) {
        areasVec.push_back(std::make_tuple(area.first, area.second));
    }

    std::sort(areasVec.begin(), areasVec.end(), [](auto &l, auto &r) { return std::get<1>(r) < std::get<1>(l); });

    printf("\n");

    for (auto &area : areasVec) {
        fmt::print("{},{:.0f}\n", std::get<0>(area), std::get<1>(area));
    }
}

static void ValidateTexinfoChains(const mbsp_t &bsp)
{
    enum class visit_state : uint8_t
    {
        unseen,
        visiting,
        done
    };
    std::vector<visit_state> states(bsp.texinfo.size(), visit_state::unseen);

    for (size_t root = 0; root < bsp.texinfo.size(); ++root) {
        if (states[root] != visit_state::unseen)
            continue;

        std::vector<size_t> path;
        int64_t current = static_cast<int64_t>(root);

        while (current != -1) {
            if (current < 0 || static_cast<size_t>(current) >= bsp.texinfo.size()) {
                FError("texinfo {} has out-of-range nexttexinfo index {}", path.empty() ? root : path.back(), current);
            }

            const size_t index = static_cast<size_t>(current);
            if (states[index] == visit_state::done)
                break;
            if (states[index] == visit_state::visiting) {
                FError("infinite texinfo animation chain detected at index {}", index);
            }

            states[index] = visit_state::visiting;
            path.push_back(index);
            current = bsp.texinfo[index].nexttexinfo;
        }

        for (size_t index : path) {
            states[index] = visit_state::done;
        }
    }
}

// TODO
settings::common_settings bspinfo_options;

static void PrintUsage()
{
    fmt::print("usage: vmt-bspinfo [-help/-h/-?] [-werror] [-export-lightmap-atlas] [--] bspfile [bspfiles]\n");
}

static bool IsHelpArgument(std::string_view argument)
{
    return argument == "-help" || argument == "--help" || argument == "-h" || argument == "--h" || argument == "-?" ||
           argument == "--?";
}

int main(int argc, char **argv)
{
    try {
        logging::preinitialize();
        logging::reset_warning_count();

        fmt::print("---- vmt-bspinfo / VibeyMapTools {} ----\n", VIBEYMAPTOOLS_VERSION);
        if (argc == 2 && IsHelpArgument(argv[1])) {
            PrintUsage();
            return 0;
        }
        if (argc == 1) {
            PrintUsage();
            return 1;
        }

        std::vector<std::string_view> inputs;
        bool end_of_options = false;
        bool export_lightmap_atlas = false;
        for (int32_t i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];

            if (!end_of_options && argument == "--") {
                end_of_options = true;
                continue;
            }
            if (!end_of_options && IsHelpArgument(argument)) {
                PrintUsage();
                return 0;
            }
            if (!end_of_options && (argument == "-werror" || argument == "--werror")) {
                logging::set_warnings_as_errors(true);
                continue;
            }
            if (!end_of_options && (argument == "-export-lightmap-atlas" || argument == "--export-lightmap-atlas")) {
                export_lightmap_atlas = true;
                continue;
            }
            if (!end_of_options && argument.starts_with('-')) {
                fmt::print(stderr, "ERROR: unknown option '{}'\n", argument);
                PrintUsage();
                return 1;
            }

            inputs.push_back(argument);
        }

        if (inputs.empty()) {
            fmt::print(stderr, "ERROR: expected at least one BSP input file\n");
            PrintUsage();
            return 1;
        }

        for (const std::string_view input : inputs) {
            printf("---------------------\n");
            fs::path source = DefaultExtension(std::string(input), ".bsp");
            fmt::print("{}\n", source);

            bspdata_t bsp;
            LoadBSPFile(source, &bsp);

            bsp.version->game->init_filesystem(source, bspinfo_options);

            PrintBSPFileSizes(&bsp);

            // WriteBSPFile(fs::path(source).replace_extension("bsp.rewrite"), &bsp);

            if (!ConvertBSPFormat(&bsp, &bspver_generic)) {
                FError("couldn't convert {} to the generic BSP representation", source);
            }

            ValidateTexinfoChains(std::get<mbsp_t>(bsp.bsp));

            serialize_bsp(
                bsp, std::get<mbsp_t>(bsp.bsp), fs::path(source).replace_extension("bsp.json"), export_lightmap_atlas);

            PrintBSPTextureUsage(std::get<mbsp_t>(bsp.bsp));

            printf("---------------------\n");

            fs::clear();
        }

        logging::fail_if_warnings();
        return 0;
    } catch (const std::exception &e) {
        exit_on_exception(e);
    }
}
