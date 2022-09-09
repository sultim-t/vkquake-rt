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
// r_sprite.c -- sprite model rendering

#include "quakedef.h"
#include "gl_heap.h"

extern cvar_t rt_model_metal, rt_model_rough;
extern cvar_t rt_dlight_intensity, rt_dlight_radius;

/*
================
R_GetSpriteFrame
================
*/
static mspriteframe_t *R_GetSpriteFrame (entity_t *currentent)
{
	msprite_t      *psprite;
	mspritegroup_t *pspritegroup;
	mspriteframe_t *pspriteframe;
	int             i, numframes, frame;
	float		  *pintervals, fullinterval, targettime, time;

	psprite = (msprite_t *)currentent->model->extradata;
	frame = currentent->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_DrawSprite: no such frame %d for '%s'\n", frame, currentent->model->name);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes - 1];

		time = cl.time + currentent->syncbase;

		// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
		// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i = 0; i < (numframes - 1); i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}

/*
================
R_CreateSpriteVertices
================
*/
static void R_CreateSpriteVertices (entity_t *e, mspriteframe_t *frame, RgVertex vertices[4])
{
	vec3_t     point, v_forward, v_right, v_up;
	msprite_t *psprite;
	float     *s_up, *s_right;
	float      angle, sr, cr;

	psprite = (msprite_t *)e->model->extradata;

	switch (psprite->type)
	{
	case SPR_VP_PARALLEL_UPRIGHT: // faces view plane, up is towards the heavens
		v_up[0] = 0;
		v_up[1] = 0;
		v_up[2] = 1;
		s_up = v_up;
		s_right = vright;
		break;
	case SPR_FACING_UPRIGHT: // faces camera origin, up is towards the heavens
		VectorSubtract (e->origin, r_origin, v_forward);
		v_forward[2] = 0;
		VectorNormalizeFast (v_forward);
		v_right[0] = v_forward[1];
		v_right[1] = -v_forward[0];
		v_right[2] = 0;
		v_up[0] = 0;
		v_up[1] = 0;
		v_up[2] = 1;
		s_up = v_up;
		s_right = v_right;
		break;
	case SPR_VP_PARALLEL: // faces view plane, up is towards the top of the screen
		s_up = vup;
		s_right = vright;
		break;
	case SPR_ORIENTED: // pitch yaw roll are independent of camera
		AngleVectors (e->angles, v_forward, v_right, v_up);
		s_up = v_up;
		s_right = v_right;
		break;
	case SPR_VP_PARALLEL_ORIENTED: // faces view plane, but obeys roll value
		angle = e->angles[ROLL] * M_PI_DIV_180;
		sr = sin (angle);
		cr = cos (angle);
		v_right[0] = vright[0] * cr + vup[0] * sr;
		v_right[1] = vright[1] * cr + vup[1] * sr;
		v_right[2] = vright[2] * cr + vup[2] * sr;
		v_up[0] = vright[0] * -sr + vup[0] * cr;
		v_up[1] = vright[1] * -sr + vup[1] * cr;
		v_up[2] = vright[2] * -sr + vup[2] * cr;
		s_up = v_up;
		s_right = v_right;
		break;
	default:
		return;
	}

	VectorMA (e->origin, frame->down, s_up, point);
	VectorMA (point, frame->left, s_right, point);
	vertices[0].position[0] = point[0];
	vertices[0].position[1] = point[1];
	vertices[0].position[2] = point[2];
	vertices[0].texCoord[0] = 0.0f;
	vertices[0].texCoord[1] = frame->tmax;
	vertices[0].packedColor = RT_PACKED_COLOR_WHITE;

	VectorMA (e->origin, frame->up, s_up, point);
	VectorMA (point, frame->left, s_right, point);
	vertices[1].position[0] = point[0];
	vertices[1].position[1] = point[1];
	vertices[1].position[2] = point[2];
	vertices[1].texCoord[0] = 0.0f;
	vertices[1].texCoord[1] = 0.0f;
	vertices[1].packedColor = RT_PACKED_COLOR_WHITE;

	VectorMA (e->origin, frame->up, s_up, point);
	VectorMA (point, frame->right, s_right, point);
	vertices[2].position[0] = point[0];
	vertices[2].position[1] = point[1];
	vertices[2].position[2] = point[2];
	vertices[2].texCoord[0] = frame->smax;
	vertices[2].texCoord[1] = 0.0f;
	vertices[2].packedColor = RT_PACKED_COLOR_WHITE;

	VectorMA (e->origin, frame->down, s_up, point);
	VectorMA (point, frame->right, s_right, point);
	vertices[3].position[0] = point[0];
	vertices[3].position[1] = point[1];
	vertices[3].position[2] = point[2];
	vertices[3].texCoord[0] = frame->smax;
	vertices[3].texCoord[1] = frame->tmax;
	vertices[3].packedColor = RT_PACKED_COLOR_WHITE;
}

/*
=================
R_DrawSpriteModel -- johnfitz -- rewritten: now supports all orientations
=================
*/
void R_DrawSpriteModel (cb_context_t *cbx, entity_t *e, int entuniqueid)
{
	msprite_t      *psprite = (msprite_t *)e->model->extradata;
	mspriteframe_t *frame = R_GetSpriteFrame (e);
	gltexture_t    *tx = frame->gltexture;


    RgVertex vertices[4] = {0};
	R_CreateSpriteVertices (e, frame, vertices);

	qboolean is_decal = psprite->type == SPR_ORIENTED;
	qboolean is_rasterized = is_decal;

	if (tx && tx->rtcustomtextype == RT_CUSTOMTEXTUREINFO_TYPE_RASTER_LIGHT)
	{
		is_rasterized = true;

		vec3_t color = {tx->rtlightcolor[0], tx->rtlightcolor[1], tx->rtlightcolor[2]};
		VectorScale (color, CVAR_TO_FLOAT (rt_dlight_intensity), color);
		RT_FIXUP_LIGHT_INTENSITY (color, true);

		RgSphericalLightUploadInfo light_info = {
			.uniqueID = RT_GetAliasModelUniqueId (entuniqueid),
			.color = {color[0], color[1], color[2]},
			.position = {e->origin[0], e->origin[1], e->origin[2]},
			.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_dlight_radius)),
		};

		RgResult r = rgUploadSphericalLight (vulkan_globals.instance, &light_info);
		RG_CHECK (r);
	}

	if (is_rasterized)
	{
		RgRasterizedGeometryUploadInfo info = {
			.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
			.vertexCount = countof (vertices),
			.pVertices = vertices,
			.indexCount = RT_GetFanIndexCount (countof (vertices)),
			.pIndices = RT_GetFanIndices (countof (vertices)),
			.transform = RT_TRANSFORM_IDENTITY,
			.color = RT_COLOR_WHITE,
			.material = tx ? tx->rtmaterial : RG_NO_MATERIAL,
			.pipelineState = 
			    RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | 
			    RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE | 
			    RG_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = RT_GetSpriteModelUniqueId (entuniqueid),
			.flags = RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType = RG_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
			.visibilityType = RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = countof (vertices),
			.pVertices = vertices,
			.indexCount = RT_GetFanIndexCount (countof (vertices)),
			.pIndices = RT_GetFanIndices (countof (vertices)),
			.layerColors = {RT_COLOR_WHITE},
			.layerBlendingTypes = {RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
			.defaultRoughness = CVAR_TO_FLOAT (rt_model_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_model_metal),
			.defaultEmission = 0,
			.geomMaterial = {tx ? tx->rtmaterial : RG_NO_MATERIAL},
			.transform = RT_TRANSFORM_IDENTITY,
		};

		RgResult r = rgUploadGeometry (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}
}

/*
=================
R_DrawSpriteModel_ShowTris
=================
*/
void R_DrawSpriteModel_ShowTris (cb_context_t *cbx, entity_t *e)
{
}
