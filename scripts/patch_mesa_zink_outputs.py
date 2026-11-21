#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mesa source patches for Zink over MoltenVK, run from the Mesa source root.

1. A fragment shader output whose type does not match its colour attachment (a float written to an integer target, which GL
   leaves undefined) makes Metal refuse the whole pipeline. The attachment is left out of that pipeline instead, so the draw
   still happens and only that undefined write is dropped. Blending, which GL ignores on integer targets, is turned off there.
2. Zink names SPIR-V variables after the GL ones; SPIRV-Cross keeps the names in the MSL it writes, and one that is an MSL type
   (a uniform called "sampler") breaks the shader's compile. The names are debugging aids only, so none are written.
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
      "   /* this is deleted in zink_shader_init */\n   nir_shader *nir;\n",
      "   /* this is deleted in zink_shader_init */\n   nir_shader *nir;\n"
      "   /* fragment shaders: what each colour output is, 0 none, 1 float, 2 signed int, 3 unsigned int */\n"
      "   uint8_t husk_out_kind[PIPE_MAX_COLOR_BUFS];\n")

patch(Z + "zink_compiler.c",
      "      nir_sort_variables_by_location(nir, nir_var_shader_out);\n   }\n",
      "      nir_sort_variables_by_location(nir, nir_var_shader_out);\n   }\n"
      "   if (nir->info.stage == MESA_SHADER_FRAGMENT) {\n"
      "      nir_foreach_shader_out_variable(var, nir) {\n"
      "         if (var->data.index || (var->data.location != FRAG_RESULT_COLOR && var->data.location < FRAG_RESULT_DATA0))\n"
      "            continue;\n"
      "         enum glsl_base_type bt = glsl_get_base_type(glsl_without_array(var->type));\n"
      "         uint8_t kind = glsl_base_type_is_integer(bt) ? (bt == GLSL_TYPE_INT || bt == GLSL_TYPE_INT16 || bt == GLSL_TYPE_INT8 || bt == GLSL_TYPE_INT64 ? 2 : 3) : 1;\n"
      "         unsigned first = var->data.location == FRAG_RESULT_COLOR ? 0 : var->data.location - FRAG_RESULT_DATA0;\n"
      "         unsigned n = var->data.location == FRAG_RESULT_COLOR ? PIPE_MAX_COLOR_BUFS : glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;\n"
      "         for (unsigned i = first; i < first + n && i < PIPE_MAX_COLOR_BUFS; i++)\n"
      "            zs->husk_out_kind[i] = kind;\n"
      "      }\n"
      "   }\n")

patch(Z + "zink_pipeline.c",
      '#include "util/u_debug.h"\n',
      '#include "util/u_debug.h"\n#include "vk_format.h"\n')

patch(Z + "zink_pipeline.c",
      "   pci.pNext = &state->rendering_info;\n   if (needs_vi)\n",
      "   pci.pNext = &state->rendering_info;\n"
      "   VkPipelineRenderingCreateInfo husk_ri;\n"
      "   VkFormat husk_formats[PIPE_MAX_COLOR_BUFS];\n"
      "   VkPipelineColorBlendAttachmentState husk_blend[PIPE_MAX_COLOR_BUFS];\n"
      "   struct zink_shader *husk_fs = prog->shaders[MESA_SHADER_FRAGMENT];\n"
      "   unsigned husk_n = MIN2(state->rendering_info.colorAttachmentCount, PIPE_MAX_COLOR_BUFS);\n"
      "   if (husk_fs && husk_n) {\n"
      "      bool formats_changed = false, blend_changed = false;\n"
      "      if (blend_state.pAttachments)\n"
      "         memcpy(husk_blend, blend_state.pAttachments, husk_n * sizeof(husk_blend[0]));\n"
      "      for (unsigned i = 0; i < husk_n; i++) {\n"
      "         VkFormat fmt = state->rendering_info.pColorAttachmentFormats[i];\n"
      "         husk_formats[i] = fmt;\n"
      "         if (fmt == VK_FORMAT_UNDEFINED)\n"
      "            continue;\n"
      "         uint8_t want = vk_format_is_sint(fmt) ? 2 : vk_format_is_uint(fmt) ? 3 : 1;\n"
      "         uint8_t have = husk_fs->husk_out_kind[i];\n"
      "         if (have && have != want) {\n"
      "            husk_formats[i] = VK_FORMAT_UNDEFINED;\n"
      "            formats_changed = true;\n"
      "         }\n"
      "         if (blend_state.pAttachments && (want != 1 || husk_formats[i] == VK_FORMAT_UNDEFINED)) {\n"
      "            husk_blend[i].blendEnable = VK_FALSE;\n"
      "            if (husk_formats[i] == VK_FORMAT_UNDEFINED)\n"
      "               husk_blend[i].colorWriteMask = 0;\n"
      "            blend_changed = true;\n"
      "         }\n"
      "      }\n"
      "      if (formats_changed) {\n"
      "         husk_ri = state->rendering_info;\n"
      "         husk_ri.pColorAttachmentFormats = husk_formats;\n"
      "         pci.pNext = &husk_ri;\n"
      "      }\n"
      "      if (blend_changed)\n"
      "         blend_state.pAttachments = husk_blend;\n"
      "   }\n"
      "   if (needs_vi)\n")

patch(Z + "nir_to_spirv/spirv_builder.c",
      "                        const char *name)\n{\n   size_t pos = b->debug_names.num_words;\n",
      "                        const char *name)\n{\n   if (b)\n      return;\n   size_t pos = b->debug_names.num_words;\n")

print("zink output/name patches applied")
