/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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
// r_main.c

#include "quakedef.h"
#include "tasks.h"
#include "atomics.h"

int r_visframecount; // bumped when going to a new PVS
int r_framecount;    // used for dlight push checking
atomic_uint32_t rt_require_static_submit;

mplane_t frustum[4];

qboolean render_warp;

// johnfitz -- rendering statistics
atomic_uint32_t rs_brushpolys, rs_aliaspolys, rs_skypolys, rs_particles, rs_fogpolys;
atomic_uint32_t rs_dynamiclightmaps, rs_brushpasses, rs_aliaspasses, rs_skypasses;

//
// view origin
//
vec3_t vup;
vec3_t vpn;
vec3_t vright;
vec3_t r_origin;

float r_fovx, r_fovy; // johnfitz -- rendering fov may be different becuase of r_waterwarp

//
// screen size info
//
refdef_t r_refdef;

mleaf_t *r_viewleaf, *r_oldviewleaf;

int d_lightstylevalue[256]; // 8.8 fraction of base light value

cvar_t r_drawentities = {"r_drawentities", "1", CVAR_NONE};
cvar_t r_drawviewmodel = {"r_drawviewmodel", "1", CVAR_NONE};
cvar_t r_speeds = {"r_speeds", "0", CVAR_NONE};
cvar_t r_pos = {"r_pos", "0", CVAR_NONE};
cvar_t r_fullbright = {"r_fullbright", "0", CVAR_NONE};
cvar_t r_lightmap = {"r_lightmap", "0", CVAR_NONE};
cvar_t r_wateralpha = {"r_wateralpha", "1", CVAR_ARCHIVE};
cvar_t r_dynamic = {"r_dynamic", "1", CVAR_ARCHIVE};
cvar_t r_novis = {"r_novis", "0", CVAR_ARCHIVE};
#if defined(USE_SIMD)
cvar_t r_simd = {"r_simd", "1", CVAR_ARCHIVE};
#endif

cvar_t gl_finish = {"gl_finish", "0", CVAR_NONE};
cvar_t gl_polyblend = {"gl_polyblend", "1", CVAR_NONE};
cvar_t gl_nocolors = {"gl_nocolors", "0", CVAR_NONE};

// johnfitz -- new cvars
cvar_t r_flatlightstyles = {"r_flatlightstyles", "0", CVAR_NONE};
cvar_t r_lerplightstyles = {"r_lerplightstyles", "1", CVAR_ARCHIVE}; // 0=off; 1=skip abrupt transitions; 2=always lerp
cvar_t gl_fullbrights = {"gl_fullbrights", "1", CVAR_ARCHIVE};
cvar_t gl_farclip = {"gl_farclip", "16384", CVAR_ARCHIVE};
cvar_t r_oldskyleaf = {"r_oldskyleaf", "0", CVAR_NONE};
cvar_t r_drawworld = {"r_drawworld", "1", CVAR_NONE};
cvar_t r_showtris = {"r_showtris", "0", CVAR_NONE};
cvar_t r_showbboxes = {"r_showbboxes", "0", CVAR_NONE};
cvar_t r_lerpmodels = {"r_lerpmodels", "1", CVAR_NONE};
cvar_t r_lerpmove = {"r_lerpmove", "1", CVAR_NONE};
cvar_t r_nolerp_list = {
	"r_nolerp_list",
	"progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/v_saw.mdl,progs/"
	"v_xfist.mdl,progs/h2stuff/newfire.mdl",
	CVAR_NONE};

extern cvar_t r_vfog;
// johnfitz

cvar_t gl_zfix = {"gl_zfix", "1", CVAR_ARCHIVE}; // QuakeSpasm z-fighting fix

cvar_t r_lavaalpha = {"r_lavaalpha", "0", CVAR_NONE};
cvar_t r_telealpha = {"r_telealpha", "0", CVAR_NONE};
cvar_t r_slimealpha = {"r_slimealpha", "0", CVAR_NONE};

float map_wateralpha, map_lavaalpha, map_telealpha, map_slimealpha;
float map_fallbackalpha;

qboolean r_drawworld_cheatsafe, r_fullbright_cheatsafe, r_lightmap_cheatsafe; // johnfitz

cvar_t r_gpulightmapupdate = {"r_gpulightmapupdate", "0", CVAR_NONE};

cvar_t r_tasks = {"r_tasks", "0", CVAR_NONE};

extern cvar_t rt_dlight_intensity;
extern cvar_t rt_dlight_radius;
extern cvar_t rt_flashlight;

/*
=================
R_CullBox -- johnfitz -- replaced with new function from lordhavoc

Returns true if the box is completely outside the frustum
=================
*/
qboolean R_CullBox (vec3_t emins, vec3_t emaxs)
{
	int       i;
	mplane_t *p;
	byte      signbits;
	float     vec[3];
	for (i = 0; i < 4; i++)
	{
		p = frustum + i;
		signbits = p->signbits;
		vec[0] = ((signbits % 2) < 1) ? emaxs[0] : emins[0];
		vec[1] = ((signbits % 4) < 2) ? emaxs[1] : emins[1];
		vec[2] = ((signbits % 8) < 4) ? emaxs[2] : emins[2];
		if (p->normal[0] * vec[0] + p->normal[1] * vec[1] + p->normal[2] * vec[2] < p->dist)
			return true;
	}
	return false;
}
/*
===============
R_CullModelForEntity -- johnfitz -- uses correct bounds based on rotation
===============
*/
qboolean R_CullModelForEntity (entity_t *e)
{
	vec3_t mins, maxs;

	if (e->angles[0] || e->angles[2]) // pitch or roll
	{
		VectorAdd (e->origin, e->model->rmins, mins);
		VectorAdd (e->origin, e->model->rmaxs, maxs);
	}
	else if (e->angles[1]) // yaw
	{
		VectorAdd (e->origin, e->model->ymins, mins);
		VectorAdd (e->origin, e->model->ymaxs, maxs);
	}
	else // no rotation
	{
		VectorAdd (e->origin, e->model->mins, mins);
		VectorAdd (e->origin, e->model->maxs, maxs);
	}

	return R_CullBox (mins, maxs);
}

/*
===============
R_RotateForEntity -- johnfitz -- modified to take origin and angles instead of pointer to entity
===============
*/
#define DEG2RAD(a) ((a)*M_PI_DIV_180)
void R_RotateForEntity (float matrix[16], vec3_t origin, vec3_t angles)
{
	float translation_matrix[16];
	TranslationMatrix (translation_matrix, origin[0], origin[1], origin[2]);
	MatrixMultiply (matrix, translation_matrix);

	float rotation_matrix[16];
	RotationMatrix (rotation_matrix, DEG2RAD (angles[1]), 0, 0, 1);
	MatrixMultiply (matrix, rotation_matrix);
	RotationMatrix (rotation_matrix, DEG2RAD (-angles[0]), 0, 1, 0);
	MatrixMultiply (matrix, rotation_matrix);
	RotationMatrix (rotation_matrix, DEG2RAD (angles[2]), 1, 0, 0);
	MatrixMultiply (matrix, rotation_matrix);
}

//==============================================================================
//
// SETUP FRAME
//
//==============================================================================

int SignbitsForPlane (mplane_t *out)
{
	int bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j = 0; j < 3; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1 << j;
	}
	return bits;
}

/*
===============
TurnVector -- johnfitz

turn forward towards side on the plane defined by forward and side
if angle = 90, the result will be equal to side
assumes side and forward are perpendicular, and normalized
to turn away from side, use a negative angle
===============
*/
#define DEG2RAD(a) ((a)*M_PI_DIV_180)
void TurnVector (vec3_t out, const vec3_t forward, const vec3_t side, float angle)
{
	float scale_forward, scale_side;

	scale_forward = cos (DEG2RAD (angle));
	scale_side = sin (DEG2RAD (angle));

	out[0] = scale_forward * forward[0] + scale_side * side[0];
	out[1] = scale_forward * forward[1] + scale_side * side[1];
	out[2] = scale_forward * forward[2] + scale_side * side[2];
}

/*
===============
R_SetFrustum -- johnfitz -- rewritten
===============
*/
void R_SetFrustum (float fovx, float fovy)
{
	int i;

	TurnVector (frustum[0].normal, vpn, vright, fovx / 2 - 90); // right plane
	TurnVector (frustum[1].normal, vpn, vright, 90 - fovx / 2); // left plane
	TurnVector (frustum[2].normal, vpn, vup, 90 - fovy / 2);    // bottom plane
	TurnVector (frustum[3].normal, vpn, vup, fovy / 2 - 90);    // top plane

	for (i = 0; i < 4; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_origin, frustum[i].normal); // FIXME: shouldn't this always be zero?
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}

#define NEARCLIP 4
float GL_GetCameraNear (float radfovx, float radfovy)
{
	const float w = 1.0f / tanf (radfovx * 0.5f);
	const float h = 1.0f / tanf (radfovy * 0.5f);

    // reduce near clip distance at high FOV's to avoid seeing through walls
    const float d = 12.f * q_min (w, h);
    return CLAMP (0.5f, d, NEARCLIP);
}

float GL_GetCameraFar (void)
{
	return gl_farclip.value;
}

/*
=============
GL_FrustumMatrix
=============
*/
static void GL_FrustumMatrix (float matrix[16], float radfovx, float radfovy)
{
	const float w = 1.0f / tanf (radfovx * 0.5f);
	const float h = 1.0f / tanf (radfovy * 0.5f);

	const float n = GL_GetCameraNear (radfovx, radfovy);
	const float f = GL_GetCameraFar ();

	memset (matrix, 0, 16 * sizeof (float));

	// First column
	matrix[0 * 4 + 0] = w;

	// Second column
	matrix[1 * 4 + 1] = -h;

	// Third column
	matrix[2 * 4 + 2] = f / (f - n) - 1.0f;
	matrix[2 * 4 + 3] = -1.0f;

	// Fourth column
	matrix[3 * 4 + 2] = (n * f) / (f - n);
}

/*
=============
R_SetupMatrices
=============
*/
static void R_SetupMatrices ()
{
	// Projection matrix
	GL_FrustumMatrix (vulkan_globals.projection_matrix, DEG2RAD (r_fovx), DEG2RAD (r_fovy));

	// View matrix
	float rotation_matrix[16];
	RotationMatrix (vulkan_globals.view_matrix, -M_PI / 2.0f, 1.0f, 0.0f, 0.0f);
	RotationMatrix (rotation_matrix, M_PI / 2.0f, 0.0f, 0.0f, 1.0f);
	MatrixMultiply (vulkan_globals.view_matrix, rotation_matrix);
	RotationMatrix (rotation_matrix, DEG2RAD (-r_refdef.viewangles[2]), 1.0f, 0.0f, 0.0f);
	MatrixMultiply (vulkan_globals.view_matrix, rotation_matrix);
	RotationMatrix (rotation_matrix, DEG2RAD (-r_refdef.viewangles[0]), 0.0f, 1.0f, 0.0f);
	MatrixMultiply (vulkan_globals.view_matrix, rotation_matrix);
	RotationMatrix (rotation_matrix, DEG2RAD (-r_refdef.viewangles[1]), 0.0f, 0.0f, 1.0f);
	MatrixMultiply (vulkan_globals.view_matrix, rotation_matrix);

	float translation_matrix[16];
	TranslationMatrix (translation_matrix, -r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);
	MatrixMultiply (vulkan_globals.view_matrix, translation_matrix);

	// View projection matrix
	memcpy (vulkan_globals.view_projection_matrix, vulkan_globals.projection_matrix, 16 * sizeof (float));
	MatrixMultiply (vulkan_globals.view_projection_matrix, vulkan_globals.view_matrix);
}

/*
=============
R_SetupContext
=============
*/
static void R_SetupContext (cb_context_t *cbx)
{
	GL_Viewport (
		cbx, glx + r_refdef.vrect.x, gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height, r_refdef.vrect.width, r_refdef.vrect.height, 0.0f, 1.0f);
}

static void RT_UploadAllDlights ()
{
	for (int i = 0; i < MAX_DLIGHTS; i++)
	{
		const dlight_t *l = &cl_dlights[i];

		if (l->die < cl.time || !l->radius)
		{
			continue;
		}

		float falloff_mult = QUAKEUNIT_TO_METRIC (l->radius);

		vec3_t color = {l->color[0], l->color[1], l->color[2]};
		VectorScale (color, CVAR_TO_FLOAT (rt_dlight_intensity), color);
		VectorScale (color, RT_QUAKE_LIGHT_AREA_INTENSITY_FIX, color);
		VectorScale (color, falloff_mult, color);

		RgSphericalLightUploadInfo info = {
			.uniqueID = i,
			.color = {color[0], color[1], color[2]},
			.position = {l->origin[0], l->origin[1], l->origin[2]},
			.radius = METRIC_TO_GOLDSRCUNIT (CVAR_TO_FLOAT (rt_dlight_radius) ),
		};

		RgResult r = rgUploadSphericalLight (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}

	if (CVAR_TO_FLOAT (rt_flashlight) > 0.1f)
	{
		vec3_t pos;
		VectorCopy (r_origin, pos);
		VectorMA (pos, METRIC_TO_GOLDSRCUNIT (-0.3f), vup, pos);
		VectorMA (pos, METRIC_TO_GOLDSRCUNIT (-0.4f), vright, pos);

		vec3_t color = {1.0f, 0.92f, 0.82f};
		VectorScale (color, CVAR_TO_FLOAT (rt_flashlight), color);
		VectorScale (color, RT_QUAKE_LIGHT_AREA_INTENSITY_FIX, color);

		RgSpotlightUploadInfo info = {
			.uniqueID = MAX_DLIGHTS,
			.color = {color[0], color[1], color[2]},
			.position = {pos[0], pos[1], pos[2]},
			.direction = {vpn[0], vpn[1], vpn[2]},
			.radius = METRIC_TO_GOLDSRCUNIT (0.1f),
			.angleOuter = DEG2RAD (30),
			.angleInner = 0,
		};

		RgResult r = rgUploadSpotlightLight (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}
}

/*
===============
R_SetupViewBeforeMark
===============
*/
void R_SetupViewBeforeMark (void *unused)
{
	// Need to do those early because we now update dynamic light maps during R_MarkSurfaces
	if (!r_gpulightmapupdate.value)
		R_PushDlights ();
	R_AnimateLight ();

	// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	// johnfitz -- calculate r_fovx and r_fovy here
	r_fovx = r_refdef.fov_x;
	r_fovy = r_refdef.fov_y;
	render_warp = false;

	if (r_waterwarp.value)
	{
		int contents = Mod_PointInLeaf (r_origin, cl.worldmodel)->contents;
		if (contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA)
		{
			if (r_waterwarp.value == 1)
				render_warp = true;
			else
			{
				// variance is a percentage of width, where width = 2 * tan(fov / 2) otherwise the effect is too dramatic at high FOV and too subtle at low FOV.
				// what a mess!
				r_fovx = atan (tan (DEG2RAD (r_refdef.fov_x) / 2) * (0.97 + sin (cl.time * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
				r_fovy = atan (tan (DEG2RAD (r_refdef.fov_y) / 2) * (1.03 - sin (cl.time * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
			}
		}
	}
	// johnfitz

	R_SetFrustum (r_fovx, r_fovy); // johnfitz -- use r_fov* vars
	R_SetupMatrices ();

	// johnfitz -- cheat-protect some draw modes
	r_fullbright_cheatsafe = false;
	r_lightmap_cheatsafe = false;
	r_drawworld_cheatsafe = true;
	if (cl.maxclients == 1)
	{
		if (!r_drawworld.value)
			r_drawworld_cheatsafe = false;
		if (r_lightmap.value)
			r_lightmap_cheatsafe = true;
		else if (r_fullbright.value)
			r_fullbright_cheatsafe = true;
	}
	if (!cl.worldmodel->lightdata)
	{
		r_fullbright_cheatsafe = true;
		r_lightmap_cheatsafe = false;
	}
	// johnfitz

	RT_UploadAllDlights ();
}

//==============================================================================
//
// RENDER VIEW
//
//==============================================================================

/*
=============
R_DrawEntitiesOnList
=============
*/
void R_DrawEntitiesOnList (cb_context_t *cbx, qboolean alphapass, int chain, int startedict, int endedict) // johnfitz -- added parameter
{
	int i;

	if (!r_drawentities.value)
		return;

	R_BeginDebugUtilsLabel (cbx, alphapass ? "Entities Alpha Pass" : "Entities");
	// johnfitz -- sprites are not a special case
	for (i = startedict; i < endedict; ++i)
	{
		entity_t *currententity = cl_visedicts[i];

		// johnfitz -- if alphapass is true, draw only alpha entites this time
		// if alphapass is false, draw only nonalpha entities this time
		if ((ENTALPHA_DECODE (currententity->alpha) < 1 && !alphapass) || (ENTALPHA_DECODE (currententity->alpha) == 1 && alphapass))
			continue;

		// johnfitz -- chasecam
		if (currententity == &cl.entities[cl.viewentity])
			currententity->angles[0] *= 0.3;
		// johnfitz

		// spike -- this would be more efficient elsewhere, but its more correct here.
		if (currententity->eflags & EFLAGS_EXTERIORMODEL)
			continue;

		switch (currententity->model->type)
		{
		case mod_alias:
			R_DrawAliasModel (cbx, currententity, i);
			break;
		case mod_brush:
			R_DrawBrushModel (cbx, currententity, chain, i);
			break;
		case mod_sprite:
			R_DrawSpriteModel (cbx, currententity, i);
			break;
		}
	}
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawViewModel -- johnfitz -- gutted
=============
*/
void R_DrawViewModel (cb_context_t *cbx)
{
	if (!r_drawviewmodel.value || !r_drawentities.value || chase_active.value)
		return;

	if (cl.items & IT_INVISIBILITY || cl.stats[STAT_HEALTH] <= 0)
		return;

	entity_t *currententity = &cl.viewent;
	if (!currententity->model)
		return;

	// johnfitz -- this fixes a crash
	if (currententity->model->type != mod_alias)
		return;
	// johnfitz

	R_BeginDebugUtilsLabel (cbx, "View Model");

	// hack the depth range to prevent view model from poking into walls
	GL_Viewport (
		cbx, glx + r_refdef.vrect.x, gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height, r_refdef.vrect.width, r_refdef.vrect.height, 0.7f, 1.0f);

	R_DrawAliasModel (cbx, currententity, ENT_UNIQUEID_VIEWMODEL);

	GL_Viewport (
		cbx, glx + r_refdef.vrect.x, gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height, r_refdef.vrect.width, r_refdef.vrect.height, 0.0f, 1.0f);

	R_EndDebugUtilsLabel (cbx);
}

/*
================
R_EmitWirePoint -- johnfitz -- draws a wireframe cross shape for point entities
================
*/
void R_EmitWirePoint (cb_context_t *cbx, vec3_t origin)
{
	const int size = 8;

	RgVertex vertices[6] = {0};

	vertices[0].position[0] = origin[0] - size;
	vertices[0].position[1] = origin[1];
	vertices[0].position[2] = origin[2];
	vertices[1].position[0] = origin[0] + size;
	vertices[1].position[1] = origin[1];
	vertices[1].position[2] = origin[2];
	vertices[2].position[0] = origin[0];
	vertices[2].position[1] = origin[1] - size;
	vertices[2].position[2] = origin[2];
	vertices[3].position[0] = origin[0];
	vertices[3].position[1] = origin[1] + size;
	vertices[3].position[2] = origin[2];
	vertices[4].position[0] = origin[0];
	vertices[4].position[1] = origin[1];
	vertices[4].position[2] = origin[2] - size;
	vertices[5].position[0] = origin[0];
	vertices[5].position[1] = origin[1];
	vertices[5].position[2] = origin[2] + size;

	for (int i = 0; i < (int)countof (vertices); i++)
	{
		vertices[i].packedColor = RT_PackColorToUint32 (255, 255, 255, 255);
	}

	RgRasterizedGeometryUploadInfo info = {
		.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
		.vertexCount = countof (vertices),
		.pVertices = vertices,
		.indexCount = 0,
		.pIndices = NULL,
		.transform = RT_TRANSFORM_IDENTITY,
		.color = RT_COLOR_WHITE,
		.material = RG_NO_MATERIAL,
		.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST,
		.blendFuncSrc = 0,
		.blendFuncDst = 0,
	};

	RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
	RG_CHECK (r);
}

/*
================
R_EmitWireBox -- johnfitz -- draws one axis aligned bounding box
================
*/
void R_EmitWireBox (cb_context_t *cbx, vec3_t mins, vec3_t maxs)
{
	const static uint32_t box_indices[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 4, 1, 5, 2, 6, 3, 7, 0, 2, 1, 3, 4, 6, 5, 7};

	RgVertex vertices[8] = {0};

	for (int i = 0; i < 8; ++i)
	{
		vertices[i].position[0] = ((i % 2) < 1) ? mins[0] : maxs[0];
		vertices[i].position[1] = ((i % 4) < 2) ? mins[1] : maxs[1];
		vertices[i].position[2] = ((i % 8) < 4) ? mins[2] : maxs[2];
		vertices[i].packedColor = RT_PackColorToUint32 (255, 255, 255, 255);
	}

	RgRasterizedGeometryUploadInfo info = {
		.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
		.vertexCount = countof (vertices),
		.pVertices = vertices,
		.indexCount = countof (box_indices),
		.pIndices = box_indices,
		.transform = RT_TRANSFORM_IDENTITY,
		.color = RT_COLOR_WHITE,
		.material = RG_NO_MATERIAL,
		.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST,
		.blendFuncSrc = 0,
		.blendFuncDst = 0,
	};

	RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
	RG_CHECK (r);
}

/*
================
R_ShowBoundingBoxes -- johnfitz

draw bounding boxes -- the server-side boxes, not the renderer cullboxes
================
*/
void R_ShowBoundingBoxes (cb_context_t *cbx)
{
	extern edict_t *sv_player;
	vec3_t          mins, maxs;
	edict_t        *ed;
	int             i;

	if (!r_showbboxes.value || cl.maxclients > 1 || !r_drawentities.value || !sv.active)
		return;

	R_BeginDebugUtilsLabel (cbx, "show bboxes");

	PR_SwitchQCVM (&sv.qcvm);
	for (i = 0, ed = NEXT_EDICT (qcvm->edicts); i < qcvm->num_edicts; i++, ed = NEXT_EDICT (ed))
	{
		if (ed == sv_player)
			continue; // don't draw player's own bbox

		if (ed->v.mins[0] == ed->v.maxs[0] && ed->v.mins[1] == ed->v.maxs[1] && ed->v.mins[2] == ed->v.maxs[2])
		{
			// point entity
			R_EmitWirePoint (cbx, ed->v.origin);
		}
		else
		{
			// box entity
			VectorAdd (ed->v.mins, ed->v.origin, mins);
			VectorAdd (ed->v.maxs, ed->v.origin, maxs);
			R_EmitWireBox (cbx, mins, maxs);
		}
	}
	PR_SwitchQCVM (NULL);

	R_EndDebugUtilsLabel (cbx);
}

/*
================
R_ShowTris -- johnfitz
================
*/
void R_ShowTris (cb_context_t *cbx)
{
	extern cvar_t r_particles;
	int           i;

	if (r_showtris.value < 1 || r_showtris.value > 2 || cl.maxclients > 1 || !vulkan_globals.non_solid_fill)
		return;

	R_BeginDebugUtilsLabel (cbx, "show tris");
	if (r_drawworld.value)
		R_DrawWorld_ShowTris (cbx);

	if (r_drawentities.value)
	{
		for (i = 0; i < cl_numvisedicts; i++)
		{
			entity_t *currententity = cl_visedicts[i];

			if (currententity == &cl.entities[cl.viewentity]) // chasecam
				currententity->angles[0] *= 0.3;

			switch (currententity->model->type)
			{
			case mod_brush:
				R_DrawBrushModel_ShowTris (cbx, currententity);
				break;
			case mod_alias:
				R_DrawAliasModel_ShowTris (cbx, currententity);
				break;
			case mod_sprite:
				R_DrawSpriteModel_ShowTris (cbx, currententity);
				break;
			default:
				break;
			}
		}

		// viewmodel
		entity_t *currententity = &cl.viewent;
		if (r_drawviewmodel.value && !chase_active.value && cl.stats[STAT_HEALTH] > 0 && !(cl.items & IT_INVISIBILITY) && currententity->model &&
		    currententity->model->type == mod_alias)
		{
			R_DrawAliasModel_ShowTris (cbx, currententity);
		}
	}

	if (r_particles.value)
	{
		R_DrawParticles_ShowTris (cbx);
#ifdef PSET_SCRIPT
		PScript_DrawParticles_ShowTris (cbx);
#endif
	}

	R_EndDebugUtilsLabel (cbx);
}

/*
================
R_DrawWorldTask
================
*/
void R_DrawWorldTask (int index, void *unused)
{
	if (!Atomic_LoadUInt32 (&rt_require_static_submit))
	{
		return;
	}

	RgResult r;
	
	r = rgStartNewScene (vulkan_globals.instance);
	RG_CHECK (r);

	const int     cbx_index = index + CBX_WORLD_0;
	cb_context_t *cbx = &vulkan_globals.secondary_cb_contexts[cbx_index];
	R_SetupContext (cbx);
	Fog_EnableGFog (cbx);
	R_DrawWorld (cbx, index);

	r = rgSubmitStaticGeometries (vulkan_globals.instance);
	RG_CHECK (r);

	Atomic_StoreUInt32 (&rt_require_static_submit, false);
}

/*
================
R_DrawSkyAndWaterTask
================
*/
static void R_DrawSkyAndWaterTask (void *unused)
{
	R_SetupContext (&vulkan_globals.secondary_cb_contexts[CBX_SKY_AND_WATER]);
	Fog_EnableGFog (&vulkan_globals.secondary_cb_contexts[CBX_SKY_AND_WATER]);
	Sky_DrawSky (&vulkan_globals.secondary_cb_contexts[CBX_SKY_AND_WATER]);
	R_DrawWorld_Water (&vulkan_globals.secondary_cb_contexts[CBX_SKY_AND_WATER]);
}

/*
================
R_DrawEntitiesTask
================
*/
static void R_DrawEntitiesTask (int index, void *unused)
{
	const int cbx_index = index + CBX_ENTITIES_0;
	R_SetupContext (&vulkan_globals.secondary_cb_contexts[cbx_index]);
	Fog_EnableGFog (&vulkan_globals.secondary_cb_contexts[cbx_index]); // johnfitz
	const int num_edicts_per_cb = (cl_numvisedicts + NUM_ENTITIES_CBX - 1) / NUM_ENTITIES_CBX;
	int       startedict = index * num_edicts_per_cb;
	int       endedict = q_min ((index + 1) * num_edicts_per_cb, cl_numvisedicts);
	R_DrawEntitiesOnList (&vulkan_globals.secondary_cb_contexts[cbx_index], false, index + chain_model_0, startedict, endedict);
}

/*
================
R_DrawAlphaEntitiesTask
================
*/
static void R_DrawAlphaEntitiesTask (void *unused)
{
	R_SetupContext (&vulkan_globals.secondary_cb_contexts[CBX_ALPHA_ENTITIES]);
	Fog_EnableGFog (&vulkan_globals.secondary_cb_contexts[CBX_ALPHA_ENTITIES]);
	R_DrawEntitiesOnList (
		&vulkan_globals.secondary_cb_contexts[CBX_ALPHA_ENTITIES], true, chain_alpha_model, 0,
		cl_numvisedicts); // johnfitz -- true means this is the pass for alpha entities
}

/*
================
R_DrawParticlesTask
================
*/
static void R_DrawParticlesTask (void *unused)
{
	R_SetupContext (&vulkan_globals.secondary_cb_contexts[CBX_PARTICLES]);
	Fog_EnableGFog (&vulkan_globals.secondary_cb_contexts[CBX_PARTICLES]); // johnfitz
	R_DrawParticles (&vulkan_globals.secondary_cb_contexts[CBX_PARTICLES]);
#ifdef PSET_SCRIPT
	PScript_DrawParticles (&vulkan_globals.secondary_cb_contexts[CBX_PARTICLES]);
#endif
}

/*
================
R_DrawViewModelTask
================
*/
static void R_DrawViewModelTask (void *unused)
{
	R_SetupContext (&vulkan_globals.secondary_cb_contexts[CBX_VIEW_MODEL]);
	R_DrawViewModel (&vulkan_globals.secondary_cb_contexts[CBX_VIEW_MODEL]);     // johnfitz -- moved here from R_RenderView
	R_ShowTris (&vulkan_globals.secondary_cb_contexts[CBX_VIEW_MODEL]);          // johnfitz
	R_ShowBoundingBoxes (&vulkan_globals.secondary_cb_contexts[CBX_VIEW_MODEL]); // johnfitz
}

/*
================
R_RenderView
================
*/
void R_RenderView (qboolean use_tasks, task_handle_t begin_rendering_task, task_handle_t setup_frame_task, task_handle_t draw_done_task)
{
	double time1, time2;

	if (!cl.worldmodel)
		Sys_Error ("R_RenderView: NULL worldmodel");

	time1 = 0; /* avoid compiler warning */
	if (r_speeds.value)
	{
		time1 = Sys_DoubleTime ();

		// johnfitz -- rendering statistics
		Atomic_StoreUInt32 (&rs_brushpolys, 0u);
		Atomic_StoreUInt32 (&rs_aliaspolys, 0u);
		Atomic_StoreUInt32 (&rs_skypolys, 0u);
		Atomic_StoreUInt32 (&rs_particles, 0u);
		Atomic_StoreUInt32 (&rs_fogpolys, 0u);
		Atomic_StoreUInt32 (&rs_dynamiclightmaps, 0u);
		Atomic_StoreUInt32 (&rs_aliaspasses, 0u);
		Atomic_StoreUInt32 (&rs_skypasses, 0u);
		Atomic_StoreUInt32 (&rs_brushpasses, 0u);
	}

	if (use_tasks)
	{
		task_handle_t before_mark = Task_AllocateAndAssignFunc (R_SetupViewBeforeMark, NULL, 0);
		Task_AddDependency (setup_frame_task, before_mark);

		task_handle_t store_efrags = INVALID_TASK_HANDLE;
		task_handle_t cull_surfaces = INVALID_TASK_HANDLE;
		task_handle_t chain_surfaces = INVALID_TASK_HANDLE;
		R_MarkSurfaces (use_tasks, before_mark, &store_efrags, &cull_surfaces, &chain_surfaces);

		task_handle_t draw_world_task = Task_AllocateAndAssignIndexedFunc (R_DrawWorldTask, NUM_WORLD_CBX, NULL, 0);
		Task_AddDependency (chain_surfaces, draw_world_task);
		Task_AddDependency (begin_rendering_task, draw_world_task);
		Task_AddDependency (draw_world_task, draw_done_task);

		task_handle_t draw_sky_and_water_task = Task_AllocateAndAssignFunc (R_DrawSkyAndWaterTask, NULL, 0);
		Task_AddDependency (store_efrags, draw_sky_and_water_task);
		Task_AddDependency (chain_surfaces, draw_sky_and_water_task);
		Task_AddDependency (begin_rendering_task, draw_sky_and_water_task);
		Task_AddDependency (draw_sky_and_water_task, draw_done_task);

		task_handle_t draw_view_model_task = Task_AllocateAndAssignFunc (R_DrawViewModelTask, NULL, 0);
		Task_AddDependency (before_mark, draw_view_model_task);
		Task_AddDependency (begin_rendering_task, draw_view_model_task);
		Task_AddDependency (draw_view_model_task, draw_done_task);

		task_handle_t draw_entities_task = Task_AllocateAndAssignIndexedFunc (R_DrawEntitiesTask, NUM_ENTITIES_CBX, NULL, 0);
		Task_AddDependency (store_efrags, draw_entities_task);
		Task_AddDependency (begin_rendering_task, draw_entities_task);
		Task_AddDependency (draw_entities_task, draw_done_task);

		task_handle_t draw_alpha_entities_task = Task_AllocateAndAssignFunc (R_DrawAlphaEntitiesTask, NULL, 0);
		Task_AddDependency (store_efrags, draw_alpha_entities_task);
		Task_AddDependency (begin_rendering_task, draw_alpha_entities_task);
		Task_AddDependency (draw_alpha_entities_task, draw_done_task);

		task_handle_t draw_particles_task = Task_AllocateAndAssignFunc (R_DrawParticlesTask, NULL, 0);
		Task_AddDependency (before_mark, draw_particles_task);
		Task_AddDependency (begin_rendering_task, draw_particles_task);
		Task_AddDependency (draw_particles_task, draw_done_task);

		task_handle_t update_lightmaps_task = Task_AllocateAndAssignFunc (R_UpdateLightmaps, NULL, 0);
		Task_AddDependency (cull_surfaces, update_lightmaps_task);
		Task_AddDependency (begin_rendering_task, update_lightmaps_task);
		Task_AddDependency (update_lightmaps_task, draw_done_task);

		// RT: no need for draw_world_task, as it's done on R_NewMap
		task_handle_t tasks[] = {before_mark,          store_efrags,		                         draw_world_task,     draw_sky_and_water_task,
		                         draw_view_model_task, draw_entities_task, draw_alpha_entities_task, draw_particles_task, update_lightmaps_task};
		Tasks_Submit ((sizeof (tasks) / sizeof (task_handle_t)), tasks);
		if (store_efrags != cull_surfaces)
		{
			Task_Submit (cull_surfaces);
			Task_Submit (chain_surfaces);
		}
	}
	else
	{
		R_SetupViewBeforeMark (NULL);
		R_MarkSurfaces (use_tasks, INVALID_TASK_HANDLE, NULL, NULL, NULL); // johnfitz -- create texture chains from PVS
		R_DrawWorldTask (0, NULL);
		R_DrawSkyAndWaterTask (NULL);
		for (int i = 0; i < NUM_ENTITIES_CBX; ++i)
			R_DrawEntitiesTask (i, NULL);
		R_DrawAlphaEntitiesTask (NULL);
		R_DrawParticlesTask (NULL);
		R_DrawViewModelTask (NULL);
		if (r_gpulightmapupdate.value)
			R_UpdateLightmaps (NULL);
	}

	// johnfitz

	// johnfitz -- modified r_speeds output
	time2 = Sys_DoubleTime ();
	if (r_pos.value)
		Con_Printf (
			"x %i y %i z %i (pitch %i yaw %i roll %i)\n", (int)cl.entities[cl.viewentity].origin[0], (int)cl.entities[cl.viewentity].origin[1],
			(int)cl.entities[cl.viewentity].origin[2], (int)cl.viewangles[PITCH], (int)cl.viewangles[YAW], (int)cl.viewangles[ROLL]);
	else if (r_speeds.value == 2)
		Con_Printf (
			"%6.3f ms  %4u/%4u wpoly %4u/%4u epoly %3u lmap %4u/%4u sky\n", (time2 - time1) * 1000.0, rs_brushpolys, rs_brushpasses, rs_aliaspolys,
			rs_aliaspasses, rs_dynamiclightmaps, rs_skypolys, rs_skypasses);
	else if (r_speeds.value)
		Con_Printf ("%3i ms  %4i wpoly %4i epoly %3i lmap\n", (int)((time2 - time1) * 1000), rs_brushpolys, rs_aliaspolys, rs_dynamiclightmaps);
	// johnfitz
}
