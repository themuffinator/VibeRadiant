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

#include <light/trace.hh>

#include <common/imglib.hh>
#include <common/bsputils.hh>

#include <limits>

/*
==============
Light_PointInLeaf

from hmap2
==============
*/
const mleaf_t *Light_PointInLeaf(const mbsp_t *bsp, const qvec3f &point)
{
    int num = 0;

    while (num >= 0)
        num = bsp->dnodes[num].children[bsp->dplanes[bsp->dnodes[num].planenum].distance_to_fast(point) < 0];

    return &bsp->dleafs[-1 - num];
}

/**
 * Given a float texture coordinate, returns a pixel index to sample in [0, width-1].
 * This assumes the texture repeats and nearest filtering
 */
uint32_t clamp_texcoord(float in, uint32_t width)
{
    if (!width || !std::isfinite(in)) {
        return 0;
    }

    const double wrapped = std::fmod(std::floor(static_cast<double>(in)), static_cast<double>(width));
    return static_cast<uint32_t>(wrapped < 0.0 ? wrapped + width : wrapped);
}

qvec4b SampleTexture(
    const mface_t *face, const mtexinfo_t *tex, const img::texture *texture, const mbsp_t *bsp, const qvec3f &point)
{
    if (tex == nullptr || texture == nullptr || !texture->width || !texture->height) {
        return {};
    }

    const uint64_t pixel_count = static_cast<uint64_t>(texture->width) * static_cast<uint64_t>(texture->height);
    if (pixel_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        texture->pixels.size() < static_cast<size_t>(pixel_count)) {
        return {};
    }

    qvec2d texcoord = WorldToTexCoord(point, tex);

    const uint32_t x = clamp_texcoord(texcoord[0] * texture->width_scale, texture->width);
    const uint32_t y = clamp_texcoord(texcoord[1] * texture->height_scale, texture->height);

    const uint64_t pixel_index = static_cast<uint64_t>(texture->width) * y + x;
    return texture->pixels[static_cast<size_t>(pixel_index)];
}
