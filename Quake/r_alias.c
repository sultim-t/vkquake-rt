/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

// r_alias.c -- alias model rendering

#include "quakedef.h"

extern cvar_t r_drawflat, gl_fullbrights, r_lerpmodels, r_lerpmove, r_showtris; // johnfitz
extern cvar_t scr_fov;

extern cvar_t rt_model_rough, rt_model_metal, rt_enable_pvs;
extern cvar_t rt_viewm_fovscale, rt_viewm_wide;
extern cvar_t rt_classic_render;

// up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; // johnfitz -- changed to an array of pointers

#define NUMVERTEXNORMALS 162

float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16

// johnfitz -- struct for passing lerp information to drawing functions
typedef struct
{
	short  pose1;
	short  pose2;
	float  blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
// johnfitz

typedef struct
{
	float        model_matrix[16];
	float        shade_vector[3];
	float        blend_factor;
	float        light_color[3];
	float        entalpha;
	unsigned int flags;
} aliasubo_t;

static const RgVertex *GetModelVerticesForPose (const qmodel_t *m, const aliashdr_t *hdr, int pose)
{
	assert (m != NULL && m->rtvertices != NULL);

	return &m->rtvertices[(size_t)pose * hdr->numverts_vbo];
}

static size_t GetNextAllocStep (size_t x)
{
	const size_t step = 4096;

	size_t i = (x + (step - 1)) / step;
	return i * step;
}

static float r_avertexnormal_dot (const vec3_t vertexnormal, const vec3_t shadevector) 
{
	float dot = DotProduct (vertexnormal, shadevector);
	// wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case
	if (dot < 0.0f)
		return 1.0f + dot * (13.0f / 44.0f);
	else
		return 1.0f + dot;
}

static float Lerp (float a, float b, float t)
{
	float dt = b - a;
	return a + dt * t;
}

static void LerpPosition (float *dst, const float *src1, const float *src2, float blend)
{
	for (int j = 0; j < 3; j++)
	{
		dst[j] = Lerp (src1[j], src2[j], blend);
	}
}

static const RgVertex *
GetPoseVertices (const qmodel_t *m, const aliashdr_t *hdr, int pose1, int pose2, float blend, /* const */ vec3_t shadevector, /* const */ vec3_t lightcolor)
{
	const RgVertex *v_pose1 = GetModelVerticesForPose (m, hdr, pose1);
	const RgVertex *v_pose2 = GetModelVerticesForPose (m, hdr, pose2);

	// we don't care about per-vertex colors with RT
	const qboolean need_vertex_lighting = CVAR_TO_BOOL (rt_classic_render);

	if (blend < FLT_EPSILON && !need_vertex_lighting)
	{
		return v_pose1;
	}

	static RgVertex *tempstorage = NULL;
	static size_t    tempstorage_numverts = 0;
	if ((size_t)hdr->numverts_vbo > tempstorage_numverts)
	{
		tempstorage_numverts = GetNextAllocStep (hdr->numverts_vbo);
		Mem_Free (tempstorage);
		tempstorage = Mem_Alloc (tempstorage_numverts * sizeof (RgVertex));
	}

	memcpy (tempstorage, v_pose1, hdr->numverts_vbo * sizeof (RgVertex));

	for (int i = 0; i < hdr->numverts_vbo; i++)
	{
	    RgVertex *dst = &tempstorage[i];

		const RgVertex *src1 = &v_pose1[i];
		const RgVertex *src2 = &v_pose2[i];

		LerpPosition (dst->position, src1->position, src2->position, blend);

		if (need_vertex_lighting)
		{
			float dot1 = r_avertexnormal_dot (src1->normal, shadevector);
			float dot2 = r_avertexnormal_dot (src2->normal, shadevector);

			vec3_t vertcolor;
			VectorScale (lightcolor, Lerp (dot1, dot2, blend), vertcolor);

			dst->packedColor = RT_PackColorToUint32_FromFloat01 (vertcolor[0], vertcolor[1], vertcolor[2], 1.0f);
		}
	}

	return tempstorage;
}

static RgTransform RT_GetAliasModelTransform (const aliashdr_t *paliashdr, lerpdata_t *lerpdata, qboolean isfirstperson)
{
	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, lerpdata->origin, lerpdata->angles);

	float fovscalex = 1.0f;
	float fovscaley = 1.0f;
	if (isfirstperson && CVAR_TO_FLOAT (rt_viewm_fovscale) > 0)
	{
		fovscalex = CVAR_TO_FLOAT (rt_viewm_fovscale) * CVAR_TO_FLOAT (rt_viewm_wide);
		fovscaley = CVAR_TO_FLOAT (rt_viewm_fovscale);
	}

	float translation_matrix[16];
	TranslationMatrix (translation_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscalex, paliashdr->scale_origin[2] * fovscaley);
	MatrixMultiply (model_matrix, translation_matrix);

	float scale_matrix[16];
	ScaleMatrix (scale_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscalex, paliashdr->scale[2] * fovscaley);
	MatrixMultiply (model_matrix, scale_matrix);

	return RT_GetModelTransform (model_matrix);
}

/*
=============
GL_DrawAliasFrame -- ericw

Optimized alias model drawing codepath. This makes 1 draw call,
no vertex data is uploaded (it's already in the r_meshvbo and r_meshindexesvbo
static VBOs), and lerping and lighting is done in the vertex shader.

Supports optional fullbright pixels.

Based on code by MH from RMQEngine
=============
*/
static void GL_DrawAliasFrame (
	cb_context_t *cbx, entity_t *e, aliashdr_t *paliashdr, lerpdata_t lerpdata, gltexture_t *tx, float entity_alpha,
	qboolean alphatest, vec3_t shadevector, vec3_t lightcolor, int entuniqueid)
{

	// poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
    float blend = lerpdata.pose1 != lerpdata.pose2 ? lerpdata.blend : 0;

	qboolean rasterize = entity_alpha < 1.0f;
	qboolean isfirstperson = (e == &cl.viewent);
	qboolean isviewer = (e == &cl.entities[cl.viewentity]);

	assert (
		(!isviewer && !isfirstperson) || 
		(isviewer && !isfirstperson) || 
		(!isviewer && isfirstperson));

	if (rasterize)
	{
		if (isviewer)
		{
			return;
		}

		RgRasterizedGeometryUploadInfo info = {
			.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
			.vertexCount = paliashdr->numverts_vbo,
			.pVertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, shadevector, lightcolor),
			.indexCount = paliashdr->numindexes,
			.pIndices = e->model->rtindices,
			.transform = RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson),
			.color = RT_COLOR_WHITE,
			.material = tx ? tx->rtmaterial : RG_NO_MATERIAL,
			.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		if (alphatest)
		{
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
		}

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = RT_GetAliasModelUniqueId (entuniqueid),
			.flags = RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType = RG_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
			.visibilityType = 
			    isfirstperson ? RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON :
		        isviewer ? RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER :
		        RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = paliashdr->numverts_vbo,
			.pVertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, shadevector, lightcolor),
			.indexCount = paliashdr->numindexes,
			.pIndices = e->model->rtindices,
			.layerColors = {RT_COLOR_WHITE},
			.layerBlendingTypes = {RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
			.geomMaterial = {tx ? tx->rtmaterial : RG_NO_MATERIAL},
			.defaultRoughness = CVAR_TO_FLOAT (rt_model_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_model_metal),
			.defaultEmission = 0,
			.transform = RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson),
		};

		RgResult r = rgUploadGeometry (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}

	Atomic_AddUInt32 (&rs_aliaspasses, paliashdr->numtris);
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr, int frame, lerpdata_t *lerpdata)
{
	int posenum, numposes;

	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
		frame = 0;
	}

	posenum = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		posenum += (int)(cl.time / e->lerptime) % numposes;
	}
	else
		e->lerptime = 0.1;

	if (e->lerpflags & LERP_RESETANIM) // kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) // defer lerping one more time
		{
			e->lerpstart = 0;
			e->previouspose = posenum;
			e->currentpose = posenum;
			e->lerpflags -= LERP_RESETANIM2;
		}
		else
		{
			e->lerpstart = cl.time;
			e->previouspose = e->currentpose;
			e->currentpose = posenum;
		}
	}

	// set up values
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		if (e->lerpflags & LERP_FINISH && numposes == 1)
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1);
		else
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / e->lerptime, 1);

		if (e->currentpose >= paliashdr->numposes || e->currentpose < 0)
		{
			Con_DPrintf ("R_AliasSetupFrame: invalid current pose %d (%d total) for '%s'\n", e->currentpose, paliashdr->numposes, e->model->name);
			e->currentpose = 0;
		}

		if (e->previouspose >= paliashdr->numposes || e->previouspose < 0)
		{
			Con_DPrintf ("R_AliasSetupFrame: invalid prev pose %d (%d total) for '%s'\n", e->previouspose, paliashdr->numposes, e->model->name);
			e->previouspose = e->currentpose;
		}

		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else // don't lerp
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata)
{
	float  blend;
	vec3_t d;
	int    i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
		e->movelerpstart = 0;
		VectorCopy (e->origin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->angles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
		e->lerpflags -= LERP_RESETMOVE;
	}
	else if (!VectorCompare (e->origin, e->currentorigin) || !VectorCompare (e->angles, e->currentangles)) // origin/angles changed, start new lerp
	{
		e->movelerpstart = cl.time;
		VectorCopy (e->currentorigin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
	}

	// set up values
	if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
	{
		if (e->lerpflags & LERP_FINISH)
			blend = CLAMP (0, (cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1);
		else
			blend = CLAMP (0, (cl.time - e->movelerpstart) / 0.1, 1);

		// translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		// rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180)
				d[i] -= 360;
			if (d[i] < -180)
				d[i] += 360;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else // don't lerp
	{
		VectorCopy (e->origin, lerpdata->origin);
		VectorCopy (e->angles, lerpdata->angles);
	}
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
static void R_SetupAliasLighting (entity_t *e, vec3_t *shadevector, vec3_t *lightcolor)
{
	vec3_t dist;
	float  add;
	int    i;
	int    quantizedangle;
	float  radiansangle;
	vec3_t lpos;

	VectorCopy (e->origin, lpos);
	// start the light trace from slightly above the origin
	// this helps with models whose origin is below ground level, but are otherwise visible
	// (e.g. some of the candles in the DOTM start map, which would otherwise appear black)
	lpos[2] += e->model->maxs[2] * 0.5f;
	R_LightPoint (lpos, &e->lightcache, lightcolor);

	// add dlights
	for (i = 0; i < MAX_DLIGHTS; i++)
	{
		if (cl_dlights[i].die >= cl.time)
		{
			VectorSubtract (e->origin, cl_dlights[i].origin, dist);
			add = cl_dlights[i].radius - VectorLength (dist);
			if (add > 0)
				VectorMA (*lightcolor, add, cl_dlights[i].color, *lightcolor);
		}
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
		if (add > 0.0f)
		{
			(*lightcolor)[0] += add / 3.0f;
			(*lightcolor)[1] += add / 3.0f;
			(*lightcolor)[2] += add / 3.0f;
		}
	}

	// minimum light value on players (8)
	if (e > cl.entities && e <= cl.entities + cl.maxclients)
	{
		add = 24.0f - ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
		if (add > 0.0f)
		{
			(*lightcolor)[0] += add / 3.0f;
			(*lightcolor)[1] += add / 3.0f;
			(*lightcolor)[2] += add / 3.0f;
		}
	}

	// clamp lighting so it doesn't overbright as much (96)
	add = 288.0f / ((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]);
	if (add < 1.0f)
		VectorScale ((*lightcolor), add, (*lightcolor));

	quantizedangle = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);

	// ericw -- shadevector is passed to the shader to compute shadedots inside the
	// shader, see GLAlias_CreateShaders()
	radiansangle = (quantizedangle / 16.0) * 2.0 * 3.14159;
	(*shadevector)[0] = cos (-radiansangle);
	(*shadevector)[1] = sin (-radiansangle);
	(*shadevector)[2] = 1;
	VectorNormalize (*shadevector);
	// ericw --

	VectorScale ((*lightcolor), 1.0f / 200.0f, (*lightcolor));
}

/*
=================
R_DrawAliasModel -- johnfitz -- almost completely rewritten
=================
*/
void R_DrawAliasModel (cb_context_t *cbx, entity_t *e, int entuniqueid)
{
	aliashdr_t  *paliashdr;
	int          anim, skinnum;
	gltexture_t *tx;
	lerpdata_t   lerpdata;
	qboolean     alphatest = !!(e->model->flags & MF_HOLEY);

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (e, paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	//
	// cull it
	//
	if (CVAR_TO_BOOL (rt_enable_pvs))
	{
		if (R_CullModelForEntity (e))
			return;
	}

	//
	// set up for alpha blending
	//
	float entalpha;
	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE (e->alpha);
	if (entalpha == 0)
		return;

	//
	// set up lighting
	//
	Atomic_AddUInt32 (&rs_aliaspolys, paliashdr->numtris);
	vec3_t shadevector, lightcolor;
	R_SetupAliasLighting (e, &shadevector, &lightcolor);

	//
	// set up textures
	//
	anim = (int)(cl.time * 10) & 3;
	skinnum = e->skinnum;
	if ((skinnum >= paliashdr->numskins) || (skinnum < 0))
	{
		Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e->model->name);
		// ericw -- display skin 0 for winquake compatibility
		skinnum = 0;
	}
	tx = paliashdr->gltextures[skinnum][anim];
	if (e->colormap != vid.colormap && !gl_nocolors.value)
		if ((uintptr_t)e >= (uintptr_t)&cl.entities[1] && (uintptr_t)e <= (uintptr_t)&cl.entities[cl.maxclients])
			tx = playertextures[e - cl.entities - 1];

	if (r_fullbright_cheatsafe)
	{
		lightcolor[0] = 0.5f;
		lightcolor[1] = 0.5f;
		lightcolor[2] = 0.5f;
	}
	if (r_lightmap_cheatsafe)
	{
		tx = whitetexture;
		if (r_fullbright.value)
		{
			lightcolor[0] = 1.0f;
			lightcolor[1] = 1.0f;
			lightcolor[2] = 1.0f;
		}
	}

	//
	// draw it
	//
	GL_DrawAliasFrame (cbx, e, paliashdr, lerpdata, tx, entalpha, alphatest, shadevector, lightcolor, entuniqueid);
}

// johnfitz -- values for shadow matrix
#define SHADOW_SKEW_X -0.7 // skew along x axis. -0.7 to mimic glquake shadows
#define SHADOW_SKEW_Y 0    // skew along y axis. 0 to mimic glquake shadows
#define SHADOW_VSCALE 0    // 0=completely flat
#define SHADOW_HEIGHT 0.1  // how far above the floor to render the shadow
// johnfitz

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris (cb_context_t *cbx, entity_t *e)
{
}
