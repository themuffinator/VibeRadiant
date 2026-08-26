#include <vis/vis.hh>

#include <vis/leafbits.hh>
#include <common/log.hh>
#include <common/bsputils.hh>
#include <common/fs.hh>
#include <common/parallel.hh>

#include <climits>
#include <cmath>
#include <cstdint>
#include <bit> // for std::countr_zero
#include <functional>
#include <numeric> // for std::accumulate
#include <type_traits>

#include <fmt/chrono.h>

/*
 * If the portal file is "PRT2" format, then the leafs we are dealing with are
 * really clusters of leaves. So, after the vis job is done we need to expand
 * the clusters to the real leaf numbers before writing back to the bsp file.
 */
int numportals;
int portalleafs; /* leafs (PRT1) or clusters (PRT2) */
int portalleafs_real; /* real no. of leafs after expanding PRT2 clusters. Not used for Q2. */

std::vector<visportal_t> portals; // always numportals * 2; front and back
std::vector<leaf_t> leafs;

static std::vector<uint8_t> vismap;

uint32_t originalvismapsize;

std::vector<uint8_t> uncompressed;

int leafbytes; // (portalleafs+63)>>3
int leaflongs;
int leafbytes_real; // (portalleafs_real+63)>>3, not used for Q2.

namespace vis
{
std::vector<surfflags_t> extended_texinfo_flags;
}

namespace settings
{
setting_group vis_output_group{"Output", 200, expected_source::commandline};
setting_group vis_advanced_group{"Advanced", 300, expected_source::commandline};

void vis_settings::initialize(int argc, const char **argv)
{
    try {
        common_settings::initialize(argc - 1, argv + 1);

        if (remainder.empty() || remainder.size() > 2) {
            throw parse_exception("expected one or two positional arguments");
        }

        sourceMap = DefaultExtension(remainder[0], "bsp");
    } catch (parse_exception &ex) {
        print_help(false);
        logging::print("ERROR OCCURRED WHEN TRYING TO PARSE ARGUMENTS:\n");
        logging::print(ex.what());
        logging::print("\n\n");
        throw settings::quit_after_help_exception(1);
    }
}
} // namespace settings

settings::vis_settings vis_options;

fs::path portalfile, statefile, statetmpfile;
uint64_t portal_topology_digest = 0;

namespace
{
void DigestByte(uint64_t &digest, uint8_t value)
{
    constexpr uint64_t FNV_PRIME = 1099511628211ull;
    digest ^= value;
    digest *= FNV_PRIME;
}

template<typename T>
void DigestInteger(uint64_t &digest, T value)
{
    using unsigned_t = std::make_unsigned_t<T>;
    unsigned_t bits = static_cast<unsigned_t>(value);
    for (size_t i = 0; i < sizeof(bits); ++i) {
        DigestByte(digest, static_cast<uint8_t>(bits & 0xffu));
        bits >>= 8;
    }
}

} // namespace

uint64_t prt_topology_digest(const prtfile_t &prtfile)
{
    uint64_t digest = 14695981039346656037ull;
    DigestInteger(digest, static_cast<int32_t>(prtfile.portalleafs));
    DigestInteger(digest, static_cast<int32_t>(prtfile.portalleafs_real));
    DigestInteger(digest, static_cast<uint64_t>(prtfile.portals.size()));
    for (const prtfile_portal_t &portal : prtfile.portals) {
        DigestInteger(digest, static_cast<int32_t>(portal.leafnums[0]));
        DigestInteger(digest, static_cast<int32_t>(portal.leafnums[1]));
        DigestInteger(digest, static_cast<uint64_t>(portal.winding.size()));
        for (const qvec3d &point : portal.winding) {
            for (size_t component = 0; component < 3; ++component) {
                DigestInteger(digest, std::bit_cast<uint64_t>(point[component]));
            }
        }
    }
    DigestInteger(digest, static_cast<uint64_t>(prtfile.dleafinfos.size()));
    for (const prtfile_dleafinfo_t &leaf : prtfile.dleafinfos) {
        DigestInteger(digest, static_cast<int32_t>(leaf.cluster));
    }
    return digest;
}

/*
  ==================
  AllocStackWinding

  Return a pointer to a free fixed winding on the stack
  The memory is not initialized.
  ==================
*/
viswinding_t *AllocStackWinding(pstack_t &stack)
{
    for (size_t i = 0; i < STACK_WINDINGS; i++) {
        if (!stack.windings_used[i]) {
            stack.windings_used[i] = true;
            return &stack.windings[i];
        }
    }

    FError("failed");
}

/*
  ==================
  FreeStackWinding

  As long as the winding passed in is local to the stack, free it. Otherwise,
  do nothing (the winding either belongs to a portal or another stack
  structure further up the call chain).
  ==================
*/
void FreeStackWinding(viswinding_t *&w, pstack_t &stack)
{
    if (!w) {
        return;
    }

    viswinding_t *const begin = stack.windings;
    viswinding_t *const end = begin + STACK_WINDINGS;
    const std::less<viswinding_t *> less;

    // std::less provides a strict total order for unrelated pointers. Built-in
    // relational comparisons do not, and the end pointer is not an element.
    if (!less(w, begin) && less(w, end)) {
        size_t i = w - stack.windings;
        if (!stack.windings_used[i])
            FError("winding already freed");
        stack.windings_used[i] = false;
        w = nullptr;
    }
}

/*
  ==================
  ClipStackWinding

  Clips the winding to the plane, returning the new winding on the positive
  side. Frees the input winding (if on stack). If the resulting winding would
  have too many points, the clip operation is aborted and the original winding
  is returned.
  ==================
*/
viswinding_t *ClipStackWinding(visstats_t &stats, viswinding_t *in, pstack_t &stack, const qplane3d &split)
{
    double dists[MAX_WINDING + 1];
    int sides[MAX_WINDING + 1];
    size_t i;

    /* Fast test first */
    double dot = split.distance_to(in->origin);
    if (dot < -in->radius) {
        FreeStackWinding(in, stack);
        return nullptr;
    } else if (dot > in->radius) {
        return in;
    }

    if (in->size() > MAX_WINDING)
        FError("in->numpoints > MAX_WINDING ({} > {})", in->size(), MAX_WINDING);

    int counts[3] = {0, 0, 0};

    /* determine sides for each point */
    for (i = 0; i < in->size(); i++) {
        dot = split.distance_to((*in)[i]);
        dists[i] = dot;
        if (dot > VIS_ON_EPSILON)
            sides[i] = SIDE_FRONT;
        else if (dot < -VIS_ON_EPSILON)
            sides[i] = SIDE_BACK;
        else {
            sides[i] = SIDE_ON;
        }
        counts[sides[i]]++;
    }
    sides[i] = sides[0];
    dists[i] = dists[0];

    // ericw -- coplanar portals: return without clipping. Otherwise when two portals are less than ON_EPSILON apart,
    // one will get fully clipped away and we can't see through it causing
    // https://github.com/ericwa/ericw-tools/issues/261
    if (counts[SIDE_ON] == in->size()) {
        return in;
    }

    if (!counts[0]) {
        FreeStackWinding(in, stack);
        return nullptr;
    }
    if (!counts[1])
        return in;

    auto *neww = AllocStackWinding(stack);
    neww->numpoints = 0;
    neww->origin = in->origin;
    neww->radius = in->radius;

    for (i = 0; i < in->size(); i++) {
        const qvec3d &p1 = (*in)[i];

        if (sides[i] == SIDE_ON) {
            if (neww->size() == MAX_WINDING_FIXED)
                goto noclip;
            neww->push_back(p1);
            continue;
        }

        if (sides[i] == SIDE_FRONT) {
            if (neww->size() == MAX_WINDING_FIXED)
                goto noclip;
            neww->push_back(p1);
        }

        if (sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i])
            continue;

        /* generate a split point */
        const qvec3d &p2 = (*in)[(i + 1) % in->size()];
        qvec3d mid;
        double fraction = dists[i] / (dists[i] - dists[i + 1]);
        for (size_t j = 0; j < 3; j++) {
            /* avoid round off error when possible */
            if (split.normal[j] == 1)
                mid[j] = split.dist;
            else if (split.normal[j] == -1)
                mid[j] = -split.dist;
            else
                mid[j] = p1[j] + fraction * (p2[j] - p1[j]);
        }

        if (neww->size() == MAX_WINDING_FIXED)
            goto noclip;

        neww->push_back(mid);
    }

    FreeStackWinding(in, stack);
    return neww;

noclip:
    FreeStackWinding(neww, stack);
    stats.c_noclip++;
    return in;
}

//============================================================================

#include <mutex>
#include <shared_mutex>

static std::shared_mutex portal_mutex;
static constexpr size_t INVALID_HEAP_POSITION = std::numeric_limits<size_t>::max();
static std::vector<size_t> portal_heap;
static std::vector<size_t> portal_heap_positions;

size_t CheckedPortalIndex(const visportal_t *portal)
{
    if (portal == nullptr || portals.empty()) {
        FError("invalid portal reference");
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(portals.data());
    const uintptr_t address = reinterpret_cast<uintptr_t>(portal);
    if (portals.size() > (std::numeric_limits<uintptr_t>::max() - base) / sizeof(visportal_t)) {
        FError("portal storage exceeds the addressable range");
    }
    const uintptr_t end = base + portals.size() * sizeof(visportal_t);
    if (address < base || address >= end || (address - base) % sizeof(visportal_t) != 0) {
        FError("invalid portal reference");
    }
    return static_cast<size_t>((address - base) / sizeof(visportal_t));
}

portal_visibility_intersection_t IntersectPortalVisibility(const visportal_t &portal,
    const leafbits_t &previous_mightsee, const leafbits_t &already_visible, leafbits_t &intersection)
{
    if (portalleafs < 0 || previous_mightsee.size() != static_cast<size_t>(portalleafs) ||
        already_visible.size() != static_cast<size_t>(portalleafs) ||
        intersection.size() != static_cast<size_t>(portalleafs)) {
        FError("portal visibility intersection has inconsistent dimensions");
    }

    std::shared_lock lock(portal_mutex);
    if (portal.status != pstat_none && portal.status != pstat_working && portal.status != pstat_done) {
        FError("portal has invalid visibility status {}", static_cast<int>(portal.status));
    }
    const bool exact = portal.status == pstat_done;
    const leafbits_t &source = exact ? portal.visbits : portal.mightsee;
    if (source.size() != static_cast<size_t>(portalleafs)) {
        FError("portal visibility source has an inconsistent size");
    }

    uint32_t more = 0;
    const size_t numblocks = (static_cast<size_t>(portalleafs) + leafbits_t::mask) >> leafbits_t::shift;
    for (size_t block = 0; block < numblocks; ++block) {
        intersection.data()[block] = previous_mightsee.data()[block] & source.data()[block];
        more |= intersection.data()[block] & ~already_visible.data()[block];
    }
    return {more, exact};
}

static bool PortalPriorityLess(size_t lhs, size_t rhs)
{
    const visportal_t &left = portals[lhs];
    const visportal_t &right = portals[rhs];
    return left.nummightsee < right.nummightsee || (left.nummightsee == right.nummightsee && lhs < rhs);
}

static void PortalHeapSwap(size_t lhs, size_t rhs)
{
    std::swap(portal_heap[lhs], portal_heap[rhs]);
    portal_heap_positions[portal_heap[lhs]] = lhs;
    portal_heap_positions[portal_heap[rhs]] = rhs;
}

static void PortalHeapSiftUp(size_t position)
{
    while (position) {
        const size_t parent = (position - 1) / 2;
        if (!PortalPriorityLess(portal_heap[position], portal_heap[parent])) {
            break;
        }
        PortalHeapSwap(position, parent);
        position = parent;
    }
}

static void PortalHeapSiftDown(size_t position)
{
    while (position < portal_heap.size()) {
        const size_t left = position * 2 + 1;
        if (left >= portal_heap.size()) {
            break;
        }
        const size_t right = left + 1;
        size_t best = left;
        if (right < portal_heap.size() && PortalPriorityLess(portal_heap[right], portal_heap[left])) {
            best = right;
        }
        if (!PortalPriorityLess(portal_heap[best], portal_heap[position])) {
            break;
        }
        PortalHeapSwap(position, best);
        position = best;
    }
}

static void InitializePortalHeap()
{
    portal_heap.clear();
    portal_heap.reserve(portals.size());
    portal_heap_positions.assign(portals.size(), INVALID_HEAP_POSITION);

    for (size_t i = 0; i < portals.size(); ++i) {
        if (portals[i].status != pstat_none) {
            continue;
        }
        if (portals[i].nummightsee < 0) {
            FError("portal {} has an invalid might-see count {}", i, portals[i].nummightsee);
        }
        portal_heap_positions[i] = portal_heap.size();
        portal_heap.push_back(i);
    }

    for (size_t position = portal_heap.size() / 2; position-- > 0;) {
        PortalHeapSiftDown(position);
    }
}

static void PortalHeapPriorityDecreased(visportal_t *portal)
{
    const size_t portal_index = CheckedPortalIndex(portal);
    if (portal_index >= portal_heap_positions.size()) {
        FError("invalid portal priority update");
    }
    const size_t position = portal_heap_positions[portal_index];
    if (position == INVALID_HEAP_POSITION || position >= portal_heap.size()) {
        FError("portal priority update references a non-pending portal");
    }
    PortalHeapSiftUp(position);
}

/*
  =============
  GetNextPortal

  Returns the next portal for a thread to work on
  Returns the portals from the least complex, so the later ones can reuse
  the earlier information.
  =============
*/
visportal_t *GetNextPortal()
{
    std::scoped_lock lock(portal_mutex);
    if (portal_heap.empty()) {
        return nullptr;
    }

    const size_t portal_index = portal_heap.front();
    const size_t last = portal_heap.size() - 1;
    PortalHeapSwap(0, last);
    portal_heap.pop_back();
    portal_heap_positions[portal_index] = INVALID_HEAP_POSITION;
    if (!portal_heap.empty()) {
        PortalHeapSiftDown(0);
    }

    visportal_t &result = portals[portal_index];
    if (result.status != pstat_none) {
        FError("portal work heap contains a non-pending portal");
    }
    result.status = pstat_working;
    return &result;
}

/*
  =============
  UpdateMightSee

  Called after completing a portal and finding that the source leaf is no
  longer visible from the dest leaf. Visibility is symetrical, so the reverse
  must also be true. Update mightsee for any portals on the source leaf which
  haven't yet started processing.

  Called with the lock held.
  =============
*/
static void UpdateMightsee(visstats_t &stats, const leaf_t &source, const leaf_t &dest)
{
    size_t leafnum = &dest - leafs.data();
    for (visportal_t *p : source.portals) {
        if (p->status != pstat_none) {
            continue;
        }
        if (p->mightsee[leafnum]) {
            p->mightsee[leafnum] = false;
            if (p->nummightsee <= 0) {
                FError("portal might-see count underflow");
            }
            p->nummightsee--;
            PortalHeapPriorityDecreased(p);
            stats.c_mightseeupdate++;
        }
    }
}

/*
  =============
  PortalCompleted

  Mark the portal completed and propogate new vis information across
  to the complementry portals.

  Called with the lock held.
  =============
*/
static void PortalCompleted(visstats_t &stats, visportal_t *completed)
{
    std::scoped_lock lock(portal_mutex);

    completed->status = pstat_done;

    if (completed->leaf < 0 || static_cast<size_t>(completed->leaf) >= leafs.size()) {
        FError("completed portal references invalid leaf {}", completed->leaf);
    }

    /*
     * For each portal on the leaf, check the leafs we eliminated from
     * mightsee during the full vis so far.
     */
    const leaf_t &myleaf = leafs[completed->leaf];
    for (int i = 0; i < myleaf.portals.size(); i++) {
        const visportal_t *p = myleaf.portals[i];
        if (p->status != pstat_done)
            continue;

        auto might = p->mightsee.data();
        auto vis = p->visbits.data();
        const size_t numblocks = (static_cast<size_t>(portalleafs) >> leafbits_t::shift) +
                                 ((static_cast<size_t>(portalleafs) & leafbits_t::mask) != 0);
        for (size_t j = 0; j < numblocks; j++) {
            uint32_t changed = might[j] & ~vis[j];
            if (!changed)
                continue;

            /*
             * If any of these changed bits are still visible from another
             * portal, we can't update yet.
             */
            for (int k = 0; k < myleaf.portals.size(); k++) {
                if (k == i)
                    continue;
                const visportal_t *p2 = myleaf.portals[k];
                if (p2->status == pstat_done)
                    changed &= ~p2->visbits.data()[j];
                else
                    changed &= ~p2->mightsee.data()[j];
                if (!changed)
                    break;
            }

            /*
             * Update mightsee for any of the changed bits that survived
             */
            while (changed) {
                int bit = std::countr_zero(changed);
                changed &= ~nth_bit(bit);
                const size_t leafnum = (j << leafbits_t::shift) + static_cast<size_t>(bit);
                if (leafnum >= leafs.size()) {
                    FError("portal visibility contains an out-of-range padding bit");
                }
                UpdateMightsee(stats, leafs[leafnum], myleaf);
            }
        }
    }
}

qtime_point starttime, endtime, statetime;
static duration stateinterval;

/*
  ==============
  LeafThread
  ==============
*/
static visstats_t LeafThread()
{
    {
        std::scoped_lock lock(portal_mutex);
        /* Save state if sufficient time has elapsed */
        auto now = I_FloatTime();
        if (now > statetime + stateinterval) {
            statetime = now;
            SaveVisState();
        }
    }

    visportal_t *p = GetNextPortal();
    if (!p)
        return {};

    visstats_t stats = PortalFlow(p);

    PortalCompleted(stats, p);

    logging::print(logging::flag::VERBOSE, "portal:{:4}  mightsee:{:4}  cansee:{:4}\n", (ptrdiff_t)(p - portals.data()),
        p->nummightsee, p->numcansee);

    return stats;
}

/*
  ===============
  LeafFlow

  Builds the entire visibility list for a leaf
  ===============
*/
int64_t totalvis;

static std::vector<uint8_t> compressed;

static size_t CheckedVisHeaderSize(const mvis_t &vis)
{
    constexpr size_t fixed_header_size = sizeof(int32_t);
    constexpr size_t cluster_header_size = sizeof(int32_t) * 2;
    if (vis.bit_offsets.size() >
        (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - fixed_header_size) / cluster_header_size) {
        FError("visibility header exceeds the BSP offset limit");
    }
    return fixed_header_size + vis.bit_offsets.size() * cluster_header_size;
}

static uint32_t CheckedOriginalVisMapSize(size_t leaf_count)
{
    if (leaf_count > std::numeric_limits<size_t>::max() - 7) {
        FError("uncompressed visibility row size overflows this platform");
    }
    const size_t row_size = (leaf_count + 7) / 8;
    if (row_size && leaf_count > std::numeric_limits<size_t>::max() / row_size) {
        FError("uncompressed visibility data size overflows this platform");
    }
    const size_t total_size = leaf_count * row_size;
    if (total_size > std::numeric_limits<uint32_t>::max()) {
        FError("uncompressed visibility data is too large");
    }
    return static_cast<uint32_t>(total_size);
}

static int ClusterFlow(int clusternum, leafbits_t &buffer, mbsp_t *bsp)
{
    /*
     * Collect visible bits from all portals into buffer
     */
    leaf_t *leaf = &leafs[clusternum];
    const size_t numblocks = (static_cast<size_t>(portalleafs) >> leafbits_t::shift) +
                             ((static_cast<size_t>(portalleafs) & leafbits_t::mask) != 0);
    for (const visportal_t *p : leaf->portals) {
        if (p->status != pstat_done)
            FError("portal not done");
        for (size_t j = 0; j < numblocks; j++)
            buffer.data()[j] |= p->visbits.data()[j];
    }

    if (buffer[clusternum])
        logging::print("WARNING: Leaf portals saw into cluster ({})\n", clusternum);

    buffer[clusternum] = true;

    /*
     * Now expand the clusters into the full leaf visibility map
     */
    int numvis = 0;

    uint8_t *outbuffer;
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        outbuffer = uncompressed.data() + clusternum * leafbytes;
        for (int i = 0; i < portalleafs; i++) {
            if (buffer[i]) {
                outbuffer[i >> 3] |= nth_bit(i & 7);
                numvis++;
            }
        }
    } else {
        outbuffer = uncompressed.data() + clusternum * leafbytes_real;
        for (int i = 0; i < portalleafs_real; i++) {
            if (buffer[bsp->dleafs[i + 1].cluster]) {
                outbuffer[i >> 3] |= nth_bit(i & 7);
                numvis++;
            }
        }
    }

    /*
     * compress the bit string
     */
    logging::print(logging::flag::VERBOSE, "cluster {:4} : {:4} visible\n", clusternum, numvis);

    /*
     * increment totalvis by
     * (# of real leafs in this cluster) x (# of real leafs visible from this cluster)
     */
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        // FIXME: not sure what this is supposed to be?
        totalvis += numvis;
    } else {
        for (int i = 0; i < portalleafs_real; i++) {
            if (bsp->dleafs[i + 1].cluster == clusternum) {
                totalvis += numvis;
            }
        }
    }

    compressed.clear();

    /* Allocate for worst case where RLE might grow the data (unlikely) */
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        CompressRow(outbuffer, (static_cast<size_t>(portalleafs) + 7) >> 3, std::back_inserter(compressed));
    } else {
        CompressRow(outbuffer, (static_cast<size_t>(portalleafs_real) + 7) >> 3, std::back_inserter(compressed));
    }

    /* leaf 0 is a common solid */
    const size_t vis_header_size = CheckedVisHeaderSize(bsp->dvis);
    if (vismap.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) - vis_header_size ||
        compressed.size() >
            static_cast<size_t>(std::numeric_limits<int32_t>::max()) - vis_header_size - vismap.size()) {
        FError("compressed visibility data exceeds the BSP offset limit");
    }
    const int32_t visofs = static_cast<int32_t>(vismap.size());

    bsp->dvis.set_bit_offset(VIS_PVS, clusternum, visofs);

    // Set pointers
    if (bsp->loadversion->game->id != GAME_QUAKE_II) {
        for (int i = 0; i < portalleafs_real; i++) {
            if (bsp->dleafs[i + 1].cluster == clusternum) {
                bsp->dleafs[i + 1].visofs = visofs;
            }
        }
    }

    std::copy(compressed.begin(), compressed.end(), std::back_inserter(vismap));

    return numvis;
}

visibility_summary_t summarize_visibility(std::span<const int> visible_counts, int possible_count)
{
    visibility_summary_t summary;
    if (visible_counts.empty() || possible_count <= 0) {
        return summary;
    }

    summary.rows = visible_counts.size();
    summary.minimum = visible_counts.front();
    summary.maximum = visible_counts.front();

    double sum = 0;
    for (const int count : visible_counts) {
        summary.minimum = std::min(summary.minimum, count);
        summary.maximum = std::max(summary.maximum, count);
        sum += count;
    }

    summary.mean = sum / static_cast<double>(summary.rows);
    double squared_deviation_sum = 0;
    for (const int count : visible_counts) {
        const double deviation = count - summary.mean;
        squared_deviation_sum += deviation * deviation;
    }
    summary.standard_deviation = std::sqrt(squared_deviation_sum / static_cast<double>(summary.rows));
    summary.percentage = (summary.mean * 100.0) / possible_count;
    return summary;
}

std::vector<int> expand_leaf_visibility_counts(
    std::span<const int> cluster_visible_counts, std::span<const mleaf_t> real_leaves)
{
    std::vector<int> result;
    result.reserve(real_leaves.size());
    for (size_t leafnum = 0; leafnum < real_leaves.size(); ++leafnum) {
        const int cluster = real_leaves[leafnum].cluster;
        if (cluster < 0 || static_cast<size_t>(cluster) >= cluster_visible_counts.size()) {
            FError("real leaf {} references invalid visibility cluster {}", leafnum + 1, cluster);
        }
        result.push_back(cluster_visible_counts[cluster]);
    }
    return result;
}

/*
  ==================
  CalcPortalVis
  ==================
*/
visstats_t CalcPortalVis(const mbsp_t *bsp)
{
    // fastvis just uses mightsee for a very loose bound
    if (vis_options.fast.value()) {
        for (auto &p : portals) {
            p.visbits = p.mightsee;
            p.status = pstat_done;
        }
        return {};
    }

    /*
     * Count the already completed portals in case we loaded previous state
     */
    size_t startcount = 0;
    for (auto &p : portals) {
        if (p.status == pstat_done) {
            startcount++;
        }
    }

    InitializePortalHeap();

    std::vector<visstats_t> stats_perportal;
    stats_perportal.resize(portals.size());

    logging::parallel_for(startcount, portals.size(), [&](size_t i) { stats_perportal[i] = LeafThread(); });

    const visstats_t stats = std::accumulate(stats_perportal.begin(), stats_perportal.end(), visstats_t{});

    SaveVisState();

    logging::print(logging::flag::VERBOSE, "portalcheck: {}  portaltest: {}  portalpass: {}\n", stats.c_portalcheck,
        stats.c_portaltest, stats.c_portalpass);
    logging::print(logging::flag::VERBOSE, "c_vistest: {}  c_mighttest: {}  c_mightseeupdate {}\n", stats.c_vistest,
        stats.c_mighttest, stats.c_mightseeupdate);
    logging::print(logging::flag::VERBOSE, "c_targetcheck: {}\n", stats.c_targetcheck);

    return stats;
}

/*
  ==================
  CalcVis
  ==================
*/
visstats_t CalcVis(mbsp_t *bsp)
{
    if (LoadVisState()) {
        logging::print("Loaded previous state. Resuming progress...\n");
    } else {
        logging::print("Calculating Base Vis:\n");
        BasePortalVis();
    }

    logging::print("Calculating Full Vis:\n");
    auto stats = CalcPortalVis(bsp);

    //
    // assemble the leaf vis lists by oring and compressing the portal lists
    //
    logging::print("Expanding clusters...\n");
    leafbits_t buffer(portalleafs);
    std::vector<int> cluster_visible_counts(static_cast<size_t>(portalleafs));
    for (int i = 0; i < portalleafs; i++) {
        cluster_visible_counts[i] = ClusterFlow(i, buffer, bsp);
        buffer.clear();
    }

    std::vector<int> visible_counts;
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        visible_counts = std::move(cluster_visible_counts);
    } else {
        if (portalleafs_real < 0 || bsp->dleafs.empty() ||
            static_cast<size_t>(portalleafs_real) > bsp->dleafs.size() - 1) {
            FError("portal file real-leaf count {} exceeds BSP leaf count {}", portalleafs_real,
                bsp->dleafs.empty() ? 0 : bsp->dleafs.size() - 1);
        }
        visible_counts = expand_leaf_visibility_counts(cluster_visible_counts,
            std::span<const mleaf_t>(bsp->dleafs).subspan(1, static_cast<size_t>(portalleafs_real)));
    }

    int64_t avg = totalvis;

    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        avg /= static_cast<int64_t>(portalleafs);

        logging::print("average clusters visible: {}\n", avg);
    } else {
        avg /= static_cast<int64_t>(portalleafs_real);

        logging::print("average leafs visible: {}\n", avg);
    }

    const int possible_count = bsp->loadversion->game->id == GAME_QUAKE_II ? portalleafs : portalleafs_real;
    const visibility_summary_t summary = summarize_visibility(visible_counts, possible_count);
    if (summary.rows != static_cast<size_t>(possible_count)) {
        FError("visibility summary has {} rows, expected {}", summary.rows, possible_count);
    }
    logging::print("visibility distribution: min {}, max {}, stddev {:.2f}, mean {:.2f}%\n", summary.minimum,
        summary.maximum, summary.standard_deviation, summary.percentage);

    return stats;
}

// ===========================================================================

#include <fstream>
#include <common/prtfile.hh>

void validate_prt_leaf_mapping_cardinality(const prtfile_t &prtfile, const mbsp_t &bsp)
{
    // Q2 BSPs store cluster membership natively; their PRT1 count is a
    // cluster count rather than a world-model leaf count.
    if (bsp.loadversion->game->id == GAME_QUAKE_II) {
        return;
    }

    if (bsp.dmodels.empty() || bsp.dmodels[0].visleafs < 0) {
        FError("BSP contains an invalid world-model visibility leaf count");
    }

    const size_t expected_real_leafs = static_cast<size_t>(bsp.dmodels[0].visleafs);
    if (static_cast<size_t>(prtfile.portalleafs_real) != expected_real_leafs ||
        prtfile.dleafinfos.size() != expected_real_leafs + 1) {
        FError("portal file describes {} real leaves, but the BSP world model has {}", prtfile.portalleafs_real,
            expected_real_leafs);
    }

    // Inline models may legitimately append more BSP leaves. Only the world
    // model's leaf prefix is represented by the portal file.
    if (bsp.dleafs.empty() || expected_real_leafs > bsp.dleafs.size() - 1) {
        FError("BSP has too few leaf records for its world-model visibility leaf count");
    }
}

/*
  ============
  LoadPortals
  ============
*/
static void LoadPortals(const fs::path &name, mbsp_t *bsp)
{
    const prtfile_t prtfile = LoadPrtFile(name, bsp->loadversion);

    portal_topology_digest = prt_topology_digest(prtfile);

    portalleafs = prtfile.portalleafs;
    portalleafs_real = prtfile.portalleafs_real;

    if (portalleafs <= 0) {
        FError("portal file contains no clusters");
    }
    if (bsp->loadversion->game->id != GAME_QUAKE_II && portalleafs_real <= 0) {
        FError("portal file contains no real leaves");
    }
    validate_prt_leaf_mapping_cardinality(prtfile, *bsp);
    if (prtfile.portals.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 2)) {
        FError("portal file contains too many portals");
    }

    /* Allocate for worst case where RLE might grow the data (unlikely) */
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        compressed.reserve(std::max<size_t>(1, static_cast<size_t>(portalleafs) / 4));
    } else {
        compressed.reserve(std::max<size_t>(1, static_cast<size_t>(portalleafs_real) / 4));
    }

    numportals = static_cast<int>(prtfile.portals.size());

    if (bsp->loadversion->game->id != GAME_QUAKE_II) {
        // since q2bsp has native cluster support, we shouldn't look at portalleafs_real at all.
        logging::print("{:6} leafs\n", portalleafs_real);
    }
    logging::print("{:6} clusters\n", portalleafs);
    logging::print("{:6} portals\n", numportals);

    const auto padded_leaf_bytes = [](int count) -> int {
        const size_t bytes = ((static_cast<size_t>(count) + 63) & ~size_t{63}) >> 3;
        if (bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
            FError("portal file contains too many leaves");
        }
        return static_cast<int>(bytes);
    };

    leafbytes = padded_leaf_bytes(portalleafs);
    leaflongs = leafbytes / sizeof(long);
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        // not used in Q2
        leafbytes_real = 0;
    } else {
        leafbytes_real = padded_leaf_bytes(portalleafs_real);
    }

    // each file portal is split into two memory portals
    portals.resize(numportals * 2);
    leafs.resize(portalleafs);

    const size_t original_leaf_count = bsp->loadversion->game->id == GAME_QUAKE_II
                                           ? static_cast<size_t>(portalleafs)
                                           : static_cast<size_t>(portalleafs_real);
    originalvismapsize = CheckedOriginalVisMapSize(original_leaf_count);

    bsp->dvis.resize(portalleafs);

    if (static_cast<size_t>(originalvismapsize) <=
        std::min(vismap.max_size(), static_cast<size_t>(std::numeric_limits<int32_t>::max())) / 2) {
        vismap.reserve(static_cast<size_t>(originalvismapsize) * 2);
    }

    auto dest_portal_it = portals.begin();

    for (const auto &sourceportal : prtfile.portals) {
        if (sourceportal.leafnums[0] < 0 || sourceportal.leafnums[0] >= portalleafs || sourceportal.leafnums[1] < 0 ||
            sourceportal.leafnums[1] >= portalleafs) {
            FError("portal references an out-of-range cluster");
        }
        if (sourceportal.winding.size() < 3 || sourceportal.winding.size() > MAX_WINDING) {
            FError("portal has invalid winding point count {}", sourceportal.winding.size());
        }

        qplane3d plane;

        {
            auto &p = *dest_portal_it;
            p.winding = viswinding_t::copy_polylib_winding(sourceportal.winding);

            // calc plane
            plane = sourceportal.winding.plane();

            // create forward portal
            auto &l = leafs[sourceportal.leafnums[0]];
            l.portals.push_back(&p);

            p.plane = -plane;
            p.leaf = sourceportal.leafnums[1];
            dest_portal_it++;
        }

        {
            auto &p = *dest_portal_it;
            // create backwards portal
            auto &l = leafs[sourceportal.leafnums[1]];
            l.portals.push_back(&p);

            // Create a reverse winding
            const auto flipped = sourceportal.winding.flip();
            p.winding = viswinding_t::copy_polylib_winding(flipped);

            p.plane = plane;
            p.leaf = sourceportal.leafnums[0];
            dest_portal_it++;
        }
    }

    // Q2 doesn't need this, it's PRT1 has the data we need
    if (bsp->loadversion->game->id == GAME_QUAKE_II) {
        return;
    }

    // Copy cluster mapping from .prt file
    for (int i = 1; i < prtfile.dleafinfos.size(); ++i) {
        if (prtfile.dleafinfos[i].cluster < 0 || prtfile.dleafinfos[i].cluster >= portalleafs) {
            FError("portal leaf {} references out-of-range cluster {}", i, prtfile.dleafinfos[i].cluster);
        }
        bsp->dleafs[i].cluster = prtfile.dleafinfos[i].cluster;
    }
}

void vis_reset()
{
    numportals = 0;
    portalleafs = 0;
    portalleafs_real = 0;

    portals.clear();
    leafs.clear();

    vismap.clear();

    originalvismapsize = 0;

    uncompressed.clear();

    leafbytes = 0;
    leaflongs = 0;
    leafbytes_real = 0;

    vis_options.reset();

    portalfile = fs::path();
    statefile = fs::path();
    statetmpfile = fs::path();
    portal_topology_digest = 0;

    portal_heap.clear();
    portal_heap_positions.clear();

    starttime = {};
    endtime = {};
    statetime = {};

    stateinterval = duration();

    totalvis = 0;
    compressed.clear();

    vis::extended_texinfo_flags.clear();
}

static int vis_main_impl(int argc, const char **argv)
{
    vis_reset();

    bspdata_t bspdata;
    const bspversion_t *loadversion;

    vis_options.preinitialize(argc, argv);
    vis_options.initialize(argc, argv);
    vis_options.postinitialize(argc, argv);

    vis_options.sourceMap.replace_extension("bsp");

    logging::init(fs::path(vis_options.sourceMap)
                      .replace_filename(vis_options.sourceMap.stem().string() + "-vis")
                      .replace_extension("log"),
        vis_options);

    vis_options.print_summary();

    stateinterval = std::chrono::minutes(5); /* 5 minutes */
    starttime = statetime = I_FloatTime();

    LoadBSPFile(vis_options.sourceMap, &bspdata);

    bspdata.version->game->init_filesystem(vis_options.sourceMap, vis_options);

    loadversion = bspdata.version;
    if (!ConvertBSPFormat(&bspdata, &bspver_generic)) {
        FError("couldn't convert {} to the generic BSP representation", vis_options.sourceMap);
    }

    mbsp_t &bsp = std::get<mbsp_t>(bspdata.bsp);

    vis::extended_texinfo_flags = LoadExtendedTexinfoFlags(vis_options.sourceMap, &bsp);

    if (vis_options.phsonly.value()) {
        if (bsp.loadversion->game->id != GAME_QUAKE_II) {
            FError("need a Q2-esque BSP for -phsonly");
        }

        if (bsp.dvis.bit_offsets.empty() ||
            bsp.dvis.bit_offsets.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 63)) {
            FError("BSP contains an invalid visibility cluster count");
        }
        portalleafs = static_cast<int>(bsp.dvis.bit_offsets.size());
        leafbytes = ((portalleafs + 63) & ~63) >> 3;
        leaflongs = leafbytes / sizeof(long);

        originalvismapsize = CheckedOriginalVisMapSize(static_cast<size_t>(portalleafs));
    } else {
        portalfile = fs::path(vis_options.sourceMap).replace_extension("prt");
        LoadPortals(portalfile, &bsp);

        statefile = fs::path(vis_options.sourceMap).replace_extension("vis");
        statetmpfile = fs::path(vis_options.sourceMap).replace_extension("vi0");

        if (bsp.loadversion->game->id != GAME_QUAKE_II) {
            uncompressed.resize(static_cast<size_t>(portalleafs) * static_cast<size_t>(leafbytes_real));
        } else {
            uncompressed.resize(static_cast<size_t>(portalleafs) * static_cast<size_t>(leafbytes));
        }

        auto stats = CalcVis(&bsp);

        logging::print("c_noclip: {}\n", stats.c_noclip);
        logging::print("c_chains: {}\n", stats.c_chains);

        bsp.dvis.bits = std::move(vismap);
        bsp.dvis.bits.shrink_to_fit();
        logging::print("visdatasize:{}  compressed from {}\n", bsp.dvis.bits.size(), originalvismapsize);
    }

    // no ambient sounds for Q2
    if (bsp.loadversion->game->id != GAME_QUAKE_II) {
        CalcAmbientSounds(&bsp);
    } else {
        if (vis_options.phsonly.value()) {
            CalcPHS(&bsp);
        } else {
            CalcPHS(&bsp, uncompressed, static_cast<size_t>(leafbytes));
        }
    }

    /* Convert data format back if necessary */
    if (!ConvertBSPFormat(&bspdata, loadversion)) {
        FError("couldn't convert {} back to {}", vis_options.sourceMap, loadversion->short_name);
    }

    WriteBSPFile(vis_options.sourceMap, &bspdata);

    endtime = I_FloatTime();
    logging::print("{:.2} elapsed\n", (endtime - starttime));

    if (vis_options.autoclean.value()) {
        CleanVisState();
    }

    logging::close();

    return 0;
}

int vis_main(int argc, const char **argv)
{
    const int result = vis_main_impl(argc, argv);
    if (result == 0) {
        // Check only after the implementation has returned, so all workers
        // and local diagnostic/stat objects have finished emitting warnings.
        logging::fail_if_warnings();
    }
    return result;
}

int vis_main(const std::vector<std::string> &args)
{
    std::vector<const char *> argPtrs;
    for (const std::string &arg : args) {
        argPtrs.push_back(arg.data());
    }

    return vis_main(argPtrs.size(), argPtrs.data());
}
