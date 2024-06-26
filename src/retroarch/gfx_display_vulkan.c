/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2016-2017 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gfx_display.h"

#include "vulkan_common.h"

 /* Will do Y-flip later, but try to make it similar to GL. */
static const float vk_vertexes[] = {
   0, 0,
   1, 0,
   0, 1,
   1, 1
};

static const float vk_tex_coords[] = {
   0, 1,
   1, 1,
   0, 0,
   1, 0
};

static const float vk_colors[] = {
   1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,
};

static void* gfx_display_vk_get_default_mvp(void* data)
{
    vk_t* vk = (vk_t*)data;
    if (!vk)
        return NULL;
    return &vk->mvp_no_rot;
}

static const float* gfx_display_vk_get_default_vertices(void)
{
    return &vk_vertexes[0];
}

static const float* gfx_display_vk_get_default_tex_coords(void)
{
    return &vk_tex_coords[0];
}

static void gfx_display_vk_draw(gfx_display_ctx_draw_t* draw,
    void* data, unsigned video_width, unsigned video_height)
{
    unsigned i;
    struct vk_buffer_range range;
    struct vk_texture* texture = NULL;
    const float* vertex = NULL;
    const float* tex_coord = NULL;
    const float* color = NULL;
    struct vk_vertex* pv = NULL;
    vk_t* vk = (vk_t*)data;

    if (!vk || !draw)
        return;

    texture = (struct vk_texture*)draw->texture;
    vertex = draw->coords->vertex;
    tex_coord = draw->coords->tex_coord;
    color = draw->coords->color;

    if (!vertex)
        vertex = &vk_vertexes[0];
    if (!tex_coord)
        tex_coord = &vk_tex_coords[0];
    if (!draw->coords->lut_tex_coord)
        draw->coords->lut_tex_coord = &vk_tex_coords[0];
    if (!texture)
        texture = &vk->display.blank_texture;
    if (!color)
        color = &vk_colors[0];

    vk->vk_vp.x = draw->x;
    vk->vk_vp.y = vk->context->swapchain_height - draw->y - draw->height;
    vk->vk_vp.width = draw->width;
    vk->vk_vp.height = draw->height;
    vk->vk_vp.minDepth = 0.0f;
    vk->vk_vp.maxDepth = 1.0f;

    vk->tracker.dirty |= VULKAN_DIRTY_DYNAMIC_BIT;

    /* Bake interleaved VBO. Kinda ugly, we should probably try to move to
     * an interleaved model to begin with ... */
    if (!vulkan_buffer_chain_alloc(vk->context, &vk->chain->vbo,
        draw->coords->vertices * sizeof(struct vk_vertex), &range))
        return;

    pv = (struct vk_vertex*)range.data;
    for (i = 0; i < draw->coords->vertices; i++, pv++)
    {
        pv->x = *vertex++;
        /* Y-flip. Vulkan is top-left clip space */
        pv->y = 1.0f - (*vertex++);
        pv->tex_x = *tex_coord++;
        pv->tex_y = *tex_coord++;
        pv->color.r = *color++;
        pv->color.g = *color++;
        pv->color.b = *color++;
        pv->color.a = *color++;
    }

    switch (draw->pipeline_id)
    {
    default:
    {
        struct vk_draw_triangles call;
        unsigned
            disp_pipeline = ((draw->prim_type ==
                GFX_DISPLAY_PRIM_TRIANGLESTRIP) << 1) |
            (vk->display.blend << 0);
        call.pipeline = vk->display.pipelines[disp_pipeline];
        call.texture = texture;
        call.sampler = texture->mipmap ?
            vk->samplers.mipmap_linear :
            (texture->default_smooth ? vk->samplers.linear
                : vk->samplers.nearest);
        call.uniform = draw->matrix_data
            ? draw->matrix_data : &vk->mvp_no_rot;
        call.uniform_size = sizeof(math_matrix_4x4);
        call.vbo = &range;
        call.vertices = draw->coords->vertices;

        vulkan_draw_triangles(vk, &call);
    }
    break;
    }
}

static void gfx_display_vk_blend_begin(void* data)
{
    vk_t* vk = (vk_t*)data;

    if (vk)
        vk->display.blend = true;
}

static void gfx_display_vk_blend_end(void* data)
{
    vk_t* vk = (vk_t*)data;

    if (vk)
        vk->display.blend = false;
}

static bool gfx_display_vk_font_init_first(
    void** font_handle, void* video_data, const char* font_path,
    float menu_font_size, bool is_threaded)
{
    return false;
}

static void gfx_display_vk_scissor_begin(
    void* data,
    unsigned video_width,
    unsigned video_height,
    int x, int y, unsigned width, unsigned height)
{
    vk_t* vk = (vk_t*)data;

    vk->tracker.use_scissor = true;
    vk->tracker.scissor.offset.x = x;
    vk->tracker.scissor.offset.y = y;
    vk->tracker.scissor.extent.width = width;
    vk->tracker.scissor.extent.height = height;
    vk->tracker.dirty |= VULKAN_DIRTY_DYNAMIC_BIT;
}

static void gfx_display_vk_scissor_end(void* data,
    unsigned video_width,
    unsigned video_height)
{
    vk_t* vk = (vk_t*)data;

    vk->tracker.use_scissor = false;
    vk->tracker.dirty |= VULKAN_DIRTY_DYNAMIC_BIT;
}

gfx_display_ctx_driver_t gfx_display_ctx_vulkan = {
   gfx_display_vk_draw,
#ifdef HAVE_SHADERPIPELINE
   gfx_display_vk_draw_pipeline,
#else
   NULL,                                  /* draw_pipeline */
#endif
   gfx_display_vk_blend_begin,
   gfx_display_vk_blend_end,
   gfx_display_vk_get_default_mvp,
   gfx_display_vk_get_default_vertices,
   gfx_display_vk_get_default_tex_coords,
   gfx_display_vk_font_init_first,
   // GFX_VIDEO_DRIVER_VULKAN,
   "vulkan",
   false,
   gfx_display_vk_scissor_begin,
   gfx_display_vk_scissor_end
};
