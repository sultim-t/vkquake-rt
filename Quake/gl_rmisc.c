/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_misc.c

#include "quakedef.h"
#include <float.h>

cvar_t r_lodbias = {"r_lodbias", "1", CVAR_ARCHIVE};

// johnfitz -- new cvars
extern cvar_t r_clearcolor;
extern cvar_t r_fastclear;
extern cvar_t r_flatlightstyles;
extern cvar_t r_lerplightstyles;
extern cvar_t gl_fullbrights;
extern cvar_t gl_farclip;
extern cvar_t r_waterquality;
extern cvar_t r_waterwarp;
extern cvar_t r_waterwarpcompute;
extern cvar_t r_oldskyleaf;
extern cvar_t r_drawworld;
extern cvar_t r_showtris;
extern cvar_t r_showbboxes;
extern cvar_t r_lerpmodels;
extern cvar_t r_lerpmove;
extern cvar_t r_nolerp_list;
// johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix

extern cvar_t r_gpulightmapupdate;
extern cvar_t r_tasks;
extern cvar_t r_parallelmark;
extern cvar_t r_usesops;

#if defined(USE_SIMD)
extern cvar_t r_simd;
#endif
extern gltexture_t *playertextures[MAX_SCOREBOARD]; // johnfitz

vulkanglobals_t vulkan_globals;

int    num_vulkan_tex_allocations = 0;
int    num_vulkan_bmodel_allocations = 0;
int    num_vulkan_mesh_allocations = 0;
int    num_vulkan_misc_allocations = 0;
int    num_vulkan_dynbuf_allocations = 0;
int    num_vulkan_combined_image_samplers = 0;
int    num_vulkan_ubos_dynamic = 0;
int    num_vulkan_ubos = 0;
int    num_vulkan_storage_buffers = 0;
int    num_vulkan_input_attachments = 0;
int    num_vulkan_storage_images = 0;
size_t total_device_vulkan_allocation_size = 0;
size_t total_host_vulkan_allocation_size = 0;

qboolean use_simd;

/*
====================
GL_Fullbrights_f -- johnfitz
====================
*/
static void GL_Fullbrights_f (cvar_t *var)
{
	TexMgr_ReloadNobrightImages ();
}

/*
====================
SetClearColor
====================
*/
static void SetClearColor ()
{
	byte *rgb;
	int   s;

	if (r_fastclear.value != 0.0f)
	{
		// Set to black so fast clear works properly on modern GPUs
		vec4_t color = {0};
	}
	else
	{
		s = (int)r_clearcolor.value & 0xFF;
		rgb = (byte *)(d_8to24table + s);
		vec4_t color = { rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 0};
	}
}

/*
====================
R_SetClearColor_f -- johnfitz
====================
*/
static void R_SetClearColor_f (cvar_t *var)
{
	if (r_fastclear.value != 0.0f)
		Con_Warning ("Black clear color forced by r_fastclear\n");

	SetClearColor ();
}

/*
====================
R_SetFastClear_f -- johnfitz
====================
*/
static void R_SetFastClear_f (cvar_t *var)
{
	SetClearColor ();
}

/*
===============
R_Model_ExtraFlags_List_f -- johnfitz -- called when r_nolerp_list cvar changes
===============
*/
static void R_Model_ExtraFlags_List_f (cvar_t *var)
{
	int i;
	for (i = 0; i < MAX_MODELS; i++)
		Mod_SetExtraFlags (cl.model_precache[i]);
}

/*
====================
R_SetWateralpha_f -- ericw
====================
*/
static void R_SetWateralpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWWATER) && var->value < 1)
		Con_Warning ("Map does not appear to be water-vised\n");
	map_wateralpha = var->value;
	map_fallbackalpha = var->value;
}

#if defined(USE_SIMD)
/*
====================
R_SIMD_f
====================
*/
static void R_SIMD_f (cvar_t *var)
{
#if defined(USE_SSE2)
	use_simd = SDL_HasSSE () && SDL_HasSSE2 () && (var->value != 0.0f);
#else
#error not implemented
#endif
}
#endif

/*
====================
R_SetLavaalpha_f -- ericw
====================
*/
static void R_SetLavaalpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWLAVA) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be lava-vised\n");
	map_lavaalpha = var->value;
}

/*
====================
R_SetTelealpha_f -- ericw
====================
*/
static void R_SetTelealpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWTELE) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be tele-vised\n");
	map_telealpha = var->value;
}

/*
====================
R_SetSlimealpha_f -- ericw
====================
*/
static void R_SetSlimealpha_f (cvar_t *var)
{
	if (cls.signon == SIGNONS && cl.worldmodel && !(cl.worldmodel->contentstransparent & SURF_DRAWSLIME) && var->value && var->value < 1)
		Con_Warning ("Map does not appear to be slime-vised\n");
	map_slimealpha = var->value;
}

/*
====================
GL_WaterAlphaForSurfface -- ericw
====================
*/
float GL_WaterAlphaForSurface (msurface_t *fa)
{
	if (fa->flags & SURF_DRAWLAVA)
		return map_lavaalpha > 0 ? map_lavaalpha : map_fallbackalpha;
	else if (fa->flags & SURF_DRAWTELE)
		return map_telealpha > 0 ? map_telealpha : map_fallbackalpha;
	else if (fa->flags & SURF_DRAWSLIME)
		return map_slimealpha > 0 ? map_slimealpha : map_fallbackalpha;
	else
		return map_wateralpha;
}

/*
===============
R_InitDefaultStates
===============
*/
static void R_InitDefaultStates (pipeline_create_infos_t *infos)
{
	memset (infos, 0, sizeof (pipeline_create_infos_t));

	infos->dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	infos->dynamic_state.pDynamicStates = infos->dynamic_states;

	infos->shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	infos->shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	infos->shader_stages[0].module = basic_vert_module;
	infos->shader_stages[0].pName = "main";

	infos->shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	infos->shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	infos->shader_stages[1].module = basic_frag_module;
	infos->shader_stages[1].pName = "main";

	infos->vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	infos->vertex_input_state.vertexAttributeDescriptionCount = 3;
	infos->vertex_input_state.pVertexAttributeDescriptions = basic_vertex_input_attribute_descriptions;
	infos->vertex_input_state.vertexBindingDescriptionCount = 1;
	infos->vertex_input_state.pVertexBindingDescriptions = &basic_vertex_binding_description;

	infos->input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	infos->input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	infos->viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	infos->viewport_state.viewportCount = 1;
	infos->dynamic_states[infos->dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
	infos->viewport_state.scissorCount = 1;
	infos->dynamic_states[infos->dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;

	infos->rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	infos->rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	infos->rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
	infos->rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
	infos->rasterization_state.depthClampEnable = VK_FALSE;
	infos->rasterization_state.rasterizerDiscardEnable = VK_FALSE;
	infos->rasterization_state.depthBiasEnable = VK_FALSE;
	infos->rasterization_state.lineWidth = 1.0f;

	infos->multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	infos->multisample_state.rasterizationSamples = vulkan_globals.sample_count;
	if (vulkan_globals.supersampling)
	{
		infos->multisample_state.sampleShadingEnable = VK_TRUE;
		infos->multisample_state.minSampleShading = 1.0f;
	}

	infos->depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	infos->depth_stencil_state.depthTestEnable = VK_FALSE;
	infos->depth_stencil_state.depthWriteEnable = VK_FALSE;
	infos->depth_stencil_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	infos->depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
	infos->depth_stencil_state.back.failOp = VK_STENCIL_OP_KEEP;
	infos->depth_stencil_state.back.passOp = VK_STENCIL_OP_KEEP;
	infos->depth_stencil_state.back.compareOp = VK_COMPARE_OP_ALWAYS;
	infos->depth_stencil_state.stencilTestEnable = VK_FALSE;
	infos->depth_stencil_state.front = infos->depth_stencil_state.back;

	infos->color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	infos->blend_attachment_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	infos->blend_attachment_state.blendEnable = VK_FALSE;
	infos->blend_attachment_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	infos->blend_attachment_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	infos->blend_attachment_state.colorBlendOp = VK_BLEND_OP_ADD;
	infos->blend_attachment_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	infos->blend_attachment_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	infos->blend_attachment_state.alphaBlendOp = VK_BLEND_OP_ADD;
	infos->color_blend_state.attachmentCount = 1;
	infos->color_blend_state.pAttachments = &infos->blend_attachment_state;

	infos->graphics_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	infos->graphics_pipeline.stageCount = 2;
	infos->graphics_pipeline.pStages = infos->shader_stages;
	infos->graphics_pipeline.pVertexInputState = &infos->vertex_input_state;
	infos->graphics_pipeline.pInputAssemblyState = &infos->input_assembly_state;
	infos->graphics_pipeline.pViewportState = &infos->viewport_state;
	infos->graphics_pipeline.pRasterizationState = &infos->rasterization_state;
	infos->graphics_pipeline.pMultisampleState = &infos->multisample_state;
	infos->graphics_pipeline.pDepthStencilState = &infos->depth_stencil_state;
	infos->graphics_pipeline.pColorBlendState = &infos->color_blend_state;
	infos->graphics_pipeline.pDynamicState = &infos->dynamic_state;
	infos->graphics_pipeline.layout = vulkan_globals.basic_pipeline_layout.handle;
	infos->graphics_pipeline.renderPass = vulkan_globals.secondary_cb_contexts[CBX_WORLD_0].render_pass;
}

/*
===============
R_CreateBasicPipelines
===============
*/
static void R_CreateBasicPipelines ()
{
	int                     render_pass;
	VkResult                err;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	VkRenderPass main_render_pass = vulkan_globals.secondary_cb_contexts[CBX_WORLD_0].render_pass;
	VkRenderPass ui_render_pass = vulkan_globals.secondary_cb_contexts[CBX_GUI].render_pass;

	infos.depth_stencil_state.depthTestEnable = VK_TRUE;
	infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
	infos.shader_stages[1].module = basic_alphatest_frag_module;
	for (render_pass = 0; render_pass < 2; ++render_pass)
	{
		infos.graphics_pipeline.renderPass = (render_pass == 0) ? main_render_pass : ui_render_pass;
		infos.multisample_state.rasterizationSamples = (render_pass == 0) ? vulkan_globals.sample_count : VK_SAMPLE_COUNT_1_BIT;

		assert (vulkan_globals.basic_alphatest_pipeline[render_pass].handle == VK_NULL_HANDLE);
		err = vkCreateGraphicsPipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.basic_alphatest_pipeline[render_pass].handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateGraphicsPipelines failed");
		vulkan_globals.basic_alphatest_pipeline[render_pass].layout = vulkan_globals.basic_pipeline_layout;
		GL_SetObjectName ((uint64_t)vulkan_globals.basic_alphatest_pipeline[render_pass].handle, VK_OBJECT_TYPE_PIPELINE, "basic_alphatest");
	}

	infos.shader_stages[1].module = basic_notex_frag_module;
	infos.depth_stencil_state.depthTestEnable = VK_FALSE;
	infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
	infos.blend_attachment_state.blendEnable = VK_TRUE;

	for (render_pass = 0; render_pass < 2; ++render_pass)
	{
		infos.graphics_pipeline.renderPass = (render_pass == 0) ? main_render_pass : ui_render_pass;
		infos.multisample_state.rasterizationSamples = (render_pass == 0) ? vulkan_globals.sample_count : VK_SAMPLE_COUNT_1_BIT;

		assert (vulkan_globals.basic_notex_blend_pipeline[render_pass].handle == VK_NULL_HANDLE);
		err = vkCreateGraphicsPipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.basic_notex_blend_pipeline[render_pass].handle);
	}

	infos.graphics_pipeline.renderPass = main_render_pass;
	infos.graphics_pipeline.subpass = 0;
	infos.multisample_state.rasterizationSamples = vulkan_globals.sample_count;

	infos.shader_stages[1].module = basic_frag_module;

	for (render_pass = 0; render_pass < 2; ++render_pass)
	{
		infos.graphics_pipeline.renderPass = (render_pass == 0) ? main_render_pass : ui_render_pass;
		infos.multisample_state.rasterizationSamples = (render_pass == 0) ? vulkan_globals.sample_count : VK_SAMPLE_COUNT_1_BIT;

		assert (vulkan_globals.basic_blend_pipeline[render_pass].handle == VK_NULL_HANDLE);
		err = vkCreateGraphicsPipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.basic_blend_pipeline[render_pass].handle);
	}
}

/*
===============
R_CreateWorldPipelines
===============
*/
static void R_CreateWorldPipelines ()
{
	VkResult                err;
	int                     alpha_blend, alpha_test, fullbright_enabled;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	infos.rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
	infos.rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	infos.depth_stencil_state.depthTestEnable = VK_TRUE;
	infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
	infos.rasterization_state.depthBiasEnable = VK_TRUE;
	infos.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	infos.dynamic_states[infos.dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;

	infos.vertex_input_state.vertexAttributeDescriptionCount = 3;
	infos.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
	infos.vertex_input_state.vertexBindingDescriptionCount = 1;
	infos.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;

	VkSpecializationMapEntry specialization_entries[3];
	specialization_entries[0].constantID = 0;
	specialization_entries[0].offset = 0;
	specialization_entries[0].size = 4;
	specialization_entries[1].constantID = 1;
	specialization_entries[1].offset = 4;
	specialization_entries[1].size = 4;
	specialization_entries[2].constantID = 2;
	specialization_entries[2].offset = 8;
	specialization_entries[2].size = 4;

	uint32_t specialization_data[3];
	specialization_data[0] = 0;
	specialization_data[1] = 0;
	specialization_data[2] = 0;

	VkSpecializationInfo specialization_info;
	specialization_info.mapEntryCount = 3;
	specialization_info.pMapEntries = specialization_entries;
	specialization_info.dataSize = 12;
	specialization_info.pData = specialization_data;

	infos.graphics_pipeline.layout = vulkan_globals.world_pipeline_layout.handle;

	infos.shader_stages[0].module = world_vert_module;
	infos.shader_stages[1].module = world_frag_module;
	infos.shader_stages[1].pSpecializationInfo = &specialization_info;

	for (alpha_blend = 0; alpha_blend < 2; ++alpha_blend)
	{
		for (alpha_test = 0; alpha_test < 2; ++alpha_test)
		{
			for (fullbright_enabled = 0; fullbright_enabled < 2; ++fullbright_enabled)
			{
				int pipeline_index = fullbright_enabled + (alpha_test * 2) + (alpha_blend * 4);

				specialization_data[0] = fullbright_enabled;
				specialization_data[1] = alpha_test;
				specialization_data[2] = alpha_blend;

				infos.blend_attachment_state.blendEnable = alpha_blend ? VK_TRUE : VK_FALSE;
				infos.depth_stencil_state.depthWriteEnable = alpha_blend ? VK_FALSE : VK_TRUE;
				if (pipeline_index > 0)
				{
					infos.graphics_pipeline.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
					infos.graphics_pipeline.basePipelineHandle = vulkan_globals.world_pipelines[0].handle;
					infos.graphics_pipeline.basePipelineIndex = -1;
				}
				else
				{
					infos.graphics_pipeline.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
				}

				assert (vulkan_globals.world_pipelines[pipeline_index].handle == VK_NULL_HANDLE);
				err = vkCreateGraphicsPipelines (
					vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.world_pipelines[pipeline_index].handle);
				if (err != VK_SUCCESS)
					Sys_Error ("vkCreateGraphicsPipelines failed");
				GL_SetObjectName ((uint64_t)vulkan_globals.world_pipelines[pipeline_index].handle, VK_OBJECT_TYPE_PIPELINE, va ("world %d", pipeline_index));
				vulkan_globals.world_pipelines[pipeline_index].layout = vulkan_globals.world_pipeline_layout;
			}
		}
	}
}

/*
===============
R_CreateAliasPipelines
===============
*/
static void R_CreateAliasPipelines ()
{
	VkResult                err;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	infos.depth_stencil_state.depthTestEnable = VK_TRUE;
	infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
	infos.rasterization_state.depthBiasEnable = VK_FALSE;
	infos.blend_attachment_state.blendEnable = VK_FALSE;
	infos.shader_stages[1].pSpecializationInfo = NULL;

	infos.vertex_input_state.vertexAttributeDescriptionCount = 5;
	infos.vertex_input_state.pVertexAttributeDescriptions = alias_vertex_input_attribute_descriptions;
	infos.vertex_input_state.vertexBindingDescriptionCount = 3;
	infos.vertex_input_state.pVertexBindingDescriptions = alias_vertex_binding_descriptions;

	infos.shader_stages[0].module = alias_vert_module;
	infos.shader_stages[1].module = alias_frag_module;

	infos.graphics_pipeline.layout = vulkan_globals.alias_pipeline.layout.handle;

	assert (vulkan_globals.alias_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateGraphicsPipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateGraphicsPipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.alias_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias");

	infos.shader_stages[1].module = alias_alphatest_frag_module;

	assert (vulkan_globals.alias_alphatest_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateGraphicsPipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_alphatest_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateGraphicsPipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.alias_alphatest_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias_alphatest");
	vulkan_globals.alias_alphatest_pipeline.layout = vulkan_globals.alias_pipeline.layout;

	infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
	infos.blend_attachment_state.blendEnable = VK_TRUE;
	infos.shader_stages[1].module = alias_frag_module;

	assert (vulkan_globals.alias_blend_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateGraphicsPipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_blend_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateGraphicsPipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.alias_blend_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias_blend");
	vulkan_globals.alias_blend_pipeline.layout = vulkan_globals.alias_pipeline.layout;

	infos.shader_stages[1].module = alias_alphatest_frag_module;

	assert (vulkan_globals.alias_alphatest_blend_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateGraphicsPipelines (
		vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_alphatest_blend_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateGraphicsPipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.alias_alphatest_blend_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias_alphatest_blend");
	vulkan_globals.alias_alphatest_blend_pipeline.layout = vulkan_globals.alias_pipeline.layout;

	if (vulkan_globals.non_solid_fill)
	{
		infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
		infos.rasterization_state.polygonMode = VK_POLYGON_MODE_LINE;
		infos.depth_stencil_state.depthTestEnable = VK_FALSE;
		infos.depth_stencil_state.depthWriteEnable = VK_FALSE;
		infos.blend_attachment_state.blendEnable = VK_FALSE;
		infos.input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		infos.shader_stages[0].module = alias_vert_module;
		infos.shader_stages[1].module = showtris_frag_module;

		infos.graphics_pipeline.layout = vulkan_globals.alias_pipeline.layout.handle;

		assert (vulkan_globals.alias_showtris_pipeline.handle == VK_NULL_HANDLE);
		err = vkCreateGraphicsPipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_showtris_pipeline.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateGraphicsPipelines failed");
		GL_SetObjectName ((uint64_t)vulkan_globals.alias_showtris_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias_showtris");
		vulkan_globals.alias_showtris_pipeline.layout = vulkan_globals.alias_pipeline.layout;

		infos.depth_stencil_state.depthTestEnable = VK_TRUE;
		infos.rasterization_state.depthBiasEnable = VK_TRUE;
		infos.rasterization_state.depthBiasConstantFactor = 500.0f;
		infos.rasterization_state.depthBiasSlopeFactor = 0.0f;

		assert (vulkan_globals.alias_showtris_depth_test_pipeline.handle == VK_NULL_HANDLE);
		err = vkCreateGraphicsPipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.alias_showtris_depth_test_pipeline.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateGraphicsPipelines failed");
		GL_SetObjectName ((uint64_t)vulkan_globals.alias_showtris_depth_test_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "alias_showtris_depth_test");
		vulkan_globals.alias_showtris_depth_test_pipeline.layout = vulkan_globals.alias_pipeline.layout;
	}
}

/*
===============
R_CreatePostprocessPipelines
===============
*/
static void R_CreatePostprocessPipelines ()
{
	VkResult                err;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	infos.multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
	infos.rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	infos.rasterization_state.cullMode = VK_CULL_MODE_NONE;
	infos.rasterization_state.depthBiasEnable = VK_FALSE;
	infos.depth_stencil_state.depthTestEnable = VK_TRUE;
	infos.depth_stencil_state.depthWriteEnable = VK_TRUE;
	infos.blend_attachment_state.blendEnable = VK_FALSE;

	infos.vertex_input_state.vertexAttributeDescriptionCount = 0;
	infos.vertex_input_state.pVertexAttributeDescriptions = NULL;
	infos.vertex_input_state.vertexBindingDescriptionCount = 0;
	infos.vertex_input_state.pVertexBindingDescriptions = NULL;

	infos.shader_stages[0].module = postprocess_vert_module;
	infos.shader_stages[1].module = postprocess_frag_module;
	infos.graphics_pipeline.renderPass = vulkan_globals.secondary_cb_contexts[CBX_GUI].render_pass;
	infos.graphics_pipeline.layout = vulkan_globals.postprocess_pipeline.layout.handle;
	infos.graphics_pipeline.subpass = 1;

	assert (vulkan_globals.postprocess_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateGraphicsPipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.graphics_pipeline, NULL, &vulkan_globals.postprocess_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateGraphicsPipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.postprocess_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "postprocess");
}

/*
===============
R_CreateScreenEffectsPipelines
===============
*/
static void R_CreateScreenEffectsPipelines ()
{
	VkResult                err;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	VkPipelineShaderStageCreateInfo compute_shader_stage;
	memset (&compute_shader_stage, 0, sizeof (compute_shader_stage));
	compute_shader_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	compute_shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute_shader_stage.module =
		(vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) ? screen_effects_10bit_comp_module : screen_effects_8bit_comp_module;
	compute_shader_stage.pName = "main";

	memset (&infos.compute_pipeline, 0, sizeof (infos.compute_pipeline));
	infos.compute_pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	infos.compute_pipeline.stage = compute_shader_stage;
	infos.compute_pipeline.layout = vulkan_globals.screen_effects_pipeline.layout.handle;

	assert (vulkan_globals.screen_effects_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateComputePipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.compute_pipeline, NULL, &vulkan_globals.screen_effects_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateComputePipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.screen_effects_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "screen_effects");

	compute_shader_stage.module =
		(vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) ? screen_effects_10bit_scale_comp_module : screen_effects_8bit_scale_comp_module;
	infos.compute_pipeline.stage = compute_shader_stage;
	assert (vulkan_globals.screen_effects_scale_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateComputePipelines (
		vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.compute_pipeline, NULL, &vulkan_globals.screen_effects_scale_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateComputePipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.screen_effects_scale_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "screen_effects_scale");

#if defined(VK_EXT_subgroup_size_control)
	if (vulkan_globals.screen_effects_sops)
	{
		compute_shader_stage.module = (vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) ? screen_effects_10bit_scale_sops_comp_module
		                                                                                                  : screen_effects_8bit_scale_sops_comp_module;
		compute_shader_stage.flags =
			VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT | VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
		infos.compute_pipeline.stage = compute_shader_stage;
		assert (vulkan_globals.screen_effects_scale_sops_pipeline.handle == VK_NULL_HANDLE);
		err = vkCreateComputePipelines (
			vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.compute_pipeline, NULL, &vulkan_globals.screen_effects_scale_sops_pipeline.handle);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateComputePipelines failed");
		GL_SetObjectName ((uint64_t)vulkan_globals.screen_effects_scale_sops_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "screen_effects_scale_sops");
		compute_shader_stage.flags = 0;
	}
#endif
}

/*
===============
R_CreateUpdateLightmapPipelines
===============
*/
static void R_CreateUpdateLightmapPipelines ()
{
	VkResult                err;
	pipeline_create_infos_t infos;
	R_InitDefaultStates (&infos);

	VkPipelineShaderStageCreateInfo compute_shader_stage;
	memset (&compute_shader_stage, 0, sizeof (compute_shader_stage));
	compute_shader_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	compute_shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute_shader_stage.module = update_lightmap_comp_module;
	compute_shader_stage.pName = "main";

	memset (&infos.compute_pipeline, 0, sizeof (infos.compute_pipeline));
	infos.compute_pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	infos.compute_pipeline.stage = compute_shader_stage;
	infos.compute_pipeline.layout = vulkan_globals.update_lightmap_pipeline.layout.handle;

	assert (vulkan_globals.update_lightmap_pipeline.handle == VK_NULL_HANDLE);
	err = vkCreateComputePipelines (vulkan_globals.device, VK_NULL_HANDLE, 1, &infos.compute_pipeline, NULL, &vulkan_globals.update_lightmap_pipeline.handle);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateComputePipelines failed");
	GL_SetObjectName ((uint64_t)vulkan_globals.update_lightmap_pipeline.handle, VK_OBJECT_TYPE_PIPELINE, "update_lightmap");
}

/*
===================
R_ScaleChanged_f
===================
*/
static void R_ScaleChanged_f (cvar_t *var)
{
}

/*
===============
R_Init
===============
*/
void R_Init (void)
{
	Cmd_AddCommand ("timerefresh", R_TimeRefresh_f);
	Cmd_AddCommand ("pointfile", R_ReadPointFile_f);

	Cvar_RegisterVariable (&r_fullbright);
	Cvar_RegisterVariable (&r_lightmap);
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_drawviewmodel);
	Cvar_RegisterVariable (&r_wateralpha);
	Cvar_SetCallback (&r_wateralpha, R_SetWateralpha_f);
	Cvar_RegisterVariable (&r_dynamic);
	Cvar_RegisterVariable (&r_novis);
#if defined(USE_SIMD)
	Cvar_RegisterVariable (&r_simd);
	Cvar_SetCallback (&r_simd, R_SIMD_f);
	R_SIMD_f (&r_simd);
#endif
	Cvar_RegisterVariable (&r_speeds);
	Cvar_RegisterVariable (&r_pos);
	Cvar_RegisterVariable (&gl_polyblend);
	Cvar_RegisterVariable (&gl_nocolors);

	// johnfitz -- new cvars
	Cvar_RegisterVariable (&r_clearcolor);
	Cvar_SetCallback (&r_clearcolor, R_SetClearColor_f);
	Cvar_RegisterVariable (&r_fastclear);
	Cvar_SetCallback (&r_fastclear, R_SetFastClear_f);
	Cvar_RegisterVariable (&r_waterquality);
	Cvar_RegisterVariable (&r_waterwarp);
	Cvar_RegisterVariable (&r_waterwarpcompute);
	Cvar_RegisterVariable (&r_flatlightstyles);
	Cvar_RegisterVariable (&r_lerplightstyles);
	Cvar_RegisterVariable (&r_oldskyleaf);
	Cvar_RegisterVariable (&r_drawworld);
	Cvar_RegisterVariable (&r_showtris);
	Cvar_RegisterVariable (&r_showbboxes);
	Cvar_RegisterVariable (&gl_farclip);
	Cvar_RegisterVariable (&gl_fullbrights);
	Cvar_SetCallback (&gl_fullbrights, GL_Fullbrights_f);
	Cvar_RegisterVariable (&r_lerpmodels);
	Cvar_RegisterVariable (&r_lerpmove);
	Cvar_RegisterVariable (&r_nolerp_list);
	Cvar_SetCallback (&r_nolerp_list, R_Model_ExtraFlags_List_f);
	// johnfitz

	Cvar_RegisterVariable (&gl_zfix); // QuakeSpasm z-fighting fix
	Cvar_RegisterVariable (&r_lavaalpha);
	Cvar_RegisterVariable (&r_telealpha);
	Cvar_RegisterVariable (&r_slimealpha);
	Cvar_RegisterVariable (&r_scale);
	Cvar_RegisterVariable (&r_lodbias);
	Cvar_SetCallback (&r_scale, R_ScaleChanged_f);
	Cvar_SetCallback (&r_lodbias, R_ScaleChanged_f);
	Cvar_SetCallback (&r_lavaalpha, R_SetLavaalpha_f);
	Cvar_SetCallback (&r_telealpha, R_SetTelealpha_f);
	Cvar_SetCallback (&r_slimealpha, R_SetSlimealpha_f);

	Cvar_RegisterVariable (&r_gpulightmapupdate);
	Cvar_RegisterVariable (&r_tasks);
	Cvar_RegisterVariable (&r_parallelmark);
	Cvar_RegisterVariable (&r_usesops);

	R_InitParticles ();
	SetClearColor (); // johnfitz

	Sky_Init (); // johnfitz
	Fog_Init (); // johnfitz
}

/*
===============
R_TranslatePlayerSkin -- johnfitz -- rewritten.  also, only handles new colors, not new skins
===============
*/
void R_TranslatePlayerSkin (int playernum)
{
	int top, bottom;

	top = (cl.scores[playernum].colors & 0xf0) >> 4;
	bottom = cl.scores[playernum].colors & 15;

	// FIXME: if gl_nocolors is on, then turned off, the textures may be out of sync with the scoreboard colors.
	if (!gl_nocolors.value)
		if (playertextures[playernum])
			TexMgr_ReloadImage (playertextures[playernum], top, bottom);
}

/*
===============
R_TranslateNewPlayerSkin -- johnfitz -- split off of TranslatePlayerSkin -- this is called when
the skin or model actually changes, instead of just new colors
added bug fix from bengt jardup
===============
*/
void R_TranslateNewPlayerSkin (int playernum)
{
	char        name[64];
	byte       *pixels;
	aliashdr_t *paliashdr;
	int         skinnum;

	// get correct texture pixels
	entity_t *currententity = &cl.entities[1 + playernum];

	if (!currententity->model || currententity->model->type != mod_alias)
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);

	skinnum = currententity->skinnum;

	// TODO: move these tests to the place where skinnum gets received from the server
	if (skinnum < 0 || skinnum >= paliashdr->numskins)
	{
		Con_DPrintf ("(%d): Invalid player skin #%d\n", playernum, skinnum);
		skinnum = 0;
	}

	pixels = (byte *)paliashdr->texels[skinnum];

	// upload new image
	q_snprintf (name, sizeof (name), "player_%i", playernum);
	playertextures[playernum] = TexMgr_LoadImage (
		currententity->model, name, paliashdr->skinwidth, paliashdr->skinheight, SRC_INDEXED, pixels, paliashdr->gltextures[skinnum][0]->source_file,
		paliashdr->gltextures[skinnum][0]->source_offset, TEXPREF_PAD | TEXPREF_OVERWRITE);

	// now recolor it
	R_TranslatePlayerSkin (playernum);
}

/*
===============
R_NewGame -- johnfitz -- handle a game switch
===============
*/
void R_NewGame (void)
{
	int i;

	// clear playertexture pointers (the textures themselves were freed by texmgr_newgame)
	for (i = 0; i < MAX_SCOREBOARD; i++)
		playertextures[i] = NULL;
}

/*
=============
R_ParseWorldspawn

called at map load
=============
*/
static void R_ParseWorldspawn (void)
{
	char        key[128], value[4096];
	const char *data;

	map_fallbackalpha = r_wateralpha.value;
	map_wateralpha = (cl.worldmodel->contentstransparent & SURF_DRAWWATER) ? r_wateralpha.value : 1;
	map_lavaalpha = (cl.worldmodel->contentstransparent & SURF_DRAWLAVA) ? r_lavaalpha.value : 1;
	map_telealpha = (cl.worldmodel->contentstransparent & SURF_DRAWTELE) ? r_telealpha.value : 1;
	map_slimealpha = (cl.worldmodel->contentstransparent & SURF_DRAWSLIME) ? r_slimealpha.value : 1;

	data = COM_Parse (cl.worldmodel->entities);
	if (!data)
		return; // error
	if (com_token[0] != '{')
		return; // error
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		if (!strcmp ("wateralpha", key))
			map_wateralpha = atof (value);

		if (!strcmp ("lavaalpha", key))
			map_lavaalpha = atof (value);

		if (!strcmp ("telealpha", key))
			map_telealpha = atof (value);

		if (!strcmp ("slimealpha", key))
			map_slimealpha = atof (value);
	}
}

/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int i;

	for (i = 0; i < 256; i++)
		d_lightstylevalue[i] = 264; // normal light value

	// clear out efrags in case the level hasn't been reloaded
	// FIXME: is this one short?
	for (i = 0; i < cl.worldmodel->numleafs; i++)
		cl.worldmodel->leafs[i].efrags = NULL;

	r_viewleaf = NULL;
	R_ClearParticles ();
#ifdef PSET_SCRIPT
	PScript_ClearParticles ();
#endif
	GL_DeleteBModelVertexBuffer ();

	GL_BuildLightmaps ();
	GL_BuildBModelVertexBuffer ();
	GL_PrepareSIMDData ();
	// ericw -- no longer load alias models into a VBO here, it's done in Mod_LoadAliasModel

	r_framecount = 0;    // johnfitz -- paranoid?
	r_visframecount = 0; // johnfitz -- paranoid?

	Sky_NewMap ();        // johnfitz -- skybox in worldspawn
	Fog_NewMap ();        // johnfitz -- global fog in worldspawn
	R_ParseWorldspawn (); // ericw -- wateralpha, lavaalpha, telealpha, slimealpha in worldspawn
}

/*
====================
R_TimeRefresh_f

For program optimization
====================
*/
void R_TimeRefresh_f (void)
{
	int   i;
	float start, stop, time;

	if (cls.state != ca_connected)
	{
		Con_Printf ("Not connected to a server\n");
		return;
	}

	GL_SynchronizeEndRenderingTask ();

	start = Sys_DoubleTime ();
	for (i = 0; i < 128; i++)
	{
		GL_BeginRendering (false, NULL, &glx, &gly, &glwidth, &glheight);
		r_refdef.viewangles[1] = i / 128.0 * 360.0;
		R_RenderView (false, INVALID_TASK_HANDLE, INVALID_TASK_HANDLE, INVALID_TASK_HANDLE);
		GL_EndRendering (false, false);
	}

	// glFinish ();
	stop = Sys_DoubleTime ();
	time = stop - start;
	Con_Printf ("%f seconds (%f fps)\n", time, 128 / time);
}
