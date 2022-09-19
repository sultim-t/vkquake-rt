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
// r_world.c: world model rendering

#include "quakedef.h"
#include "atomics.h"

extern cvar_t gl_fullbrights;
extern cvar_t r_drawflat;
extern cvar_t r_oldskyleaf;
extern cvar_t r_showtris;
extern cvar_t r_simd;
extern cvar_t gl_zfix;
extern cvar_t r_gpulightmapupdate;

extern cvar_t rt_brush_metal;
extern cvar_t rt_brush_rough;
extern cvar_t rt_enable_pvs;
extern cvar_t rt_reflrefr_depth;
extern cvar_t rt_dlight_intensity;
extern cvar_t rt_elight_radius;
extern cvar_t rt_wlight_intensity, rt_wlight_radius;

cvar_t r_parallelmark = {"r_parallelmark", "1", CVAR_NONE};

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

static int world_texstart[NUM_WORLD_CBX];
static int world_texend[NUM_WORLD_CBX];

extern RgVertex *rtallbrushvertices;

#define MAX_WORLDLIGHTS_COUNT 1024
static RgPolygonalLightUploadInfo rt_wldlights_tri[MAX_WORLDLIGHTS_COUNT];
static RgSphericalLightUploadInfo rt_wldlights_sph[MAX_WORLDLIGHTS_COUNT];
static int                        rt_wldlights_count = 0;

#define RT_CUSTOMLIGHTS_PATH        RT_OVERRIDEN_FOLDER "world_custom_lights.txt"
#define RT_CUSTOMLIGHTS_PATH_BACKUP RT_OVERRIDEN_FOLDER "world_custom_lights - Backup.txt"
typedef struct rt_worldcustomlight_t
{
	char      mapname[64];
	// color is in [0,1]
	RgFloat3D color01;
	RgFloat3D position;
	qboolean  deleted;
} rt_worldcustomlight_t;
static rt_worldcustomlight_t *rt_customlights_all = NULL;
static int                    rt_customlights_all_count = 0;
static int                   *rt_customlights_curr = NULL;
static int                    rt_customlights_curr_count = 0;

// RT: remove SIMD here, as the culling is not requires
#undef USE_SIMD
#undef USE_SSE2

/*
===============
mark_surfaces_state_t
===============
*/
typedef struct
{
#if defined(USE_SIMD)
	__m128 frustum_px[4];
	__m128 frustum_py[4];
	__m128 frustum_pz[4];
	__m128 frustum_pd[4];
	__m128 vieworg_px;
	__m128 vieworg_py;
	__m128 vieworg_pz;
	int    frustum_ofsx[4];
	int    frustum_ofsy[4];
	int    frustum_ofsz[4];
#endif
	byte *vis;
} mark_surfaces_state_t;
mark_surfaces_state_t mark_surfaces_state;

//==============================================================================
//
// SETUP CHAINS
//
//==============================================================================

/*
================
R_ClearTextureChains -- ericw

clears texture chains for all textures used by the given model, and also
clears the lightmap chains
================
*/
void R_ClearTextureChains (qmodel_t *mod, texchain_t chain)
{
	int i;

	// set all chains to null
	for (i = 0; i < mod->numtextures; i++)
	{
		if (mod->textures[i])
		{
			mod->textures[i]->texturechains[chain] = NULL;
			mod->textures[i]->chain_size[chain] = 0;
		}
	}
}

/*
================
R_ChainSurface -- ericw -- adds the given surface to its texture chain
================
*/
void R_ChainSurface (msurface_t *surf, texchain_t chain)
{
	surf->texturechains[chain] = surf->texinfo->texture->texturechains[chain];
	surf->texinfo->texture->texturechains[chain] = surf;
	surf->texinfo->texture->chain_size[chain] += 1;
}

/*
================
R_BackFaceCull -- johnfitz -- returns true if the surface is facing away from vieworg
================
*/
static inline qboolean R_BackFaceCull (msurface_t *surf)
{
	double dot;

	if (surf->plane->type < 3)
		dot = r_refdef.vieworg[surf->plane->type] - surf->plane->dist;
	else
		dot = DotProduct (r_refdef.vieworg, surf->plane->normal) - surf->plane->dist;

	if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
		return true;

	return false;
}

/*
===============
R_SetupWorldCBXTexRanges
===============
*/
void R_SetupWorldCBXTexRanges (qboolean use_tasks)
{
	memset (world_texstart, 0, sizeof (world_texstart));
	memset (world_texend, 0, sizeof (world_texend));

	const int num_textures = cl.worldmodel->numtextures;
	if (!use_tasks)
	{
		world_texstart[0] = 0;
		world_texend[0] = num_textures;
		return;
	}

	int total_world_surfs = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;
		total_world_surfs += t->chain_size[chain_world];
	}

	const int num_surfs_per_cbx = (total_world_surfs + NUM_WORLD_CBX - 1) / NUM_WORLD_CBX;
	int       current_cbx = 0;
	int       num_assigned_to_cbx = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;
		assert (current_cbx < NUM_WORLD_CBX);
		world_texend[current_cbx] = i + 1;
		num_assigned_to_cbx += t->chain_size[chain_world];
		if (num_assigned_to_cbx >= num_surfs_per_cbx)
		{
			current_cbx += 1;
			if (current_cbx < NUM_WORLD_CBX)
			{
				world_texstart[current_cbx] = i + 1;
			}
			num_assigned_to_cbx = 0;
		}
	}
}

#ifdef USE_SSE2
/*
===============
R_BackFaceCullSIMD

Performs backface culling for 32 planes
===============
*/
static FORCE_INLINE uint32_t R_BackFaceCullSIMD (soa_plane_t *planes)
{
	__m128 px = mark_surfaces_state.vieworg_px;
	__m128 py = mark_surfaces_state.vieworg_py;
	__m128 pz = mark_surfaces_state.vieworg_pz;

	uint32_t activelanes = 0;
	for (int plane_index = 0; plane_index < 4; ++plane_index)
	{
		soa_plane_t *plane = planes + plane_index;

		__m128 v0 = _mm_mul_ps (_mm_loadu_ps ((*plane) + 0), px);
		__m128 v1 = _mm_mul_ps (_mm_loadu_ps ((*plane) + 4), px);

		v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*plane) + 8), py));
		v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*plane) + 12), py));

		v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*plane) + 16), pz));
		v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*plane) + 20), pz));

		__m128 pd0 = _mm_loadu_ps ((*plane) + 24);
		__m128 pd1 = _mm_loadu_ps ((*plane) + 28);

		uint32_t plane_lanes = (uint32_t)(_mm_movemask_ps (_mm_cmplt_ps (pd0, v0)) | (_mm_movemask_ps (_mm_cmplt_ps (pd1, v1)) << 4));
		activelanes |= plane_lanes << (plane_index * 8);
	}
	return activelanes;
}

/*
===============
R_CullBoxSIMD

Performs frustum culling for 32 bounding boxes
===============
*/
static FORCE_INLINE uint32_t R_CullBoxSIMD (soa_aabb_t *boxes, uint32_t activelanes)
{
	for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
	{
		if (activelanes == 0)
			break;

		int    ofsx = mark_surfaces_state.frustum_ofsx[frustum_index];
		int    ofsy = mark_surfaces_state.frustum_ofsy[frustum_index];
		int    ofsz = mark_surfaces_state.frustum_ofsz[frustum_index];
		__m128 px = mark_surfaces_state.frustum_px[frustum_index];
		__m128 py = mark_surfaces_state.frustum_py[frustum_index];
		__m128 pz = mark_surfaces_state.frustum_pz[frustum_index];
		__m128 pd = mark_surfaces_state.frustum_pd[frustum_index];

		uint32_t frustum_lanes = 0;
		for (int boxes_index = 0; boxes_index < 4; ++boxes_index)
		{
			soa_aabb_t *box = boxes + boxes_index;
			__m128      v0 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx), px);
			__m128      v1 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx + 4), px);
			v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsy), py));
			v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsy + 4), py));
			v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsz), pz));
			v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsz + 4), pz));
			frustum_lanes |= (uint32_t)(_mm_movemask_ps (_mm_cmplt_ps (pd, v0)) | (_mm_movemask_ps (_mm_cmplt_ps (pd, v1)) << 4)) << (boxes_index * 8);
		}
		activelanes &= frustum_lanes;
	}

	return activelanes;
}
#endif // defined(USE_SSE2)

#if defined(USE_SIMD)
/*
===============
R_MarkVisSurfacesSIMD
===============
*/
void R_MarkVisSurfacesSIMD (qboolean *use_tasks)
{
	msurface_t  *surf;
	unsigned int i, k;
	unsigned int numleafs = cl.worldmodel->numleafs;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t    *vis = (uint32_t *)mark_surfaces_state.vis;
	uint32_t    *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t  *leafbounds = cl.worldmodel->soa_leafbounds;

	// iterate through leaves, marking surfaces
	for (i = 0; i < numleafs; i += 32)
	{
		uint32_t mask = vis[i / 32];
		if (mask == 0)
			continue;

		mask = R_CullBoxSIMD (&leafbounds[i / 8], mask);
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);

			mleaf_t *leaf = &cl.worldmodel->leafs[1 + i + j];
			if (leaf->contents != CONTENTS_SKY || r_oldskyleaf.value)
			{
				unsigned int nummarksurfaces = leaf->nummarksurfaces;
				int         *marksurfaces = leaf->firstmarksurface;
				for (k = 0; k < nummarksurfaces; ++k)
				{
					unsigned int index = marksurfaces[k];
					surfvis[index / 32] |= 1u << (index % 32);
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

	uint32_t brushpolys = 0;
	for (i = 0; i < numsurfaces; i += 32)
	{
		uint32_t mask = surfvis[i / 32];
		if (mask == 0)
			continue;

		mask &= R_BackFaceCullSIMD (&cl.worldmodel->soa_surfplanes[i / 8]);
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);

			surf = &cl.worldmodel->surfaces[i + j];
			++brushpolys;
			R_ChainSurface (surf, chain_world);
			if (!r_gpulightmapupdate.value)
				R_RenderDynamicLightmaps (surf);
			else if (surf->lightmaptexturenum >= 0)
				Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
			if (surf->texinfo->texture->warpimage)
				Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);
		}
	}

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}

/*
===============
R_MarkLeafsSIMD
===============
*/
void R_MarkLeafsSIMD (int index, void *unused)
{
	unsigned int     j;
	unsigned int     first_leaf = index * 32;
	atomic_uint32_t *surfvis = (atomic_uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t      *leafbounds = cl.worldmodel->soa_leafbounds;
	uint32_t        *vis = (uint32_t *)mark_surfaces_state.vis;

	uint32_t *mask = &vis[index];
	if (*mask == 0)
		return;

	*mask = R_CullBoxSIMD (&leafbounds[index * 4], *mask);

	uint32_t mask_iter = *mask;
	while (mask_iter != 0)
	{
		const int i = FindFirstBitNonZero (mask_iter);

		mleaf_t *leaf = &cl.worldmodel->leafs[1 + first_leaf + i];
		if (leaf->contents != CONTENTS_SKY || r_oldskyleaf.value)
		{
			unsigned int nummarksurfaces = leaf->nummarksurfaces;
			int         *marksurfaces = leaf->firstmarksurface;
			for (j = 0; j < nummarksurfaces; ++j)
			{
				unsigned int surf_index = marksurfaces[j];
				Atomic_OrUInt32 (&surfvis[surf_index / 32], 1u << (surf_index % 32));
			}
		}
		const uint32_t bit_mask = ~(1u << i);
		if (!leaf->efrags)
		{
			*mask &= bit_mask;
		}
		mask_iter &= bit_mask;
	}
}

/*
===============
R_BackfaceCullSurfacesSIMD
===============
*/
void R_BackfaceCullSurfacesSIMD (int index, void *unused)
{
	uint32_t   *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	msurface_t *surf;

	uint32_t *mask = &surfvis[index];
	if (*mask == 0)
		return;

	*mask &= R_BackFaceCullSIMD (&cl.worldmodel->soa_surfplanes[index * 4]);

	uint32_t mask_iter = *mask;
	while (mask_iter != 0)
	{
		const int i = FindFirstBitNonZero (mask_iter);

		surf = &cl.worldmodel->surfaces[(index * 32) + i];
		if (surf->lightmaptexturenum >= 0)
			Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
		if (surf->texinfo->texture->warpimage)
			Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);

		const uint32_t bit_mask = ~(1u << i);
		mask_iter &= bit_mask;
	}
}

/*
===============
R_StoreLeafEFrags
===============
*/
void R_StoreLeafEFrags (void *unused)
{
	unsigned int i;
	unsigned int numleafs = cl.worldmodel->numleafs;
	uint32_t    *vis = (uint32_t *)mark_surfaces_state.vis;
	for (i = 0; i < numleafs; i += 32)
	{
		uint32_t mask = vis[i / 32];
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);
			mleaf_t *leaf = &cl.worldmodel->leafs[1 + i + j];
			R_StoreEfrags (&leaf->efrags);
		}
	}
}

/*
===============
R_ChainVisSurfaces
===============
*/
void R_ChainVisSurfaces (qboolean *use_tasks)
{
	unsigned int i;
	msurface_t  *surf;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t    *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	uint32_t     brushpolys = 0;
	for (i = 0; i < numsurfaces; i += 32)
	{
		uint32_t mask = surfvis[i / 32];
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);
			surf = &cl.worldmodel->surfaces[i + j];
			++brushpolys;
			R_ChainSurface (surf, chain_world);
		}
	}

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}
#endif // defined(USE_SIMD)

/*
===============
R_MarkVisSurfaces
===============
*/
void R_MarkVisSurfaces (qboolean *use_tasks)
{
	int         i, j;
	msurface_t *surf;
	mleaf_t    *leaf;
	uint32_t    brushpolys = 0;
	uint32_t   *vis = (uint32_t *)mark_surfaces_state.vis;

	leaf = &cl.worldmodel->leafs[1];
	for (i = 0; i < cl.worldmodel->numleafs; i++, leaf++)
	{
        if (CVAR_TO_BOOL (rt_enable_pvs))
        {
		    if (!(vis[i / 32] & 1u << i % 32))
                continue;

            if (R_CullBox (leaf->minmaxs, leaf->minmaxs + 3))
                continue;
        }

        if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
        {
            for (j = 0; j < leaf->nummarksurfaces; j++)
            {
                surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[j]];

                if (surf->visframe == r_visframecount)
                    continue;

                surf->visframe = r_visframecount;

                if (CVAR_TO_BOOL (rt_enable_pvs))
                {		    
                    if (R_BackFaceCull (surf))
                        continue;
                }

                ++brushpolys;
                R_ChainSurface (surf, chain_world);
                if (!r_gpulightmapupdate.value)
                    R_RenderDynamicLightmaps (surf);
                else if (surf->lightmaptexturenum >= 0)
                    Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
                if (surf->texinfo->texture->warpimage)
                    Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);
            }
        }

        // add static models
        if (leaf->efrags)
            R_StoreEfrags (&leaf->efrags);
    }

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}

/*
===============
R_MarkSurfacesPrepare
===============
*/
static void R_MarkSurfacesPrepare (void *unused)
{
	int      i;
	qboolean nearwaterportal;
	int      numleafs = cl.worldmodel->numleafs;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i = 0; i < r_viewleaf->nummarksurfaces; i++)
		if (cl.worldmodel->surfaces[r_viewleaf->firstmarksurface[i]].flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (!CVAR_TO_BOOL (rt_enable_pvs) || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		mark_surfaces_state.vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		mark_surfaces_state.vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		mark_surfaces_state.vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	uint32_t *vis = (uint32_t *)mark_surfaces_state.vis;
	if ((numleafs % 32) != 0)
		vis[numleafs / 32] &= (1u << (numleafs % 32)) - 1;

	r_visframecount++;

	// set all chains to null
	for (i = 0; i < cl.worldmodel->numtextures; i++)
		if (cl.worldmodel->textures[i])
		{
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;
			cl.worldmodel->textures[i]->chain_size[chain_world] = 0;
		}

#if defined(USE_SIMD)
	if (use_simd)
	{
		memset (cl.worldmodel->surfvis, 0, (cl.worldmodel->numsurfaces + 31) / 8);
		for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
		{
			mplane_t *p = frustum + frustum_index;
			byte      signbits = p->signbits;
			__m128    vplane = _mm_loadu_ps (p->normal);
			mark_surfaces_state.frustum_ofsx[frustum_index] = signbits & 1 ? 0 : 8;   // x min/max
			mark_surfaces_state.frustum_ofsy[frustum_index] = signbits & 2 ? 16 : 24; // y min/max
			mark_surfaces_state.frustum_ofsz[frustum_index] = signbits & 4 ? 32 : 40; // z min/max
			mark_surfaces_state.frustum_px[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (0, 0, 0, 0));
			mark_surfaces_state.frustum_py[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (1, 1, 1, 1));
			mark_surfaces_state.frustum_pz[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (2, 2, 2, 2));
			mark_surfaces_state.frustum_pd[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (3, 3, 3, 3));
		}
		__m128 pos = _mm_loadu_ps (r_refdef.vieworg);
		mark_surfaces_state.vieworg_px = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (0, 0, 0, 0));
		mark_surfaces_state.vieworg_py = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (1, 1, 1, 1));
		mark_surfaces_state.vieworg_pz = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (2, 2, 2, 2));
	}
#endif
}

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (qboolean use_tasks, task_handle_t before_mark, task_handle_t *store_efrags, task_handle_t *cull_surfaces, task_handle_t *chain_surfaces)
{
	if (use_tasks)
	{
		task_handle_t prepare_mark = Task_AllocateAndAssignFunc (R_MarkSurfacesPrepare, NULL, 0);
		Task_AddDependency (before_mark, prepare_mark);
		Task_Submit (prepare_mark);
#if defined(USE_SIMD)
		if (use_simd)
		{
			if (r_parallelmark.value)
			{
				unsigned int  numleafs = cl.worldmodel->numleafs;
				task_handle_t mark_surfaces = Task_AllocateAndAssignIndexedFunc (R_MarkLeafsSIMD, (numleafs + 31) / 32, NULL, 0);
				Task_AddDependency (prepare_mark, mark_surfaces);
				Task_Submit (mark_surfaces);

				*store_efrags = Task_AllocateAndAssignFunc (R_StoreLeafEFrags, NULL, 0);
				Task_AddDependency (mark_surfaces, *store_efrags);

				unsigned int numsurfaces = cl.worldmodel->numsurfaces;
				*cull_surfaces = Task_AllocateAndAssignIndexedFunc (R_BackfaceCullSurfacesSIMD, (numsurfaces + 31) / 32, NULL, 0);
				Task_AddDependency (mark_surfaces, *cull_surfaces);

				*chain_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_ChainVisSurfaces, &use_tasks, sizeof (qboolean));
				Task_AddDependency (*cull_surfaces, *chain_surfaces);
			}
			else
			{
				task_handle_t mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfacesSIMD, &use_tasks, sizeof (qboolean));
				Task_AddDependency (prepare_mark, mark_surfaces);
				*store_efrags = mark_surfaces;
				*chain_surfaces = mark_surfaces;
				*cull_surfaces = mark_surfaces;
			}
		}
		else
#endif
		{
			task_handle_t mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfaces, &use_tasks, sizeof (qboolean));
			Task_AddDependency (prepare_mark, mark_surfaces);
			*store_efrags = mark_surfaces;
			*chain_surfaces = mark_surfaces;
			*cull_surfaces = mark_surfaces;
		}
	}
	else
	{
		R_MarkSurfacesPrepare (NULL);
		// iterate through leaves, marking surfaces
#if defined(USE_SIMD)
		if (use_simd)
		{
			R_MarkVisSurfacesSIMD (&use_tasks);
		}
		else
#endif
			R_MarkVisSurfaces (&use_tasks);
	}
}

//==============================================================================
//
// VBO SUPPORT
//
//==============================================================================

static int R_NumTriangleIndicesForSurf (int vertcount)
{
	return q_max (0, 3 * (vertcount - 2));
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (int basevert, int vertcount, uint32_t *dest)
{
	int i;
	for (i = 2; i < vertcount; i++)
	{
		*dest++ = basevert + i;
		*dest++ = basevert + i - 1;
		*dest++ = basevert;
	}
}

static void RT_ClearBatch (cb_context_t *cbx)
{
	cbx->batch_verts_count = 0;
	cbx->batch_indices_count = 0;
}

RgTransform RT_GetBrushModelMatrix (entity_t *e)
{
	if (e == NULL)
	{
		const static RgTransform identity = RT_TRANSFORM_IDENTITY;
		return identity;
	}

	vec3_t e_angles;
	VectorCopy (e->angles, e_angles);
	e_angles[0] = -e_angles[0]; // stupid quake bug

	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, e->origin, e_angles);

	return RT_GetModelTransform (model_matrix);
}

static RgSphericalLightUploadInfo MakeSphericalLightFromPoly (const RgPolygonalLightUploadInfo *src, const msurface_t *surf)
{
	RgSphericalLightUploadInfo info = {
		.uniqueID = src->uniqueID,
		.color = src->color,
		.position = {0},
		.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_elight_radius)),
	};

	float *dst = info.position.data;

	const float *a = src->positions[0].data;
	const float *b = src->positions[1].data;
	const float *c = src->positions[2].data;

	VectorAdd (dst, a, dst);
	VectorAdd (dst, b, dst);
	VectorAdd (dst, c, dst);
	VectorScale (dst, 1.0f / 3.0f, dst);

	vec3_t e1, e2;
	VectorSubtract (b, a, e1);
	VectorSubtract (c, a, e2);
	VectorNormalize (e1);
	VectorNormalize (e2);

	vec3_t normal;
	CrossProduct (e1, e2, normal);
	
	VectorMA (dst, METRIC_TO_QUAKEUNIT (0.05f), normal, dst);

	return info;
}

static qboolean RT_FindNearestTeleport (const RgGeometryUploadInfo *info, uint8_t *result, qboolean *potentially_mirror);
static RgFloat3D ApplyTransform (const RgTransform *transform, const vec3_t v);

typedef struct rt_uploadsurf_state_t
{
	int          entuniqueid;
	entity_t    *ent;
	qmodel_t    *model;
	msurface_t  *surf;
	gltexture_t *diffuse_tex;
	gltexture_t *lightmap_tex;
	qboolean     alpha_test;
	float        alpha;
	qboolean     use_zbias;
	qboolean     is_warp;
	qboolean     is_water;
	qboolean     is_teleport;
} rt_uploadsurf_state_t;

static void RT_FlushBatch (cb_context_t *cbx, const rt_uploadsurf_state_t *s, uint32_t *brushpasses)
{
	if (cbx->batch_verts_count == 0 || cbx->batch_indices_count == 0)
	{
		return;
	}

    const RgVertex *vertices = cbx->batch_verts;
	const uint32_t *indices = cbx->batch_indices;
	const int       num_surf_verts = cbx->batch_verts_count;
	const int       num_surf_indices = cbx->batch_indices_count;

	// i.e. uploaded once at the level load
    const qboolean is_static_geom = (s->model == cl.worldmodel) && !s->is_warp;

#if 0
	float constant_factor = 0.0f, slope_factor = 0.0f;
	if (use_zbias)
	{
		if (vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT)
		{
			constant_factor = -4.f;
			slope_factor = -0.125f;
		}
		else
		{
			constant_factor = -1.f;
			slope_factor = -0.25f;
		}
	}
	vkCmdSetDepthBias (cbx->cb, constant_factor, 0.0f, slope_factor);
#endif

	gltexture_t *diffuse_tex = r_lightmap_cheatsafe ? NULL : s->diffuse_tex;
	gltexture_t *lightmap_tex = r_fullbright_cheatsafe ? NULL : s->lightmap_tex;

	if (diffuse_tex && diffuse_tex->rtcustomtextype == RT_CUSTOMTEXTUREINFO_TYPE_POLY_LIGHT)
	{
		RgTransform transf = RT_GetBrushModelMatrix (s->ent);

		for (int tri = 0; tri < num_surf_indices / 3; tri++)
		{
			const vec_t *a0 = vertices[indices[tri * 3 + 0]].position;
			const vec_t *a1 = vertices[indices[tri * 3 + 1]].position;
			const vec_t *a2 = vertices[indices[tri * 3 + 2]].position;
			
			vec3_t color = RT_VEC3 (diffuse_tex->rtlightcolor);
			VectorScale (color, CVAR_TO_FLOAT (rt_dlight_intensity), color);
			RT_FIXUP_LIGHT_INTENSITY (color, true);
			
			RgPolygonalLightUploadInfo light_info = {
				.uniqueID = RT_GetBrushSurfUniqueId (s->entuniqueid, s->model, s->surf, tri),
				.color = RT_VEC3(color),
				.positions =
					{
						ApplyTransform (&transf, a0),
						ApplyTransform (&transf, a1),
						ApplyTransform (&transf, a2),
					},
			};

			if (!is_static_geom)
			{
				RgResult r = rgUploadPolygonalLight (vulkan_globals.instance, &light_info);
				RG_CHECK (r);
			}
			else
			{
				// if it's a static geometry, then save light data
				// to upload it each frame
				if (rt_wldlights_count < MAX_WORLDLIGHTS_COUNT)
				{
					rt_wldlights_tri[rt_wldlights_count] = light_info;
					rt_wldlights_sph[rt_wldlights_count] = MakeSphericalLightFromPoly (&light_info, s->surf);

					rt_wldlights_count++;
				}
				else
				{
					assert (false);
				}
			}
		}
	}

	if (s->is_teleport && CVAR_TO_INT32 (rt_reflrefr_depth) > 0)
	{
		diffuse_tex = NULL;
	}

	float alpha = CLAMP (0.0f, s->alpha, 1.0f);
	uint8_t portalindex = 0;

	qboolean is_mirror = diffuse_tex && diffuse_tex->rtcustomtextype == RT_CUSTOMTEXTUREINFO_TYPE_MIRROR;
	qboolean rasterize = (alpha < 1.0f) && !s->is_warp;

	if (rasterize)
	{
		// worldmodel must be uploaded only once
		assert (!is_static_geom);

		RgRasterizedGeometryUploadInfo info = {
			.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
			.vertexCount = num_surf_verts,
			.pVertices = vertices,
			.indexCount = num_surf_indices,
			.pIndices = indices,
			.transform = RT_GetBrushModelMatrix (s->ent),
			.color = RT_COLOR_WHITE,
			.material = diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
			.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		if (s->alpha_test)
		{
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
		}

		if (alpha < 1.0f)
		{
			info.color.data[3] = alpha;
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
			info.blendFuncSrc = RG_BLEND_FACTOR_SRC_ALPHA;
			info.blendFuncDst = RG_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}
		else
		{
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE;
		}

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = RT_GetBrushSurfUniqueId (s->entuniqueid, s->model, s->surf, 0),
			.flags = 
			    (is_mirror ? RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT : 0) |
			    (s->is_teleport ? RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT : 0) |
                RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = is_static_geom ? RG_GEOMETRY_TYPE_STATIC : RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType = 
			    is_mirror ? RG_GEOMETRY_PASS_THROUGH_TYPE_MIRROR :
			    s->is_water ? RG_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT :
			    s->is_teleport ? RG_GEOMETRY_PASS_THROUGH_TYPE_PORTAL :
		        RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType = RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = num_surf_verts,
			.pVertices = vertices,
			.indexCount = num_surf_indices,
			.pIndices = indices,
			.layerColors =
				{
					RT_COLOR_WHITE,
					RT_COLOR_WHITE,
				},
			.layerBlendingTypes =
				{
					RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE,
					lightmap_tex ? RG_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE : 0,
				},
			.geomMaterial =
				{
					diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
					lightmap_tex ? lightmap_tex->rtmaterial : RG_NO_MATERIAL,
				},
			.defaultRoughness = CVAR_TO_FLOAT (rt_brush_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_brush_metal),
			.defaultEmission = 0,
			.transform = RT_GetBrushModelMatrix (s->ent),
		};

		if (s->is_teleport)
		{
			qboolean portal_is_mirror = false;

			if (RT_FindNearestTeleport (&info, &portalindex, &portal_is_mirror))
			{
				if (portal_is_mirror)
				{
					info.passThroughType = RG_GEOMETRY_PASS_THROUGH_TYPE_MIRROR;
				}
				else
				{
				    info.pPortalIndex = &portalindex;
				}
			}
		}

		RgResult r = rgUploadGeometry (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}

	RT_ClearBatch (cbx);
	++(*brushpasses);
}

static void RT_BatchSurface (cb_context_t *cbx, const rt_uploadsurf_state_t *s, uint32_t *brushpasses)
{
	int num_surf_verts = s->surf->numedges;
	int num_surf_indices = R_NumTriangleIndicesForSurf (num_surf_verts);

	if (cbx->batch_indices_count + num_surf_indices > MAX_BATCH_INDICES ||
		cbx->batch_verts_count + num_surf_verts > MAX_BATCH_VERTS)
	{
		RT_FlushBatch (cbx, s, brushpasses);
	}

	R_TriangleIndicesForSurf (cbx->batch_verts_count, num_surf_verts, &cbx->batch_indices[cbx->batch_indices_count]);
	memcpy (&cbx->batch_verts[cbx->batch_verts_count], rtallbrushvertices + s->surf->vbo_firstvert, sizeof (RgVertex) * num_surf_verts);

	cbx->batch_indices_count += num_surf_indices;
	cbx->batch_verts_count += num_surf_verts;
}

/*
================
GL_WaterAlphaForEntitySurface -- ericw

Returns the water alpha to use for the entity and surface combination.
================
*/
float GL_WaterAlphaForEntitySurface (entity_t *ent, msurface_t *s)
{
	float entalpha;
	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForSurface (s);
	else
		entalpha = ENTALPHA_DECODE (ent->alpha);
	return entalpha;
}

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (cb_context_t *cbx, qmodel_t *model, texchain_t chain)
{
	int         i;
	msurface_t *s;
	texture_t  *t;
	float       color[] = {1.0f, 1.0f, 1.0f};
	const float alpha = 1.0f;

    const static RgTransform tr = RT_TRANSFORM_IDENTITY;

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
			DrawGLPoly (
				cbx, RT_UNIQUEID_DONTCARE,
				s->polys, color, alpha, 
				&tr, NULL, 
				CVAR_TO_BOOL (r_showtris) ? DRAW_GL_POLY_TYPE_SHOWTRI : DRAW_GL_POLY_TYPE_SHOWTRI_NODEPTH);
	}
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	int                   i;
	msurface_t           *s;
	texture_t            *t;
	rt_uploadsurf_state_t last_state = {0};

	uint32_t brushpasses = 0;
	for (i = 0; i < model->numtextures; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;
		
		RT_ClearBatch (cbx);

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			if (model != cl.worldmodel)
			{
				// ericw -- this is copied from R_DrawSequentialPoly.
				// If the poly is not part of the world we have to
				// set this flag
				Atomic_StoreUInt32 (&t->update_warp, true); // FIXME: one frame too late!
			}

			rt_uploadsurf_state_t cur_state = {
				.entuniqueid = entuniqueid,
				.ent = ent,
				.model = model,
				.surf = s,
				.diffuse_tex = t->gltexture, // t->warpimage,
				.lightmap_tex = (s->lightmaptexturenum >= 0) ? lightmaps[s->lightmaptexturenum].texture : greytexture,
				.alpha_test = false,
				.alpha = GL_WaterAlphaForEntitySurface (ent, s),
				.use_zbias = false,
				.is_warp = true,
				.is_water = (s->flags & SURF_DRAWWATER),
				.is_teleport = (s->flags & SURF_DRAWTELE),
			};

			if (cur_state.lightmap_tex != last_state.lightmap_tex ||
				fabsf(cur_state.alpha - last_state.alpha) < 0.001f)
			{
				RT_FlushBatch (cbx, &last_state, &brushpasses);
			}

			RT_BatchSurface (cbx, &cur_state, &brushpasses);
			last_state = cur_state;
		}

		RT_FlushBatch (cbx, &last_state, &brushpasses);
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
================
R_DrawTextureChains_Multitexture
================
*/
void R_DrawTextureChains_Multitexture (
	cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, const float alpha, int texstart, int texend, int entuniqueid)
{
	int                   i;
	msurface_t           *s;
	texture_t            *t;
	qboolean              use_zbias = (gl_zfix.value && model != cl.worldmodel);
	int                   ent_frame = ent != NULL ? ent->frame : 0;
	rt_uploadsurf_state_t last_state = {0};
	
	uint32_t brushpasses = 0;
	for (i = texstart; i < texend; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		RT_ClearBatch (cbx);

		qboolean alpha_test = (t->texturechains[chain]->flags & SURF_DRAWFENCE) != 0;
		gltexture_t *diffuse_tex = R_TextureAnimation (t, ent_frame)->gltexture;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			rt_uploadsurf_state_t cur_state = {
				.entuniqueid = entuniqueid,
				.ent = ent,
				.model = model,
				.surf = s,
				.diffuse_tex = diffuse_tex,
				.lightmap_tex = (s->lightmaptexturenum >= 0) ? lightmaps[s->lightmaptexturenum].texture : greytexture,
				.alpha_test = alpha_test,
				.alpha = alpha,
				.use_zbias = use_zbias,
				.is_warp = false,
				.is_water = false,
				.is_teleport = false,
			};

			if (cur_state.lightmap_tex != last_state.lightmap_tex)
			{
				RT_FlushBatch (cbx, &last_state, &brushpasses);
			}

			RT_BatchSurface (cbx, &cur_state, &brushpasses);
			last_state = cur_state;
		}

		RT_FlushBatch (cbx, &last_state, &brushpasses);
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	float entalpha;

	if (ent != NULL)
		entalpha = ENTALPHA_DECODE (ent->alpha);
	else
		entalpha = 1;

	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	R_DrawTextureChains_Multitexture (cbx, model, ent, chain, entalpha, 0, model->numtextures, entuniqueid);
}

/*
=============
R_DrawWorld -- ericw -- moved from R_DrawTextureChains, which is no longer specific to the world.
=============
*/
void R_DrawWorld (cb_context_t *cbx, int index)
{
	rt_wldlights_count = 0;

	if (!r_drawworld_cheatsafe)
		return;

	R_BeginDebugUtilsLabel (cbx, "World");
	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	R_DrawTextureChains_Multitexture (cbx, cl.worldmodel, NULL, chain_world, 1, world_texstart[index], world_texend[index], ENT_UNIQUEID_WORLD);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_BeginDebugUtilsLabel (cbx, "Water");
	R_DrawTextureChains_Water (cbx, cl.worldmodel, NULL, chain_world, ENT_UNIQUEID_WORLD);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_ShowTris (cbx, cl.worldmodel, chain_world);
}



void RT_CustomLights_Parse (void)
{
	rt_customlights_all_count = 0;
	rt_customlights_curr_count = 0;

	FILE *f = fopen (RT_CUSTOMLIGHTS_PATH, "r");
	if (f == NULL)
	{
		Con_Printf ("Couldn't open %s\n", RT_CUSTOMLIGHTS_PATH);
		return;
	}

	int alloccount = 1;
	{
		int ch = 0;
		do
		{
			ch = fgetc (f);
			if (ch == '\n')
			{
				alloccount++;
			}
		} while (ch != EOF);
	}
	rt_customlights_all = Mem_Realloc (rt_customlights_all, sizeof (rt_customlights_all[0]) * alloccount);
	rt_customlights_curr = Mem_Realloc (rt_customlights_curr, sizeof (rt_customlights_curr[0]) * alloccount);
	rewind (f);

	qboolean foundend = false;
	char     curline[256] = "";

	while (!foundend)
	{
		{
			int i = 0;

			while (true)
			{
				int ch = fgetc (f);

				if (ch == '\n' || ch == '\r' || ch == '\0' || ch == EOF)
				{
					foundend = (ch == '\0' || ch == EOF);
					break;
				}

				if (i >= (int)sizeof (curline))
				{
					Sys_Error (RT_CUSTOMLIGHTS_PATH ": line must be < 256 characters");
				}

				curline[i] = (char)ch;
				i++;
			}

			curline[i] = '\0';
		}

		if (curline[0] == '\0')
		{
			continue;
		}

		char      mapname[countof (rt_customlights_all[0].mapname)];
		RgFloat3D position;
		char      str_hexcolor[8];

		int c = sscanf (curline, "%s %f %f %f %6s", mapname, &position.data[0], &position.data[1], &position.data[2], str_hexcolor);
		if (c >= 5)
		{
			const RgFloat3D color01 = RT_HexStringToColor (str_hexcolor);

			mapname[countof (mapname) - 1] = '\0';
			
			rt_worldcustomlight_t *dst = &rt_customlights_all[rt_customlights_all_count++];
			{
				strncpy (dst->mapname, mapname, sizeof (dst->mapname));
				dst->color01 = color01;
				dst->position = position;
				dst->deleted = false;
			}
		}
	}

	fclose (f);


	// make list for current map
	const char *cur_mapname = cl.worldmodel->name;

	for (int i = 0; i < rt_customlights_all_count; i++)
	{
		const rt_worldcustomlight_t *src = &rt_customlights_all[i];

		if (strncmp (src->mapname, cur_mapname, countof (src->mapname)) == 0)
		{
			rt_customlights_curr[rt_customlights_curr_count++] = i;
		}
	}
}

void RT_CustomLights_SaveCmd (void)
{
#ifdef _WIN32
	// backup file
	CopyFile (RT_CUSTOMLIGHTS_PATH, RT_CUSTOMLIGHTS_PATH_BACKUP, FALSE);
#endif

	FILE *f = fopen (RT_CUSTOMLIGHTS_PATH, "w+");
	if (f == NULL)
	{
		Con_Printf ("Couldn't open %s\n", RT_CUSTOMLIGHTS_PATH);
		return;
	}

    for (int i = 0; i < rt_customlights_all_count; i++)
	{
		const rt_worldcustomlight_t *lt = &rt_customlights_all[i];

		if (lt->deleted)
		{
			continue;
		}

		const float *rawcolor = lt->color01.data;
		const float *position = lt->position.data;

		char hexstr[7];
		assert (rawcolor[0] >= 0.0f && rawcolor[0] < 1.01f);
		assert (rawcolor[1] >= 0.0f && rawcolor[1] < 1.01f);
		assert (rawcolor[2] >= 0.0f && rawcolor[2] < 1.01f);
		RT_ColorToHexString (rawcolor, hexstr);

	    fprintf (f, "%s %.2f %.2f %.2f %6s\n", lt->mapname, position[0], position[1], position[2], hexstr);
	}

	fclose (f);
}

void RT_CustomLights_AddCmd (void)
{
	if (Cmd_Argc () != 4)
	{
		Con_Printf ("adds a custom light around the camera\n");
		Con_Printf ("usage: <r 0..255> <g 0..255> <b 0..255>\n");
		return;
	}

	const char *cur_mapname = cl.worldmodel->name;
	if (cur_mapname == NULL)
	{
		Con_Printf ("Null world\n");
		return;
	}

	const RgFloat3D position = RT_VEC3 (r_refdef.vieworg);
	RgFloat3D color01 = {
		strtof (Cmd_Argv (1), NULL),
		strtof (Cmd_Argv (2), NULL),
		strtof (Cmd_Argv (3), NULL),
	};
	color01.data[0] = CLAMP (0.0f, color01.data[0] / 255.0f, 1.0f);
	color01.data[1] = CLAMP (0.0f, color01.data[1] / 255.0f, 1.0f);
	color01.data[2] = CLAMP (0.0f, color01.data[2] / 255.0f, 1.0f);

	// add to global list
	int gindex = rt_customlights_all_count;
	{
		rt_customlights_all_count++;
		rt_customlights_all = Mem_Realloc (rt_customlights_all, sizeof (rt_customlights_all[0]) * rt_customlights_all_count);

		rt_worldcustomlight_t *dst = &rt_customlights_all[gindex];
		{
			strncpy (dst->mapname, cur_mapname, sizeof (dst->mapname));
			dst->color01 = color01;
			dst->position = position;
			dst->deleted = false;
		}
	}

	// link current world light list
	int curwld_index = rt_customlights_curr_count;
	{
		rt_customlights_curr_count++;
		rt_customlights_curr = Mem_Realloc (rt_customlights_curr, sizeof (rt_customlights_curr[0]) * rt_customlights_curr_count);

		rt_customlights_curr[curwld_index] = gindex;
	}
}

void RT_CustomLights_RemoveCmd (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("removes custom lights around the camera\n");
		Con_Printf ("usage: <radius (meters)>\n");

		return;
	}

	const vec3_t around = RT_VEC3 (r_refdef.vieworg);
	float radius = strtof (Cmd_Argv (1), NULL);
	radius = METRIC_TO_QUAKEUNIT (radius);

	int count = 0;

	// scan current world lights
	for (int i = 0; i < rt_customlights_curr_count; i++)
	{
		rt_worldcustomlight_t *lt = &rt_customlights_all[rt_customlights_curr[i]];
		
		if (VectorLengthSquared (lt->position.data, around) < radius * radius)
		{
			lt->deleted = true;
			count++;
		}
	}

	Con_Printf ("removed %d lights\n", count);
}



void RT_UploadAllWorldModelLights (void)
{
	for (int i = 0; i < rt_wldlights_count; i++)
	{
#if 0
		RgResult r = rgUploadPolygonalLight (vulkan_globals.instance, &rt_wldlights_tri[i]);
#else
		RgResult r = rgUploadSphericalLight(vulkan_globals.instance, &rt_wldlights_sph[i]);
#endif
		RG_CHECK (r);
	}

	for (int i = 0; i < rt_customlights_curr_count; i++)
	{
		const rt_worldcustomlight_t *src = &rt_customlights_all[rt_customlights_curr[i]];

		if (src->deleted)
		{
			continue;
		}

		RgFloat3D color = src->color01;
		VectorScale (color.data, CVAR_TO_FLOAT (rt_wlight_intensity), color.data);
		RT_FIXUP_LIGHT_INTENSITY (color.data, true);

		RgSphericalLightUploadInfo lt = {
			.uniqueID = RT_GetCustomObjectUniqueId (i),
			.color = color,
		    .position = src->position,
			.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_wlight_radius) ),
		};

		RgResult r = rgUploadSphericalLight (vulkan_globals.instance, &lt);
		RG_CHECK (r);
	}
}



typedef struct rt_teleport_s
{
	vec3_t   a;
	vec3_t   b;
	float    b_angle;
	qboolean potentially_mirror;
} rt_teleport_t;

rt_teleport_t *rt_teleports = NULL;
int            rt_teleports_count = 0;


struct rt_triggerteleport_t
{
	char target[128];
	char model[128];
};
struct rt_infoteleportdestination_t
{
	char   targetname[128];
	float  angle;
	qboolean angle_exists;
	vec3_t origin;
};
struct rt_parsetriggers_result_t
{
	struct rt_triggerteleport_t         *trigs;
	int                                  trigs_count;
	struct rt_infoteleportdestination_t *dsts;
	int                                  dsts_count;
};

static struct rt_parsetriggers_result_t ParseTeleportTriggers (void)
{
	struct rt_parsetriggers_result_t result = {0};

	if (!cl.worldmodel)
	{
		return result;
	}

	const char *data = cl.worldmodel->entities;
	if (!data)
	{
		return result;
	}


	char key[128], value[4096];


    #define STRUCT_STATE_STRUCT_STARTED			1
    #define STRUCT_STATE_CLASSNAME_TRIGGER		2
    #define STRUCT_STATE_CLASSNAME_DESTINATION	4
    #define STRUCT_STATE_TARGET					8
    #define STRUCT_STATE_TARGETNAME				16
    #define STRUCT_STATE_MODEL					32
    #define STRUCT_STATE_ANGLE					64
    #define STRUCT_STATE_ORIGIN					128
	int structstate = 0;

	struct
	{
		struct rt_triggerteleport_t tr;
		struct rt_infoteleportdestination_t dst;
	} structvalues = {0};
	

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return result; // error

	    if (com_token[0] == '{')
	    {
			memset (&structvalues, 0, sizeof (structvalues));
			structstate = STRUCT_STATE_STRUCT_STARTED;
            continue;
	    }
	    else if (com_token[0] == '}')
		{
			if (structstate & STRUCT_STATE_STRUCT_STARTED)
			{
				if (structstate & STRUCT_STATE_CLASSNAME_TRIGGER)
				{
					result.trigs = Mem_Realloc (result.trigs, sizeof (*result.trigs) * (result.trigs_count + 1));
					result.trigs[result.trigs_count] = structvalues.tr;
					result.trigs_count++;
				}
				else if (structstate & STRUCT_STATE_CLASSNAME_DESTINATION)
				{
					result.dsts = Mem_Realloc (result.dsts, sizeof (*result.dsts) * (result.dsts_count + 1));
					result.dsts[result.dsts_count] = structvalues.dst;
					result.dsts_count++;
				}
			}

			structstate = 0; // end of struct
			continue;
		}

		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return result; // error
		q_strlcpy (value, com_token, sizeof (value));

		
		if (strcmp (key, "classname") == 0)
		{
			if (strcmp (value, "trigger_teleport") == 0)
			{
				structstate |= STRUCT_STATE_CLASSNAME_TRIGGER;
			}
			else if (strcmp (value, "info_teleport_destination") == 0)
			{
				structstate |= STRUCT_STATE_CLASSNAME_DESTINATION;
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				structvalues.dst.origin[0] = tmpvec[0];
				structvalues.dst.origin[1] = tmpvec[1];
				structvalues.dst.origin[2] = tmpvec[2];
				structstate |= STRUCT_STATE_ORIGIN;
			}
		}
		else if (strcmp (key, "angle") == 0)
		{
			float tmp;
			int   components = sscanf (value, "%f", &tmp);

			if (components ==1)
			{
				structvalues.dst.angle = tmp;
				structvalues.dst.angle_exists = true;
				structstate |= STRUCT_STATE_ANGLE;
			}
		}
		else if (strcmp (key, "model") == 0)
		{
			q_strlcpy (structvalues.tr.model, value, sizeof (structvalues.tr.model));
			structstate |= STRUCT_STATE_MODEL;
		}
		else if (strcmp (key, "target") == 0)
		{
			q_strlcpy (structvalues.tr.target, value, sizeof (structvalues.tr.target));
			structstate |= STRUCT_STATE_TARGET;
		}
		else if (strcmp (key, "targetname") == 0)
		{
			q_strlcpy (structvalues.dst.targetname, value, sizeof (structvalues.dst.targetname));
			structstate |= STRUCT_STATE_TARGETNAME;
		}
	}
}

#define RG_MAX_PORTALS 62

void RT_ParseTeleports (void)
{
	rt_teleports_count = 0;

	struct rt_parsetriggers_result_t r = ParseTeleportTriggers ();
	if (r.trigs_count == 0 || r.dsts_count == 0)
	{
		return;
	}

	for (int i = 0; i < r.trigs_count; i++)
	{
		for (int o = 0; o < r.dsts_count; o++)
		{
			const struct rt_triggerteleport_t         *in = &r.trigs[i];
			const struct rt_infoteleportdestination_t *out = &r.dsts[o];

			// if found a match between trigger and destination
			if (strncmp (in->target, out->targetname, sizeof (in->target)) == 0)
			{
				qmodel_t *mod = Mod_ForName (in->model, false);

				if (mod)
				{
					rt_teleport_t *entry;
					{
						rt_teleports = Mem_Realloc (rt_teleports, sizeof (*rt_teleports) * (rt_teleports_count + 1));
						entry = &rt_teleports[rt_teleports_count];
						memset (entry, 0, sizeof (*entry));
						rt_teleports_count++;
					}


					// trigger position
					VectorAdd (mod->mins, mod->maxs, entry->a);
					VectorScale (entry->a, 0.5f, entry->a);


					// destination position
					VectorCopy (out->origin, entry->b);
					entry->b_angle = out->angle;


					// if angle wasn't specified and entrance/exit are close
					// example: 2 such portals exist in Haunted Halls
					if (!out->angle_exists)
					{
						vec3_t delta;
						VectorSubtract (entry->a, entry->b, delta);

						if (VectorLength (delta) < METRIC_TO_QUAKEUNIT(8.0f))
						{
							entry->potentially_mirror = true;
						}
					}
				}

				break;
			}
		}
	}

	// RTGL1's limit
	if (rt_teleports_count > RG_MAX_PORTALS)
	{
		rt_teleports_count = RG_MAX_PORTALS;
		Con_Warning ("Too many teleports to render, limit is 62");
	}

	Mem_Free (r.trigs);
	Mem_Free (r.dsts);
}


static RgFloat3D ApplyTransform (const RgTransform *transform, const vec3_t v)
{
	RgFloat3D r = {0};
	for (int i = 0; i < 3; i++)
	{
		r.data[i] = 
			transform->matrix[i][0] * v[0] + 
			transform->matrix[i][1] * v[1] + 
			transform->matrix[i][2] * v[2] + 
			transform->matrix[i][3];
	}
	return r;
}


static float DistanceSqr (const vec3_t a, const vec3_t b)
{
	vec3_t delta;
	VectorSubtract (a, b, delta);

	return DotProduct (delta, delta);
}


static qboolean RT_FindNearestTeleport (const RgGeometryUploadInfo *info, uint8_t *result, qboolean *potentially_mirror)
{
	vec3_t emin = {FLT_MAX, FLT_MAX, FLT_MAX};
	vec3_t emax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	for (uint32_t i = 0; i < info->vertexCount; i++)
	{
		RgFloat3D v = ApplyTransform (&info->transform, info->pVertices[i].position);

		for (int k = 0; k < 3; k++)
		{
			emin[k] = q_min (v.data[k], emin[k]);
			emax[k] = q_max (v.data[k], emax[k]);
		}
	}

	vec3_t center;
	VectorAdd (emin, emax, center);
	VectorScale (center, 0.5f, center);

	int nearest = -1;
	float nearest_dist = FLT_MAX;

	for (int i = 0; i < rt_teleports_count; i++)
	{
		float d = DistanceSqr (rt_teleports[i].a, center);

		if (d < nearest_dist)
		{
			nearest = i;
			nearest_dist = d;
		}
	}

	if (nearest < 0)
	{
		return false;
	}

	assert (nearest <= RG_MAX_PORTALS);

	*result = (uint8_t)nearest;
	*potentially_mirror = rt_teleports[nearest].potentially_mirror;
	return true;
}


void RT_UploadAllTeleports ()
{
	assert (rt_teleports_count >= 0 && rt_teleports_count <= RG_MAX_PORTALS);


	const vec3_t outoffset = {0, 0, 64};

	for (int i = 0; i < rt_teleports_count; i++)
	{
		const rt_teleport_t *tele = &rt_teleports[i];

		vec3_t forward, right, up;
		{
			vec3_t out_angles = {0, tele->b_angle, 0};
			AngleVectors (out_angles, forward, right, up);
		}

		RgPortalUploadInfo info = 
		{
			.portalIndex = (uint8_t)i,
			.inPosition = RT_VEC3 (tele->a),
			.outPosition = RT_VEC3 (tele->b),
			.outDirection = RT_VEC3 (forward),
			.outUp = RT_VEC3 (up),
		};

		VectorAdd (info.outPosition.data, outoffset, info.outPosition.data);

		RgResult r = rgUploadPortal (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}
}