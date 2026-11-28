#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Zink over MoltenVK: new pipelines are built on worker threads while the game is showing frames.

MoltenVK has no VK_EXT_graphics_pipeline_library, so Zink builds every new VkPipeline on the draw thread, and on Apple GPUs
that means SPIR-V -> MSL -> a Metal compile: tens of milliseconds each. An engine that brings in new materials as the player
moves (Unreal) then freezes for seconds at a time, more often the more it streams in. Here a pipeline missing while frames
are being presented is queued to worker threads, and the draws that need it are left out until it is ready (a moment of
pop-in instead of a freeze) -- what DXVK's async mode and Dolphin's do. While nothing is presented (a loading screen, which
is when engines do their one-time work such as reflection captures), pipelines are still built at once, so no one-time draw
is lost. Husk's EGL layer publishes the time of the last swap through HUSK_ZINK_SWAP_CLOCK; without it nothing changes.
TL_ZINK_SYNC_PIPELINES=1 keeps the old behaviour.
"""
import sys


def patch(path, old, new, count=1):
    with open(path) as f:
        src = f.read()
    if src.count(old) != count:
        sys.exit(f"{path}: expected {count} match(es) of {old[:60]!r}, found {src.count(old)}")
    with open(path, "w") as f:
        f.write(src.replace(old, new))


Z = "src/gallium/drivers/zink/"

patch(Z + "zink_types.h",
      "   /* GPL only */\n   struct util_queue_fence fence;\n",
      "   /* GPL only */\n   struct util_queue_fence fence;\n"
      "   /* being built on a Husk worker: the pipeline is there once the fence is signalled */\n"
      "   bool husk_async;\n")

patch(Z + "zink_program.h",
      "void\nzink_gfx_program_compile_queue(struct zink_context *ctx, struct zink_gfx_pipeline_cache_entry *pc_entry);\n",
      "void\nzink_gfx_program_compile_queue(struct zink_context *ctx, struct zink_gfx_pipeline_cache_entry *pc_entry);\n"
      "bool\nzink_husk_pipeline_async(struct zink_context *ctx, struct zink_gfx_pipeline_cache_entry *pc_entry, VkPrimitiveTopology vkmode, bool binding_map);\n")

patch(Z + "zink_program.c",
      "void\nzink_program_finish(struct zink_context *ctx, struct zink_program *pg)\n",
      r'''#include <time.h>
#include "util/u_call_once.h"
#include "util/u_atomic.h"

/* everything a pipeline is made from, copied, so the worker reads nothing the context changes or frees meanwhile */
struct husk_async_job {
   struct zink_gfx_pipeline_cache_entry *pc_entry;
   struct zink_screen *screen;
   struct zink_gfx_pipeline_state state;
   struct zink_vertex_elements_hw_state elements;
   struct zink_blend_state blend;
   struct zink_shader_object objs[ZINK_GFX_SHADER_COUNT];
   VkPrimitiveTopology vkmode;
   bool binding_map;
};

static struct util_queue husk_queue;
static bool husk_queue_ok;
static const volatile int64_t *husk_swap_clock;
static unsigned husk_async_count;

static void
husk_async_init(void)
{
   const char *clk = getenv("HUSK_ZINK_SWAP_CLOCK"), *sync = getenv("TL_ZINK_SYNC_PIPELINES");
   if (!clk || !*clk || (sync && *sync == '1'))
      return;
   husk_swap_clock = (const volatile int64_t *)(uintptr_t)strtoull(clk, NULL, 16);
   husk_queue_ok = husk_swap_clock &&
                   util_queue_init(&husk_queue, "zpipe", 64, 3, UTIL_QUEUE_INIT_RESIZE_IF_FULL, NULL);
   fprintf(stderr, "zink: new pipelines %s\n", husk_queue_ok ? "built on worker threads while frames are shown (TL_ZINK_SYNC_PIPELINES=1: on the draw thread)"
                                                            : "built on the draw thread");
}

static void
husk_async_run(void *data, void *gdata, int thread_index)
{
   struct husk_async_job *j = data;
   j->pc_entry->pipeline = zink_create_gfx_pipeline(j->screen, j->pc_entry->prog, j->objs, &j->state,
                                                    j->binding_map ? j->elements.binding_map : NULL, j->vkmode, true);
   if (!j->pc_entry->pipeline)
      fprintf(stderr, "zink: a pipeline built in the background failed; its draws are left out\n");
}

static void
husk_async_free(void *data, void *gdata, int thread_index)
{
   free(data);
}

bool
zink_husk_pipeline_async(struct zink_context *ctx, struct zink_gfx_pipeline_cache_entry *pc_entry, VkPrimitiveTopology vkmode, bool binding_map)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;
   util_call_once(&once, husk_async_init);
   if (!husk_queue_ok || ctx->blitting || ctx->num_so_targets || !pc_entry->state.element_state)
      return false;
   /* frames are being shown: the game is playing, not loading */
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   int64_t now = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec, last = *husk_swap_clock;
   if (!last || now - last > 500000000LL)
      return false;
   struct husk_async_job *j = calloc(1, sizeof(*j));
   if (!j)
      return false;
   j->pc_entry = pc_entry;
   j->screen = zink_screen(ctx->base.screen);
   memcpy(&j->state, &pc_entry->state, sizeof(j->state));
   j->state.rendering_info.pColorAttachmentFormats = j->state.rendering_formats;
   memcpy(&j->elements, pc_entry->state.element_state, sizeof(j->elements));
   j->state.element_state = &j->elements;
   if (pc_entry->state.blend_state) {
      memcpy(&j->blend, pc_entry->state.blend_state, sizeof(j->blend));
      j->state.blend_state = &j->blend;
   }
   memcpy(j->objs, pc_entry->prog->objs, sizeof(j->objs));
   j->vkmode = vkmode;
   j->binding_map = binding_map;
   pc_entry->husk_async = true;
   util_queue_add_job(&husk_queue, j, &pc_entry->fence, husk_async_run, husk_async_free, 0);
   unsigned n = p_atomic_inc_return(&husk_async_count);
   if (n == 1 || n % 250 == 0)
      fprintf(stderr, "zink: %u pipeline(s) built in the background so far\n", n);
   return true;
}

void
zink_program_finish(struct zink_context *ctx, struct zink_program *pg)
''')

H = Z + "zink_program_state.hpp"
patch(H,
      "      } else {\n         /* optimize by default only when expecting precompiles in order to reduce stuttering */\n",
      "      } else if (!HAVE_LIB && state->pipeline &&\n"
      "                 zink_husk_pipeline_async(ctx, pc_entry, vkmode, DYNAMIC_STATE != ZINK_DYNAMIC_VERTEX_INPUT2 && DYNAMIC_STATE != ZINK_DYNAMIC_VERTEX_INPUT)) {\n"
      "         /* built on a worker: until then the draw is left out, and the bound pipeline (state->pipeline) stays as it is */\n"
      "         return VK_NULL_HANDLE;\n"
      "      } else {\n         /* optimize by default only when expecting precompiles in order to reduce stuttering */\n")
patch(H,
      "   struct zink_gfx_pipeline_cache_entry *cache_entry = (struct zink_gfx_pipeline_cache_entry *)entry->data;\n"
      "   state->pipeline = cache_entry->pipeline;\n",
      "   struct zink_gfx_pipeline_cache_entry *cache_entry = (struct zink_gfx_pipeline_cache_entry *)entry->data;\n"
      "   if (unlikely(cache_entry->husk_async)) {\n"
      "      if (!util_queue_fence_is_signalled(&cache_entry->fence))\n"
      "         return VK_NULL_HANDLE;\n"
      "      cache_entry->husk_async = false;\n"
      "      if (cache_entry->pipeline)\n"
      "         zink_screen_update_pipeline_cache(screen, &prog->base, false);\n"
      "   }\n"
      "   /* a pipeline that could not be made: no draw, and state->pipeline kept, so the state hash stays whole */\n"
      "   if (unlikely(!cache_entry->pipeline))\n"
      "      return VK_NULL_HANDLE;\n"
      "   state->pipeline = cache_entry->pipeline;\n")
