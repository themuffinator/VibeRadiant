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

#include <common/prtfile.hh>

#include <common/log.hh>
#include <common/fs.hh>
#include <common/bspfile.hh>
#include <common/ostream.hh>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

constexpr const char *PORTALFILE = "PRT1";
constexpr const char *PORTALFILE2 = "PRT2";
constexpr const char *PORTALFILEAM = "PRT1-AM";

constexpr size_t PRT_MAX_WINDING = 64;
// Portal files are an interchange format, so do not bind them to one BSP
// version's legacy limits. These caps are still comfortably above the known
// practical limits while preventing hostile headers from causing unbounded
// allocation or parse loops.
constexpr int PRT_MAX_LEAFS = 1 << 20;
constexpr int PRT_MAX_PORTALS = 1 << 20;

static void ValidatePortalWinding(const prtfile_winding_t &winding, int portal_index)
{
    constexpr double minimum_magnitude = 1e-12;

    // LoadPortals derives the portal plane from the first three points. Reject
    // a zero normal here rather than letting a malformed plane enter VIS flow.
    const qvec3d origin = winding[0];
    const qvec3d initial_normal = qv::cross(winding[1] - origin, winding[2] - origin);
    const double initial_normal_length = qv::length(initial_normal);
    if (!std::isfinite(initial_normal_length) || initial_normal_length <= minimum_magnitude) {
        FError("portal {} has a degenerate first triangle", portal_index);
    }

    const qvec3d normal = initial_normal / initial_normal_length;
    const double plane_distance = qv::dot(origin, normal);
    qvec3d twice_area{};

    for (size_t point_index = 0; point_index < winding.size(); ++point_index) {
        const qvec3d &point = winding[point_index];
        const qvec3d &next = winding[(point_index + 1) % winding.size()];
        const double edge_length = qv::length(next - point);
        if (!std::isfinite(edge_length) || edge_length <= minimum_magnitude) {
            FError("portal {} has a degenerate edge at point {}", portal_index, point_index);
        }

        const double distance_from_plane = qv::dot(point, normal) - plane_distance;
        if (!std::isfinite(distance_from_plane) || std::abs(distance_from_plane) > DEFAULT_ON_EPSILON) {
            FError("portal {} point {} is not on the portal plane", portal_index, point_index);
        }

        // Translate around the first point to reduce cancellation for portals
        // far from the world origin.
        twice_area += qv::cross(point - origin, next - origin);
    }

    const double signed_twice_area = qv::dot(twice_area, normal);
    if (!std::isfinite(signed_twice_area) || std::abs(signed_twice_area) <= minimum_magnitude) {
        FError("portal {} has zero area", portal_index);
    }

    // Every point of a convex winding lies on the same side of every
    // directed edge. Use a distance tolerance so harmless collinear points
    // remain valid while concave and self-intersecting portal outlines do not
    // reach VIS's convex-winding operations.
    const qvec3d winding_normal = signed_twice_area > 0 ? normal : -normal;
    for (size_t edge_index = 0; edge_index < winding.size(); ++edge_index) {
        const qvec3d &edge_start = winding[edge_index];
        const qvec3d edge = winding[(edge_index + 1) % winding.size()] - edge_start;
        const double edge_length = qv::length(edge);

        for (size_t point_index = 0; point_index < winding.size(); ++point_index) {
            const double signed_distance =
                qv::dot(qv::cross(edge, winding[point_index] - edge_start), winding_normal) / edge_length;
            if (!std::isfinite(signed_distance) || signed_distance < -DEFAULT_ON_EPSILON) {
                FError("portal {} is not convex at edge {} (point {} lies outside by {})", portal_index, edge_index,
                    point_index, -signed_distance);
            }
        }
    }
}

prtfile_t LoadPrtFile(const fs::path &name, const bspversion_t *loadversion)
{
    if (loadversion == nullptr || loadversion->game == nullptr) {
        FError("missing BSP version while loading {}", name);
    }

    std::ifstream f(name);
    if (!f) {
        FError("unable to open portal file {}", name);
    }

    /*
     * Parse the portal file header
     */
    std::string magic;
    std::getline(f, magic);
    if (!magic.empty() && magic.back() == '\r') {
        magic.pop_back();
    }
    if (magic.empty()) {
        FError("unknown header/empty portal file {}\n", name);
    }

    prtfile_t result{};
    int numportals = 0;

    if (magic == PORTALFILE) {
        if (!(f >> result.portalleafs >> numportals)) {
            FError("unable to parse {} header\n", PORTALFILE);
        }

        if (loadversion->game->id == GAME_QUAKE_II) {
            // since q2bsp has native cluster support, we shouldn't look at portalleafs_real at all.
            result.portalleafs_real = 0;
        } else {
            result.portalleafs_real = result.portalleafs;
        }
    } else if (magic == PORTALFILE2) {
        if (loadversion->game->id == GAME_QUAKE_II) {
            FError("{} can not be used with Q2\n", PORTALFILE2);
        }
        if (!(f >> result.portalleafs_real >> result.portalleafs >> numportals)) {
            FError("unable to parse {} header\n", PORTALFILE2);
        }
    } else if (magic == PORTALFILEAM) {
        if (loadversion->game->id == GAME_QUAKE_II) {
            FError("{} can not be used with Q2\n", PORTALFILEAM);
        }
        if (!(f >> result.portalleafs >> numportals >> result.portalleafs_real)) {
            FError("unable to parse {} header\n", PORTALFILEAM);
        }
    } else {
        FError("unknown header: {}\n", magic);
    }

    if (result.portalleafs <= 0 || result.portalleafs > PRT_MAX_LEAFS) {
        FError("invalid cluster count {} (expected 1..{})", result.portalleafs, PRT_MAX_LEAFS);
    }
    if (numportals < 0 || numportals > PRT_MAX_PORTALS) {
        FError("invalid portal count {} (expected 0..{})", numportals, PRT_MAX_PORTALS);
    }
    if (loadversion->game->id != GAME_QUAKE_II) {
        if (result.portalleafs_real <= 0 || result.portalleafs_real > PRT_MAX_LEAFS) {
            FError("invalid real leaf count {} (expected 1..{})", result.portalleafs_real, PRT_MAX_LEAFS);
        }
        if (result.portalleafs > result.portalleafs_real) {
            FError("cluster count {} exceeds real leaf count {}", result.portalleafs, result.portalleafs_real);
        }
    }

    for (int i = 0; i < numportals; i++) {
        prtfile_portal_t p{};
        int numpoints = 0;

        if (!(f >> numpoints >> p.leafnums[0] >> p.leafnums[1])) {
            FError("reading portal {}", i);
        }
        if (numpoints < 3 || numpoints > static_cast<int>(PRT_MAX_WINDING)) {
            FError("portal {} has invalid point count {} (expected 3..{})", i, numpoints, PRT_MAX_WINDING);
        }
        if (p.leafnums[0] < 0 || p.leafnums[0] >= result.portalleafs || p.leafnums[1] < 0 ||
            p.leafnums[1] >= result.portalleafs) {
            FError("out of bounds leaf in portal {}", i);
        }
        if (p.leafnums[0] == p.leafnums[1]) {
            FError("portal {} connects cluster {} to itself", i, p.leafnums[0]);
        }

        auto &w = p.winding;
        w.resize(numpoints);

        for (int j = 0; j < numpoints; j++) {
            f >> std::ws;
            char delimiter = '\0';
            if (!f.get(delimiter) || delimiter != '(') {
                FError("reading portal {} point {}: expected '('", i, j);
            }

            if (!(f >> w[j][0] >> w[j][1] >> w[j][2])) {
                FError("reading portal {} point {} coordinates", i, j);
            }
            if (!std::isfinite(w[j][0]) || !std::isfinite(w[j][1]) || !std::isfinite(w[j][2])) {
                FError("portal {} point {} has non-finite coordinates", i, j);
            }

            f >> std::ws;
            if (!f.get(delimiter) || delimiter != ')') {
                FError("reading portal {} point {}: expected ')'", i, j);
            }
        }

        ValidatePortalWinding(w, i);

        result.portals.push_back(std::move(p));
    }

    const auto reject_trailing_data = [&]() {
        std::string trailing;
        if (f >> trailing) {
            FError("unexpected trailing data in portal file: {}", trailing);
        }
    };

    // Q2 doesn't need this, it's PRT1 has the data we need
    if (loadversion->game->id == GAME_QUAKE_II) {
        reject_trailing_data();
        return result;
    }

    if (magic == PORTALFILE) {
        // Quake 1 PRT1 has no explicit cluster map.
        // Assign the identity cluster numbers for consistency
        result.dleafinfos.assign(static_cast<size_t>(result.portalleafs) + 1, prtfile_dleafinfo_t{-1});

        for (int i = 0; i < result.portalleafs; i++) {
            result.dleafinfos[i + 1].cluster = i;
        }
        reject_trailing_data();
        return result;
    }

    if (magic == PORTALFILE2) {
        result.dleafinfos.assign(static_cast<size_t>(result.portalleafs_real) + 1, prtfile_dleafinfo_t{-1});
        std::vector<bool> mapped_leaves(static_cast<size_t>(result.portalleafs_real), false);

        for (int i = 0; i < result.portalleafs; i++) {
            while (true) {
                int leafnum = 0;
                if (!(f >> leafnum)) {
                    FError("couldn't read cluster map for cluster {}", i);
                }
                if (leafnum == -1) {
                    break;
                }
                if (leafnum < 0 || leafnum >= result.portalleafs_real) {
                    FError(
                        "invalid leaf number {} in cluster map (expected 0..{})", leafnum, result.portalleafs_real - 1);
                }
                if (mapped_leaves[leafnum]) {
                    FError("leaf {} occurs more than once in the cluster map", leafnum);
                }
                mapped_leaves[leafnum] = true;
                result.dleafinfos[leafnum + 1].cluster = i;
            }
        }

        const auto missing_leaf = std::find(mapped_leaves.begin(), mapped_leaves.end(), false);
        if (missing_leaf != mapped_leaves.end()) {
            FError("leaf {} is missing from the cluster map", std::distance(mapped_leaves.begin(), missing_leaf));
        }
    } else if (magic == PORTALFILEAM) {
        result.dleafinfos.assign(static_cast<size_t>(result.portalleafs_real) + 1, prtfile_dleafinfo_t{-1});

        for (int i = 0; i < result.portalleafs_real; i++) {
            int clusternum = 0;
            if (!(f >> clusternum)) {
                FError("unexpected end of cluster map at real leaf {}", i);
            }
            if (clusternum < 0 || clusternum >= result.portalleafs) {
                FError("invalid cluster number {} in cluster map, number of clusters: {}\n", clusternum,
                    result.portalleafs);
            }
            result.dleafinfos[i + 1].cluster = clusternum;
        }
    } else {
        FError("Unknown header {}\n", magic);
    }

    reject_trailing_data();
    return result;
}

static void WriteDebugPortal(const polylib::winding_t &w, std::ofstream &portalFile)
{
    ewt::print(portalFile, "{} {} {} ", w.size(), 0, 1);
    for (const qvec3d &point : w) {
        ewt::print(portalFile, "({} {} {}) ", point[0], point[1], point[2]);
    }
    ewt::print(portalFile, "\n");
}

void WriteDebugPortals(const std::vector<polylib::winding_t> &portals, fs::path name)
{
    if (portals.size() > static_cast<size_t>(PRT_MAX_PORTALS)) {
        FError("too many debug portals ({} > {})", portals.size(), PRT_MAX_PORTALS);
    }
    for (size_t i = 0; i < portals.size(); ++i) {
        const auto &portal = portals[i];
        if (portal.size() < 3 || portal.size() > PRT_MAX_WINDING) {
            FError("debug portal {} has invalid point count {} (expected 3..{})", i, portal.size(), PRT_MAX_WINDING);
        }
        for (const qvec3d &point : portal) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) {
                FError("debug portal {} has non-finite coordinates", i);
            }
        }
    }

    std::ofstream portal_file(name, std::ios_base::out);
    if (!portal_file) {
        FError("Failed to open {}: {}", name, strerror(errno));
    }

    ewt::print(portal_file, "PRT1\n");
    ewt::print(portal_file, "{}\n", 2);
    ewt::print(portal_file, "{}\n", portals.size());
    for (const auto &p : portals) {
        WriteDebugPortal(p, portal_file);
    }

    portal_file.close();
    if (!portal_file) {
        FError("Failed to write {}", name);
    }
}

/*
==============================================================================

PORTAL FILE GENERATION

==============================================================================
*/

static void WritePortal(std::ofstream &portalFile, const prtfile_portal_t &portal)
{
    ewt::print(portalFile, "{} {} {} ", portal.winding.size(), portal.leafnums[0], portal.leafnums[1]);

    for (auto &point : portal.winding) {
        ewt::print(portalFile, "({} {} {}) ", point[0], point[1], point[2]);
    }

    ewt::print(portalFile, "\n");
}

static void WritePTR2ClusterMapping(std::ofstream &portalFile, const prtfile_t &input)
{
    // build cluster -> leafs mapping from dleafinfos
    std::map<int, std::vector<int>> cluster_to_leafs;
    for (int leafnum = 0; leafnum < input.portalleafs_real; ++leafnum) {
        int cluster = input.dleafinfos[leafnum + 1].cluster;

        cluster_to_leafs[cluster].push_back(leafnum);
    }

    // print one line per cluster
    for (int i = 0; i < input.portalleafs; i++) {
        auto it = cluster_to_leafs.find(i);
        if (it != cluster_to_leafs.end()) {
            for (int leafnum : it->second) {
                ewt::print(portalFile, "{} ", leafnum);
            }
        }
        ewt::print(portalFile, "-1\n");
    }
}

/*
================
WritePortalfile
================
*/
void WritePortalfile(
    const fs::path &name, const prtfile_t &prtfile, const bspversion_t *loadversion, bool uses_detail, bool forceprt1)
{
    std::ofstream portalFile(name, std::ios_base::out); // .prt files are intentionally text mode
    if (!portalFile)
        FError("Failed to open {}: {}", name, strerror(errno));

    // q2 uses a PRT1 file, but with clusters.
    // (Since q2bsp natively supports clusters, we don't need PRT2.)
    if (loadversion->game->id == GAME_QUAKE_II) {
        ewt::print(portalFile, "PRT1\n");
        ewt::print(portalFile, "{}\n", prtfile.portalleafs);
        ewt::print(portalFile, "{}\n", prtfile.portals.size());
        for (auto &portal : prtfile.portals) {
            WritePortal(portalFile, portal);
        }
    } else if (!uses_detail) {
        /* If no detail clusters, just use a normal PRT1 format */
        ewt::print(portalFile, "PRT1\n");
        ewt::print(portalFile, "{}\n", prtfile.portalleafs);
        ewt::print(portalFile, "{}\n", prtfile.portals.size());

        for (auto &portal : prtfile.portals) {
            WritePortal(portalFile, portal);
        }
    } else if (forceprt1) {
        /* Write a PRT1 file for loading in the map editor. Vis will reject it. */
        ewt::print(portalFile, "PRT1\n");
        ewt::print(portalFile, "{}\n", prtfile.portalleafs);
        ewt::print(portalFile, "{}\n", prtfile.portals.size());

        for (auto &portal : prtfile.portals) {
            WritePortal(portalFile, portal);
        }
    } else {
        /* Write a PRT2 */
        ewt::print(portalFile, "PRT2\n");
        ewt::print(portalFile, "{}\n", prtfile.portalleafs_real);
        ewt::print(portalFile, "{}\n", prtfile.portalleafs);
        ewt::print(portalFile, "{}\n", prtfile.portals.size());

        for (auto &portal : prtfile.portals) {
            WritePortal(portalFile, portal);
        }

        WritePTR2ClusterMapping(portalFile, prtfile);
    }

    portalFile.flush();
    if (!portalFile) {
        FError("Failed to write {}", name);
    }
    portalFile.close();
    if (!portalFile) {
        FError("Failed to close {}", name);
    }
}
