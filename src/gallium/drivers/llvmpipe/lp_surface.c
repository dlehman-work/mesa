/**************************************************************************
 * 
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

#include "util/u_rect.h"
#include "util/u_surface.h"
#include "lp_context.h"
#include "lp_flush.h"
#include "lp_limits.h"
#include "lp_surface.h"
#include "lp_texture.h"
#include "lp_query.h"


static void
lp_resource_copy(struct pipe_context *pipe,
                 struct pipe_resource *dst, unsigned dst_level,
                 unsigned dstx, unsigned dsty, unsigned dstz,
                 struct pipe_resource *src, unsigned src_level,
                 const struct pipe_box *src_box)
{
   llvmpipe_flush_resource(pipe,
                           dst, dst_level,
                           FALSE, /* read_only */
                           TRUE, /* cpu_access */
                           FALSE, /* do_not_block */
                           "blit dest");

   llvmpipe_flush_resource(pipe,
                           src, src_level,
                           TRUE, /* read_only */
                           TRUE, /* cpu_access */
                           FALSE, /* do_not_block */
                           "blit src");

   util_resource_copy_region(pipe, dst, dst_level, dstx, dsty, dstz,
                             src, src_level, src_box);
}

static void lp_resolve(struct pipe_context *pipe,
                       struct pipe_blit_info *info)
{
    int i;
    uint8_t *map;
    struct pipe_box box;
    struct pipe_transfer *transfer;

    box = info->dst.box;
    map = pipe->transfer_map(pipe, info->dst.resource, 0, LP_TEX_USAGE_READ_WRITE, &box, &transfer);
    printf("%s: UNIMPLEMENTED %d -> %d map %p\n", __FUNCTION__,
            info->src.resource->nr_samples,
            info->dst.resource->nr_samples, map);

    /* NOTE: each pixel is uint32_t because R8G8B8A8 */
    for (i = 0; i < box.width * box.height * box.depth; i++)
        ((uint32_t *)map)[i] = 0xff00ff00;

    pipe->transfer_unmap(pipe, transfer);
}

static void lp_blit(struct pipe_context *pipe,
                    const struct pipe_blit_info *blit_info)
{
   struct llvmpipe_context *lp = llvmpipe_context(pipe);
   struct pipe_blit_info info = *blit_info;

   if (blit_info->render_condition_enable && !llvmpipe_check_render_cond(lp))
      return;

   if (info.src.resource->nr_samples > 1 &&
       info.dst.resource->nr_samples <= 1 &&
       !util_format_is_depth_or_stencil(info.src.resource->format) &&
       !util_format_is_pure_integer(info.src.resource->format)) {
      lp_resolve(pipe, &info);
      return;
   }

   if (util_try_blit_via_copy_region(pipe, &info)) {
      return; /* done */
   }

   if (!util_blitter_is_blit_supported(lp->blitter, &info)) {
      debug_printf("llvmpipe: blit unsupported %s -> %s\n",
                   util_format_short_name(info.src.resource->format),
                   util_format_short_name(info.dst.resource->format));
      return;
   }

   /* XXX turn off occlusion and streamout queries */

   util_blitter_save_vertex_buffer_slot(lp->blitter, lp->vertex_buffer);
   util_blitter_save_vertex_elements(lp->blitter, (void*)lp->velems);
   util_blitter_save_vertex_shader(lp->blitter, (void*)lp->vs);
   util_blitter_save_geometry_shader(lp->blitter, (void*)lp->gs);
   util_blitter_save_so_targets(lp->blitter, lp->num_so_targets,
                                (struct pipe_stream_output_target**)lp->so_targets);
   util_blitter_save_rasterizer(lp->blitter, (void*)lp->rasterizer);
   util_blitter_save_viewport(lp->blitter, &lp->viewports[0]);
   util_blitter_save_scissor(lp->blitter, &lp->scissors[0]);
   util_blitter_save_fragment_shader(lp->blitter, lp->fs);
   util_blitter_save_blend(lp->blitter, (void*)lp->blend);
   util_blitter_save_depth_stencil_alpha(lp->blitter, (void*)lp->depth_stencil);
   util_blitter_save_stencil_ref(lp->blitter, &lp->stencil_ref);
   /*util_blitter_save_sample_mask(sp->blitter, lp->sample_mask);*/
   util_blitter_save_framebuffer(lp->blitter, &lp->framebuffer);
   util_blitter_save_fragment_sampler_states(lp->blitter,
                     lp->num_samplers[PIPE_SHADER_FRAGMENT],
                     (void**)lp->samplers[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_fragment_sampler_views(lp->blitter,
                     lp->num_sampler_views[PIPE_SHADER_FRAGMENT],
                     lp->sampler_views[PIPE_SHADER_FRAGMENT]);
   util_blitter_save_render_condition(lp->blitter, lp->render_cond_query,
                                      lp->render_cond_cond, lp->render_cond_mode);
   util_blitter_blit(lp->blitter, &info);
}


static void
lp_flush_resource(struct pipe_context *ctx, struct pipe_resource *resource)
{
}


static struct pipe_surface *
llvmpipe_create_surface(struct pipe_context *pipe,
                        struct pipe_resource *pt,
                        const struct pipe_surface *surf_tmpl)
{
   struct pipe_surface *ps;

   if (!(pt->bind & (PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET))) {
      debug_printf("Illegal surface creation without bind flag\n");
      if (util_format_is_depth_or_stencil(surf_tmpl->format)) {
         pt->bind |= PIPE_BIND_DEPTH_STENCIL;
      }
      else {
         pt->bind |= PIPE_BIND_RENDER_TARGET;
      }
   }

   ps = CALLOC_STRUCT(pipe_surface);
   if (ps) {
      pipe_reference_init(&ps->reference, 1);
      pipe_resource_reference(&ps->texture, pt);
      ps->context = pipe;
      ps->format = surf_tmpl->format;
      if (llvmpipe_resource_is_texture(pt)) {
         assert(surf_tmpl->u.tex.level <= pt->last_level);
         assert(surf_tmpl->u.tex.first_layer <= surf_tmpl->u.tex.last_layer);
         ps->width = u_minify(pt->width0, surf_tmpl->u.tex.level);
         ps->height = u_minify(pt->height0, surf_tmpl->u.tex.level);
         ps->u.tex.level = surf_tmpl->u.tex.level;
         ps->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
         ps->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
      }
      else {
         /* setting width as number of elements should get us correct renderbuffer width */
         ps->width = surf_tmpl->u.buf.last_element - surf_tmpl->u.buf.first_element + 1;
         ps->height = pt->height0;
         ps->u.buf.first_element = surf_tmpl->u.buf.first_element;
         ps->u.buf.last_element = surf_tmpl->u.buf.last_element;
         assert(ps->u.buf.first_element <= ps->u.buf.last_element);
         assert(util_format_get_blocksize(surf_tmpl->format) *
                (ps->u.buf.last_element + 1) <= pt->width0);
      }
   }
   return ps;
}


static void
llvmpipe_surface_destroy(struct pipe_context *pipe,
                         struct pipe_surface *surf)
{
   /* Effectively do the texture_update work here - if texture images
    * needed post-processing to put them into hardware layout, this is
    * where it would happen.  For llvmpipe, nothing to do.
    */
   assert(surf->texture);
   pipe_resource_reference(&surf->texture, NULL);
   FREE(surf);
}


static void
llvmpipe_clear_render_target(struct pipe_context *pipe,
                             struct pipe_surface *dst,
                             const union pipe_color_union *color,
                             unsigned dstx, unsigned dsty,
                             unsigned width, unsigned height,
                             bool render_condition_enabled)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (render_condition_enabled && !llvmpipe_check_render_cond(llvmpipe))
      return;

   util_clear_render_target(pipe, dst, color,
                            dstx, dsty, width, height);
}


static void
llvmpipe_clear_depth_stencil(struct pipe_context *pipe,
                             struct pipe_surface *dst,
                             unsigned clear_flags,
                             double depth,
                             unsigned stencil,
                             unsigned dstx, unsigned dsty,
                             unsigned width, unsigned height,
                             bool render_condition_enabled)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (render_condition_enabled && !llvmpipe_check_render_cond(llvmpipe))
      return;

   util_clear_depth_stencil(pipe, dst, clear_flags,
                            depth, stencil,
                            dstx, dsty, width, height);
}

/* taken from nv50 driver */
void lp_get_sample_position(struct pipe_context *context,
                               unsigned sample_count,
                               unsigned sample_index,
                               float *out_value)
{
   static const uint8_t ms1[1][2] = { { 0x8, 0x8 } };
   static const uint8_t ms2[2][2] = {
      { 0x4, 0x4 }, { 0xc, 0xc } }; /* surface coords (0,0), (1,0) */
   static const uint8_t ms4[4][2] = {
      { 0x6, 0x2 }, { 0xe, 0x6 },   /* (0,0), (1,0) */
      { 0x2, 0xa }, { 0xa, 0xe } }; /* (0,1), (1,1) */
   static const uint8_t ms8[8][2] = {
      { 0x1, 0x7 }, { 0x5, 0x3 },   /* (0,0), (1,0) */
      { 0x3, 0xd }, { 0x7, 0xb },   /* (0,1), (1,1) */
      { 0x9, 0x5 }, { 0xf, 0x1 },   /* (2,0), (3,0) */
      { 0xb, 0xf }, { 0xd, 0x9 } }; /* (2,1), (3,1) */
   const uint8_t (*ptr)[2];

   switch (sample_count) {
   case 0:
   case 1: ptr = ms1; break;
   case 2: ptr = ms2; break;
   case 4: ptr = ms4; break;
   case 8: ptr = ms8; break;
   default:
      assert(0);
      return; /* bad sample count -> undefined locations */
   }
   out_value[0] = ptr[sample_index][0] * 0.0625f;
   out_value[1] = ptr[sample_index][1] * 0.0625f;
}

void
llvmpipe_init_surface_functions(struct llvmpipe_context *lp)
{
   lp->pipe.clear_render_target = llvmpipe_clear_render_target;
   lp->pipe.clear_depth_stencil = llvmpipe_clear_depth_stencil;
   lp->pipe.create_surface = llvmpipe_create_surface;
   lp->pipe.surface_destroy = llvmpipe_surface_destroy;
   /* These are not actually functions dealing with surfaces */
   lp->pipe.clear_texture = util_clear_texture;
   lp->pipe.resource_copy_region = lp_resource_copy;
   lp->pipe.blit = lp_blit;
   lp->pipe.flush_resource = lp_flush_resource;
   lp->pipe.get_sample_position = lp_get_sample_position;
}
