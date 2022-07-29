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

// johnfitz -- new cvars
extern cvar_t r_flatlightstyles;
extern cvar_t r_lerplightstyles;
extern cvar_t gl_fullbrights;
extern cvar_t gl_farclip;
extern cvar_t r_waterquality;
extern cvar_t r_waterwarp;
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
	Cvar_RegisterVariable (&r_waterquality);
	Cvar_RegisterVariable (&r_waterwarp);
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
	Cvar_SetCallback (&r_lavaalpha, R_SetLavaalpha_f);
	Cvar_SetCallback (&r_telealpha, R_SetTelealpha_f);
	Cvar_SetCallback (&r_slimealpha, R_SetSlimealpha_f);

	Cvar_RegisterVariable (&r_gpulightmapupdate);
	Cvar_RegisterVariable (&r_tasks);
	Cvar_RegisterVariable (&r_parallelmark);
	Cvar_RegisterVariable (&r_usesops);

	R_InitParticles ();

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
	char        rtname[64];
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
	q_snprintf (rtname, sizeof (rtname), "player/%i", playernum);

	playertextures[playernum] = TexMgr_LoadImage (
		rtname,
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

extern atomic_uint32_t rt_require_static_submit;

/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int      i;

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
	// RT: submit world geometry once
	{
		Atomic_StoreUInt32 (&rt_require_static_submit, true);
	}
	GL_PrepareSIMDData ();
	// ericw -- no longer load alias models into a VBO here, it's done in Mod_LoadAliasModel

	r_framecount = 0;    // johnfitz -- paranoid?
	r_visframecount = 0; // johnfitz -- paranoid?

	Sky_NewMap ();        // johnfitz -- skybox in worldspawn
	Fog_NewMap ();        // johnfitz -- global fog in worldspawn
	R_ParseWorldspawn (); // ericw -- wateralpha, lavaalpha, telealpha, slimealpha in worldspawn
	RT_ParseElights ();
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



uint64_t RT_GetBrushSurfUniqueId (int entuniqueid, const qmodel_t *model, const msurface_t *surf)
{
	size_t surfindex = surf - model->surfaces;

	// look gl_model.c line 1327
    if (surfindex > 32767)
	{
		Con_DWarning ("%i faces exceeds standard limit of 32767.\n", surfindex);
	}

	return
        1ull << 60 |		// brush type
		surfindex << 32 |	// surface
	    entuniqueid;		// entity
}

uint64_t RT_GetAliasModelUniqueId(int entuniqueid)
{
	return
        2ull << 60 |		// model type
		entuniqueid;		// entity
}

uint64_t RT_GetSpriteModelUniqueId (int entuniqueid)
{
	return
        3ull << 60 | // model type
		entuniqueid; // entity
}



#define MODEL_MAT(i, j) (model_matrix[(i)*4 + (j)])

RgTransform RT_GetModelTransform(const float model_matrix[16])
{
	// right side should be 0, and translation values on bottom
	assert (
		fabsf (MODEL_MAT (0, 3)) < 0.001f && 
		fabsf (MODEL_MAT (1, 3)) < 0.001f && 
		fabsf (MODEL_MAT (2, 3)) < 0.001f);

	RgTransform t = {
		MODEL_MAT (0, 0), MODEL_MAT (1, 0), MODEL_MAT (2, 0), MODEL_MAT (3, 0), MODEL_MAT (0, 1), MODEL_MAT (1, 1),
		MODEL_MAT (2, 1), MODEL_MAT (3, 1), MODEL_MAT (0, 2), MODEL_MAT (1, 2), MODEL_MAT (2, 2), MODEL_MAT (3, 2),
	};

	return t;
}

#undef MODEL_MAT
