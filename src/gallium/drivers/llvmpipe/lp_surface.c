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
   struct pipe_box box = *src_box;

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

   if (src->nr_samples)
       box.depth = src->nr_samples; /* for util_copy_box */
   util_resource_copy_region(pipe, dst, dst_level, dstx, dsty, dstz,
                             src, src_level, &box);
}

static void lp_resolve(struct pipe_context *pipe,
                       struct pipe_blit_info *info)
{
    uint8_t *src_map;
    uint8_t *dst_map;
    struct pipe_box box;
    struct pipe_transfer *src_transfer;
    struct pipe_transfer *dst_transfer;
    struct llvmpipe_resource *src;
    struct llvmpipe_resource *dst;
    unsigned i, j, stride;
    uint8_t *src_maps[LP_MAX_SAMPLES];
    float pix;

    src = llvmpipe_resource(info->src.resource);
    dst = llvmpipe_resource(info->dst.resource);
    stride = llvmpipe_sample_stride(info->src.resource);
    /* TODO: what validation do we need to do here?  how much has been done by callers? */

    box = info->dst.box;
    src_map = pipe->transfer_map(pipe, &src->base, 0, LP_TEX_USAGE_READ, &box, &src_transfer);
    dst_map = pipe->transfer_map(pipe, &dst->base, 0, LP_TEX_USAGE_READ_WRITE, &box, &dst_transfer); /* TODO: WRITE_ALL? */
    
    for (i = 0; i < src->base.nr_samples; i++)
        src_maps[i] = src_map + i * stride;

    for (i = 0; i < box.width * box.height * box.depth * sizeof(uint32_t); i++)
    {
        pix = 0.0f;
        for (j = 0; j < src->base.nr_samples; j++)
            pix += ubyte_to_float(src_maps[j][i]);
        dst_map[i] = float_to_ubyte(pix / src->base.nr_samples);
    }

    pipe->transfer_unmap(pipe, dst_transfer);
    pipe->transfer_unmap(pipe, src_transfer);
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
      // TODO ps->nr_samples = //util_framebuffer_get_num_samples(&setup->fb);
printf("%s: nr_samples %d\n", __FUNCTION__, pt->nr_samples);
      ps->nr_samples = pt->nr_samples; /* TODO other checks? ps->texture->nr_samples? */
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

/* from swr driver */
static const uint8_t get_sample_positions[][2] =
{         { 8, 8},
  /* 1x*/ { 8, 8},
  /* 2x*/ {12,12},{ 4, 4},
  /* 4x*/ { 6, 2},{14, 6},{ 2,10},{10,14},
  /* 8x*/ { 9, 5},{ 7,11},{13, 9},{ 5, 3},
          { 3,13},{ 1, 7},{11,15},{15, 1},
  /*16x*/ { 9, 9},{ 7, 5},{ 5,10},{12, 7},
          { 3, 6},{10,13},{13,11},{11, 3},
          { 6,14},{ 8, 1},{ 4, 2},{ 2,12},
          { 0, 8},{15, 4},{14,15},{ 1, 0}
};

/* taken from nv50 driver */
void lp_get_sample_position(struct pipe_context *context,
                            unsigned sample_count,
                            unsigned sample_index,
                            float *out_value)
{
   /* validate sample_count */
   sample_count = 1 << util_logbase2(sample_count);

   const uint8_t *sample = get_sample_positions[sample_count + sample_index];
   out_value[0] = sample[0] / 16.0f;
   out_value[1] = sample[1] / 16.0f;
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
