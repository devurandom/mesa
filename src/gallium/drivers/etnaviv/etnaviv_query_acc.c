/*
 * Copyright (c) 2017 Etnaviv Project
 * Copyright (C) 2017 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "etnaviv_context.h"
#include "etnaviv_debug.h"
#include "etnaviv_emit.h"
#include "etnaviv_query_acc.h"
#include "etnaviv_screen.h"

/*
 * Occlusion Query:
 *
 * OCCLUSION_COUNTER and OCCLUSION_PREDICATE differ only in how they
 * interpret results
 */

static void
occlusion_resume(struct etna_acc_query *aq, struct etna_context *ctx)
{
   struct etna_resource *rsc = etna_resource(aq->prsc);
   struct etna_reloc r = {
      .bo = rsc->bo,
      .flags = ETNA_RELOC_WRITE
   };

   if (aq->samples > 63) {
      aq->samples = 63;
      BUG("samples overflow");
   }

   r.offset = aq->samples * 8; /* 64bit value */

   etna_set_state_reloc(ctx->stream, VIVS_GL_OCCLUSION_QUERY_ADDR, &r);
}

static void
occlusion_suspend(struct etna_acc_query *aq, struct etna_context *ctx)
{
   /* 0x1DF5E76 is the value used by blob - but any random value will work */
   etna_set_state(ctx->stream, VIVS_GL_OCCLUSION_QUERY_CONTROL, 0x1DF5E76);
}

static void
occlusion_result(struct etna_acc_query *aq, void *buf,
                 union pipe_query_result *result)
{
   uint64_t sum = 0;
   uint64_t *ptr = (uint64_t *)buf;

   for (unsigned i = 0; i < aq->samples; i++)
      sum += *(ptr + i);

   if (aq->base.type == PIPE_QUERY_OCCLUSION_COUNTER)
      result->u64 = sum;
   else
      result->b = !!sum;
}

static void
etna_acc_destroy_query(struct etna_context *ctx, struct etna_query *q)
{
   struct etna_acc_query *aq = etna_acc_query(q);

   pipe_resource_reference(&aq->prsc, NULL);
   list_del(&aq->node);

   FREE(aq);
}

static const struct etna_acc_sample_provider occlusion_provider = {
   .suspend = occlusion_suspend,
   .resume = occlusion_resume,
   .result = occlusion_result,
};

static void
realloc_query_bo(struct etna_context *ctx, struct etna_acc_query *aq)
{
   struct etna_resource *rsc;
   void *map;

   pipe_resource_reference(&aq->prsc, NULL);

   /* allocate resource with space for 64 * 64bit values */
   aq->prsc = pipe_buffer_create(&ctx->screen->base, PIPE_BIND_QUERY_BUFFER,
                                 0, 0x1000);

   /* don't assume the buffer is zero-initialized */
   rsc = etna_resource(aq->prsc);

   etna_bo_cpu_prep(rsc->bo, DRM_ETNA_PREP_WRITE);

   map = etna_bo_map(rsc->bo);
   memset(map, 0, 0x1000);
   etna_bo_cpu_fini(rsc->bo);
}

static bool
etna_acc_begin_query(struct etna_context *ctx, struct etna_query *q)
{
   struct etna_acc_query *aq = etna_acc_query(q);
   const struct etna_acc_sample_provider *p = aq->provider;

   /* ->begin_query() discards previous results, so realloc bo */
   realloc_query_bo(ctx, aq);

   p->resume(aq, ctx);
   aq->samples++;

   /* add to active list */
   assert(list_is_empty(&aq->node));
   list_addtail(&aq->node, &ctx->active_acc_queries);

   return true;
}

static void
etna_acc_end_query(struct etna_context *ctx, struct etna_query *q)
{
   struct etna_acc_query *aq = etna_acc_query(q);
   const struct etna_acc_sample_provider *p = aq->provider;

   p->suspend(aq, ctx);
   aq->samples++;

   /* remove from active list */
   list_delinit(&aq->node);
}

static bool
etna_acc_get_query_result(struct etna_context *ctx, struct etna_query *q,
                          bool wait, union pipe_query_result *result)
{
   struct etna_acc_query *aq = etna_acc_query(q);
   struct etna_resource *rsc = etna_resource(aq->prsc);
   const struct etna_acc_sample_provider *p = aq->provider;

   assert(list_is_empty(&aq->node));

   if (!wait) {
      int ret;

      if (rsc->status & ETNA_PENDING_WRITE) {
         /* piglit spec@arb_occlusion_query@occlusion_query_conform
          * test, and silly apps perhaps, get stuck in a loop trying
          * to get query result forever with wait==false..  we don't
          * wait to flush unnecessarily but we also don't want to
          * spin forever.
          */
         if (aq->no_wait_cnt++ > 5)
            ctx->base.flush(&ctx->base, NULL, 0);
         return false;
      }

      ret = etna_bo_cpu_prep(rsc->bo, DRM_ETNA_PREP_READ | DRM_ETNA_PREP_NOSYNC);
      if (ret)
         return false;

      etna_bo_cpu_fini(rsc->bo);
   }

   /* flush that GPU executes all query related actions */
   ctx->base.flush(&ctx->base, NULL, 0);

   /* get the result */
   etna_bo_cpu_prep(rsc->bo, DRM_ETNA_PREP_READ);

   void *ptr = etna_bo_map(rsc->bo);
   p->result(aq, ptr, result);
   aq->samples = 0;

   etna_bo_cpu_fini(rsc->bo);

   return true;
}

static const struct etna_query_funcs acc_query_funcs = {
   .destroy_query = etna_acc_destroy_query,
   .begin_query = etna_acc_begin_query,
   .end_query = etna_acc_end_query,
   .get_query_result = etna_acc_get_query_result,
};

static inline const struct etna_acc_sample_provider *
query_sample_provider(unsigned query_type)
{
   switch (query_type) {
   case PIPE_QUERY_OCCLUSION_COUNTER:
      /* fallthrough */
   case PIPE_QUERY_OCCLUSION_PREDICATE:
      /* fallthrough */
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
      return &occlusion_provider;
   default:
      return NULL;
   }
}

struct etna_query *
etna_acc_create_query(struct etna_context *ctx, unsigned query_type)
{
   struct etna_acc_query *aq;
   struct etna_query *q;
   const struct etna_acc_sample_provider *p;

   p = query_sample_provider(query_type);
   if (!p)
      return NULL;

   aq = CALLOC_STRUCT(etna_acc_query);
   if (!aq)
      return NULL;

   aq->provider = p;

   list_inithead(&aq->node);

   q = &aq->base;
   q->funcs = &acc_query_funcs;
   q->type = query_type;

   return q;
}