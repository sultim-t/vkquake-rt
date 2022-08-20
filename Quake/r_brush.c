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
// r_brush.c: brush model rendering. renamed from r_surf.c

#include "quakedef.h"
#include "gl_heap.h"

extern cvar_t gl_fullbrights, r_drawflat, r_gpulightmapupdate; // johnfitz
extern cvar_t rt_brush_metal, rt_brush_rough, rt_enable_pvs;

int gl_lightmap_format;

#define SHELF_HEIGHT 256
#define SHELVES      (LMBLOCK_HEIGHT / SHELF_HEIGHT)

#define MAX_EXTENT 18

struct lightmap_s *lightmaps;
int                lightmap_count;
int                last_lightmap_allocated;
int                used_columns[MAX_SANITY_LIGHTMAPS][SHELVES];
int                lightmap_idx[MAX_EXTENT];
int                shelf_idx[MAX_EXTENT];
int                columns[MAX_EXTENT];
int                rows[MAX_EXTENT];

unsigned blocklights[LMBLOCK_WIDTH * LMBLOCK_HEIGHT * 3 + 1]; // johnfitz -- was 18*18, added lit support (*3) and loosened surface extents maximum
                                                              // (LMBLOCK_WIDTH*LMBLOCK_HEIGHT)

extern cvar_t r_showtris;
extern cvar_t r_simd;

extern cvar_t rt_classic_render;

RgVertex *rtallbrushvertices;


/*
===============
R_TextureAnimation -- johnfitz -- added "frame" param to eliminate use of "currententity" global

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	int relative;
	int count;

	if (frame)
		if (base->alternate_anims)
			base = base->alternate_anims;

	if (!base->anim_total)
		return base;

	relative = (int)(cl.time * 10) % base->anim_total;

	count = 0;
	while (base->anim_min > relative || base->anim_max <= relative)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}

/*
================
DrawGLPoly
================
*/
void DrawGLPoly (
	cb_context_t *cbx, uint64_t uniqueid,
	glpoly_t *p, float color[3], float alpha,
	const RgTransform *transform, const gltexture_t *tex, uint32_t type)
{
	const int numverts = p->numverts;

	RgVertex *vertices = RT_AllocScratchMemoryNulled (numverts * sizeof (RgVertex));

    float* v = p->verts[0];
	for (int i = 0; i < numverts; ++i, v += VERTEXSIZE)
	{
		vertices[i].position[0] = v[0];
		vertices[i].position[1] = v[1];
		vertices[i].position[2] = v[2];
		vertices[i].texCoord[0] = v[3];
		vertices[i].texCoord[1] = v[4];
		vertices[i].packedColor = RT_PACKED_COLOR_WHITE;
	}

	const qboolean is_sky = (type == DRAW_GL_POLY_TYPE_SKY);
	const qboolean showtri_nodepth = (type == DRAW_GL_POLY_TYPE_SHOWTRI_NODEPTH);
	const qboolean showtri = (type == DRAW_GL_POLY_TYPE_SHOWTRI) || showtri_nodepth;

	// mutually exclusive
	if (!is_sky && !showtri && !showtri_nodepth)
	{
		assert (0);
		return;
	}

	qboolean rasterize = !is_sky && showtri;

	if (rasterize)
	{
		RgRasterizedGeometryUploadInfo info = {
			.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
			.vertexCount = numverts,
			.pVertices = vertices,
			.indexCount = RT_GetFanIndexCount (numverts),
			.pIndices = RT_GetFanIndices (numverts),
			.transform = *transform,
			.color = {color[0], color[1], color[2], alpha},
			.material = tex ? tex->rtmaterial : RG_NO_MATERIAL,
			.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		if (showtri)
		{
			info.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE;
		}
		else if (showtri_nodepth)
		{
			info.pipelineState = 0;
		}
		if (alpha < 1.0f)
		{
			info.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | RG_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;

			info.blendFuncSrc = RG_BLEND_FACTOR_SRC_ALPHA;
			info.blendFuncDst = RG_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = uniqueid,
			.flags = RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType = RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType = is_sky ? RG_GEOMETRY_VISIBILITY_TYPE_SKY : RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = numverts,
			.pVertices = vertices,
			.indexCount = RT_GetFanIndexCount (numverts),
			.pIndices = RT_GetFanIndices (numverts),
			.layerColors = {{color[0], color[1], color[2], alpha}},
			.layerBlendingTypes = {RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
			.geomMaterial = {tex ? tex->rtmaterial : RG_NO_MATERIAL},
			.defaultRoughness = CVAR_TO_FLOAT (rt_brush_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_brush_metal),
			.defaultEmission = 0,
			.transform = *transform,
		};

		RgResult r = rgUploadGeometry (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}
}

/*
=============================================================

    BRUSH MODELS

=============================================================
*/

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (cb_context_t *cbx, entity_t *e, int chain, int entuniqueid)
{
	int         i, k;
	msurface_t *psurf;
	float       dot;
	mplane_t   *pplane;
	qmodel_t   *clmodel;
	vec3_t      modelorg;

	if (CVAR_TO_BOOL (rt_enable_pvs))
	{
		if (R_CullModelForEntity (e))
			return;
	}

	clmodel = e->model;

	if (CVAR_TO_BOOL (rt_enable_pvs))
	{
		VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
		if (e->angles[0] || e->angles[1] || e->angles[2])
		{
			vec3_t temp;
			vec3_t forward, right, up;

			VectorCopy (modelorg, temp);
			AngleVectors (e->angles, forward, right, up);
			modelorg[0] = DotProduct (temp, forward);
			modelorg[1] = -DotProduct (temp, right);
			modelorg[2] = DotProduct (temp, up);
		}
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	// calculate dynamic lighting for bmodel if it's not an
	// instanced model
	if (clmodel->firstmodelsurface != 0)
	{
		for (k = 0; k < MAX_DLIGHTS; k++)
		{
			if ((cl_dlights[k].die < cl.time) || (!cl_dlights[k].radius))
				continue;

			R_MarkLights (&cl_dlights[k], k, clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	R_ClearTextureChains (clmodel, chain);
	for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
	{
		if (CVAR_TO_BOOL (rt_enable_pvs))
		{
			pplane = psurf->plane;
			dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
			if ((!(psurf->flags & SURF_PLANEBACK) || dot >= -BACKFACE_EPSILON) && (psurf->flags & SURF_PLANEBACK || dot <= BACKFACE_EPSILON))
				continue;
		}

        R_ChainSurface (psurf, chain);
        if (!r_gpulightmapupdate.value)
            R_RenderDynamicLightmaps (psurf);
        else if (psurf->lightmaptexturenum >= 0)
            Atomic_StoreUInt32(&lightmaps[psurf->lightmaptexturenum].modified, true);
        Atomic_IncrementUInt32 (&rs_brushpolys);
    }

	R_DrawTextureChains (cbx, clmodel, e, chain, entuniqueid);
	R_DrawTextureChains_Water (cbx, clmodel, e, chain, entuniqueid);
}

/*
=================
R_DrawBrushModel_ShowTris -- johnfitz
=================
*/
void R_DrawBrushModel_ShowTris (cb_context_t *cbx, entity_t *e)
{
	int         i;
	msurface_t *psurf;
	float       dot;
	mplane_t   *pplane;
	qmodel_t   *clmodel;
	float       color[] = {1.0f, 1.0f, 1.0f};
	const float alpha = 1.0f;
	vec3_t      modelorg;

	if (R_CullModelForEntity (e))
		return;

	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t temp;
		vec3_t forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	const RgTransform tr = RT_GetBrushModelMatrix (e);

	//
	// draw it
	//
	for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) || (!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			DrawGLPoly (
				cbx, RT_UNIQUEID_DONTCARE,
				psurf->polys, color, alpha, 
				&tr, NULL,
				CVAR_TO_BOOL (r_showtris) ? DRAW_GL_POLY_TYPE_SHOWTRI : DRAW_GL_POLY_TYPE_SHOWTRI_NODEPTH);
		}
	}
}

/*
=============================================================

    LIGHTMAPS

=============================================================
*/

/*
================
R_RenderDynamicLightmaps
called during rendering
================
*/
void R_RenderDynamicLightmaps (msurface_t *fa)
{
	byte     *base;
	int       maps;
	glRect_t *theRect;
	int       smax, tmax;

	if (fa->flags & SURF_DRAWTILED) // johnfitz -- not a lightmapped surface
		return;

	// check for lightmap modification
	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount // dynamic this frame
	    || fa->cached_dlight)           // dynamic previously
	{
	dynamic:
		if (r_dynamic.value)
		{
			struct lightmap_s *lm = &lightmaps[fa->lightmaptexturenum];
			Atomic_StoreUInt32(&lm->modified, true);
			theRect = &lm->rectchange;
			if (fa->light_t < theRect->t)
			{
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l)
			{
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0] >> 4) + 1;
			tmax = (fa->extents[1] >> 4) + 1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s - theRect->l) + smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t - theRect->t) + tmax;
			base = lm->data;
			base += fa->light_t * LMBLOCK_WIDTH * LIGHTMAP_BYTES + fa->light_s * LIGHTMAP_BYTES;
			R_BuildLightMap (fa, base, LMBLOCK_WIDTH * LIGHTMAP_BYTES);
		}
	}
}

/*
========================
AllocBlock -- returns a texture number and the position inside it
========================
*/
static int AllocBlock (int w, int h, int *x, int *y)
{
	int i;
	int texnum;

	for (texnum = last_lightmap_allocated; texnum < MAX_SANITY_LIGHTMAPS; texnum++)
	{
		if (texnum == lightmap_count)
		{
			lightmap_count++;
			lightmaps = (struct lightmap_s *)Mem_Realloc (lightmaps, sizeof (*lightmaps) * lightmap_count);
			memset (&lightmaps[texnum], 0, sizeof (lightmaps[texnum]));
			lightmaps[texnum].data = (byte *)Mem_Alloc (LIGHTMAP_BYTES * LMBLOCK_WIDTH * LMBLOCK_HEIGHT);
			memset (used_columns[texnum], 0, sizeof (used_columns[texnum]));
			last_lightmap_allocated = texnum;
		}

		i = w - 1;
		if (columns[i] < 0 || rows[i] + h - shelf_idx[i] * SHELF_HEIGHT > SHELF_HEIGHT) // need another shelf
		{
			while (used_columns[lightmap_idx[i]][shelf_idx[i]] + w > LMBLOCK_WIDTH)
			{
				if (++shelf_idx[i] < SHELVES)
					continue;
				shelf_idx[i] = 0;
				if (++lightmap_idx[i] == lightmap_count)
					break;
			}
			if (lightmap_idx[i] == lightmap_count) // need another lightmap
				continue;

			columns[i] = used_columns[lightmap_idx[i]][shelf_idx[i]];
			used_columns[lightmap_idx[i]][shelf_idx[i]] += w;
			rows[i] = shelf_idx[i] * SHELF_HEIGHT;
		}
		*x = columns[i];
		*y = rows[i];
		rows[i] += h;
		return lightmap_idx[i];
	}

	Sys_Error ("AllocBlock: full");
	return 0; // johnfitz -- shut up compiler
}

mvertex_t *r_pcurrentvertbase;
qmodel_t  *currentmodel;

int nColinElim;

/*
===============
R_AssignSurfaceIndex
===============
*/
static void R_AssignSurfaceIndex (msurface_t *surf, uint32_t index, uint32_t *surface_indices, int stride)
{
	int width = (surf->extents[0] >> 4) + 1;
	int height = (surf->extents[1] >> 4) + 1;

	stride -= width;
	while (height-- > 0)
	{
		int i;
		for (i = 0; i < width; i++)
		{
			*surface_indices++ = index;
		}
		surface_indices += stride;
	}
}

/*
===============
R_FillLightstyleTexture
===============
*/
static void R_FillLightstyleTextures (msurface_t *surf, byte **lightstyles, int stride)
{
	int   smax, tmax;
	byte *lightmap;
	int   maps;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	lightmap = surf->samples;
	stride -= smax * LIGHTMAP_BYTES;

	// add all the lightmaps
	if (lightmap)
	{
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; ++maps)
		{
			int height = tmax;
			while (height-- > 0)
			{
				int i;
				for (i = 0; i < smax; i++)
				{
					*lightstyles[maps]++ = *lightmap++;
					*lightstyles[maps]++ = *lightmap++;
					*lightstyles[maps]++ = *lightmap++;
					*lightstyles[maps]++ = 0;
				}
				lightstyles[maps] += stride;
			}
		}
	}
}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
static void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int   smax, tmax;
	byte *base;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	base = lightmaps[surf->lightmaptexturenum].data;
	base += (surf->light_t * LMBLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;
	R_BuildLightMap (surf, base, LMBLOCK_WIDTH * LIGHTMAP_BYTES);
}

/*
================
BuildSurfaceDisplayList -- called at level load time
================
*/
void BuildSurfaceDisplayList (msurface_t *fa)
{
	int       i, lindex, lnumverts;
	medge_t  *pedges, *r_pedge;
	float    *vec;
	float     s, t, s0, t0, sdiv, tdiv;
	glpoly_t *poly;
	float    *poly_vert;

	// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	//
	// draw texture
	//
	poly = (glpoly_t *)Mem_Alloc (sizeof (glpoly_t) + (lnumverts - 4) * VERTEXSIZE * sizeof (float));
	poly->next = fa->polys;
	fa->polys = poly;
	poly->numverts = lnumverts;

	if (fa->flags & SURF_DRAWTURB)
	{
		// match Mod_PolyForUnlitSurface
		s0 = t0 = 0.f;
		sdiv = tdiv = 128.f;
	}
	else
	{
		s0 = fa->texinfo->vecs[0][3];
		t0 = fa->texinfo->vecs[1][3];
		sdiv = fa->texinfo->texture->width;
		tdiv = fa->texinfo->texture->height;
	}

	for (i = 0; i < lnumverts; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + s0;
		s /= sdiv;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + t0;
		t /= tdiv;

		poly_vert = &poly->verts[0][0] + (i * VERTEXSIZE);
		VectorCopy (vec, poly_vert);
		poly_vert[3] = s;
		poly_vert[4] = t;

		// Q64 RERELEASE texture shift
		if (fa->texinfo->texture->shift > 0)
		{
			poly_vert[3] /= (2 * fa->texinfo->texture->shift);
			poly_vert[4] /= (2 * fa->texinfo->texture->shift);
		}

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s * 16;
		s += 8;
		s /= LMBLOCK_WIDTH * 16; // fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t * 16;
		t += 8;
		t /= LMBLOCK_HEIGHT * 16; // fa->texinfo->texture->height;

		poly_vert[5] = s;
		poly_vert[6] = t;
	}

	// johnfitz -- removed gl_keeptjunctions code

	poly->numverts = lnumverts;
}

/*
==================
GL_BuildLightmaps -- called at level load time

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
	char                       name[32];
	int                        i, j;
	struct lightmap_s         *lm;
	qmodel_t                  *m;
	msurface_t                *surf;

	GL_WaitForDeviceIdle ();

	r_framecount = 1; // no dlightcache

	// Spike -- wipe out all the lightmap data (johnfitz -- the gltexture objects were already freed by Mod_ClearAll)
	for (i = 0; i < lightmap_count; i++)
	{
		Mem_Free (lightmaps[i].data);
	}

	Mem_Free (lightmaps);
	lightmaps = NULL;
	last_lightmap_allocated = 0;
	lightmap_count = 0;
	memset (columns, -1, sizeof (columns));
	memset (lightmap_idx, 0, sizeof (lightmap_idx));
	memset (shelf_idx, 0, sizeof (shelf_idx));
	
	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i = 0; i < m->numsurfaces; i++)
		{
			surf = &m->surfaces[i];
			if (surf->flags & SURF_DRAWTILED)
				continue;

			GL_CreateSurfaceLightmap (surf);
			BuildSurfaceDisplayList (surf);
		}
	}

	//
	// upload all lightmaps that were filled
	//
	for (i = 0; i < lightmap_count; i++)
	{
		lm = &lightmaps[i];
		Atomic_StoreUInt32(&lm->modified, false);
		lm->rectchange.l = LMBLOCK_WIDTH;
		lm->rectchange.t = LMBLOCK_HEIGHT;
		lm->rectchange.w = 0;
		lm->rectchange.h = 0;

		sprintf (name, "lightmap%07i", i);
		lm->texture = TexMgr_LoadImage (
			NULL, 
			cl.worldmodel, 
			name, LMBLOCK_WIDTH, LMBLOCK_HEIGHT, SRC_LIGHTMAP, 
			lm->data, "", (src_offset_t)lm->data, TEXPREF_LINEAR | TEXPREF_NOPICMIP);
	}


	// johnfitz -- warn about exceeding old limits
	// GLQuake limit was 64 textures of 128x128. Estimate how many 128x128 textures we would need
	// given that we are using lightmap_count of LMBLOCK_WIDTH x LMBLOCK_HEIGHT
	i = lightmap_count * ((LMBLOCK_WIDTH / 128) * (LMBLOCK_HEIGHT / 128));
	if (i > 64)
		Con_DWarning ("%i lightmaps exceeds standard limit of 64.\n", i);
	// johnfitz
}

/*
=============================================================

    VBO support

=============================================================
*/

void GL_DeleteBModelVertexBuffer (void)
{
	GL_WaitForDeviceIdle ();

	Mem_Free (rtallbrushvertices);
}

/*
==================
GL_BuildBModelVertexBuffer

Deletes gl_bmodel_vbo if it already exists, then rebuilds it with all
surfaces from world + all brush models
==================
*/
void GL_BuildBModelVertexBuffer (void)
{
    // count all verts in all models
	int numverts = 0;
	for (int j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];

		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (int i = 0; i < m->numsurfaces; i++)
		{
			numverts += m->surfaces[i].numedges;
		}
	}

	rtallbrushvertices = Mem_Alloc (sizeof (RgVertex) * numverts);
	memset (rtallbrushvertices, 0, sizeof (RgVertex) * numverts);

    int varray_index = 0;
	for (int j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];

		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (int i = 0; i < m->numsurfaces; i++)
		{
			msurface_t *s = &m->surfaces[i];

			s->vbo_firstvert = varray_index;

			RgVertex *const dst = &rtallbrushvertices[varray_index];

			for (int v = 0; v < s->numedges; v++)
			{
				const float *srcv = s->polys->verts[v];

				// xyz
				dst[v].position[0] = srcv[0];
				dst[v].position[1] = srcv[1];
				dst[v].position[2] = srcv[2];

				// s1t1
				dst[v].texCoord[0] = srcv[3];
				dst[v].texCoord[1] = srcv[4];

				// s2t2
				dst[v].texCoordLayer1[0] = srcv[5];
				dst[v].texCoordLayer1[1] = srcv[6];

				dst[v].packedColor = RT_PACKED_COLOR_WHITE;
			}

			varray_index += s->numedges;
		}
	}
}

/*
=================
SoA_FillBoxLane
=================
*/
void SoA_FillBoxLane (soa_aabb_t *boxes, int index, vec3_t mins, vec3_t maxs)
{
	float *dst = boxes[index >> 3];
	index &= 7;
	dst[index + 0] = mins[0];
	dst[index + 8] = maxs[0];
	dst[index + 16] = mins[1];
	dst[index + 24] = maxs[1];
	dst[index + 32] = mins[2];
	dst[index + 40] = maxs[2];
}

/*
=================
SoA_FillPlaneLane
=================
*/
void SoA_FillPlaneLane (soa_plane_t *planes, int index, mplane_t *src, qboolean flip)
{
	float  side = flip ? -1.0f : 1.0f;
	float *dst = planes[index >> 3];
	index &= 7;
	dst[index + 0] = side * src->normal[0];
	dst[index + 8] = side * src->normal[1];
	dst[index + 16] = side * src->normal[2];
	dst[index + 24] = side * src->dist;
}

/*
===============
GL_PrepareSIMDData
===============
*/
void GL_PrepareSIMDData (void)
{
#ifdef USE_SIMD
	int i;

	cl.worldmodel->soa_leafbounds = Mem_Alloc (6 * sizeof (float) * ((cl.worldmodel->numleafs + 31) & ~7));
	cl.worldmodel->surfvis = Mem_Alloc (((cl.worldmodel->numsurfaces + 31) / 8));
	cl.worldmodel->soa_surfplanes = Mem_Alloc (4 * sizeof (float) * ((cl.worldmodel->numsurfaces + 31) & ~7));

	for (i = 0; i < cl.worldmodel->numleafs; ++i)
	{
		mleaf_t *leaf = &cl.worldmodel->leafs[i + 1];
		SoA_FillBoxLane (cl.worldmodel->soa_leafbounds, i, leaf->minmaxs, leaf->minmaxs + 3);
	}

	for (i = 0; i < cl.worldmodel->numsurfaces; ++i)
	{
		msurface_t *surf = &cl.worldmodel->surfaces[i];
		SoA_FillPlaneLane (cl.worldmodel->soa_surfplanes, i, surf->plane, surf->flags & SURF_PLANEBACK);
	}
#endif // def USE_SIMD
}

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int         lnum;
	int         sd, td;
	float       dist, rad, minlight;
	vec3_t      impact, local;
	int         s, t;
	int         i;
	int         smax, tmax;
	mtexinfo_t *tex;
	// johnfitz -- lit support via lordhavoc
	float       cred, cgreen, cblue, brightness;
	unsigned   *bl;
	// johnfitz

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	tex = surf->texinfo;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		if (!(surf->dlightbits[lnum >> 5] & (1U << (lnum & 31))))
			continue; // not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) - surf->plane->dist;
		rad -= fabs (dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i = 0; i < 3; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] - surf->plane->normal[i] * dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		// johnfitz -- lit support via lordhavoc
		bl = blocklights;
		cred = cl_dlights[lnum].color[0] * 256.0f;
		cgreen = cl_dlights[lnum].color[1] * 256.0f;
		cblue = cl_dlights[lnum].color[2] * 256.0f;
		// johnfitz
		for (t = 0; t < tmax; t++)
		{
			td = local[1] - t * 16;
			if (td < 0)
				td = -td;
			for (s = 0; s < smax; s++)
			{
				sd = local[0] - s * 16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td >> 1);
				else
					dist = td + (sd >> 1);
				if (dist < minlight)
				// johnfitz -- lit support via lordhavoc
				{
					brightness = rad - dist;
					bl[0] += (int)(brightness * cred);
					bl[1] += (int)(brightness * cgreen);
					bl[2] += (int)(brightness * cblue);
				}
				bl += 3;
				// johnfitz
			}
		}
	}
}

/*
===============
R_AccumulateLightmap

Scales 'lightmap' contents (RGB8) by 'scale' and accumulates
the result in the 'blocklights' array (RGB32)
===============
*/
void R_AccumulateLightmap (byte *lightmap, unsigned scale, int texels)
{
	unsigned *bl = blocklights;
	int       size = texels * 3;

#ifdef USE_SSE2
	if (use_simd && size >= 8)
	{
		__m128i vscale = _mm_set1_epi16 (scale);
		__m128i vlo, vhi, vdst, vsrc, v;

		while (size >= 8)
		{
			vsrc = _mm_loadl_epi64 ((const __m128i *)lightmap);

			v = _mm_unpacklo_epi8 (vsrc, _mm_setzero_si128 ());
			vlo = _mm_mullo_epi16 (v, vscale);
			vhi = _mm_mulhi_epu16 (v, vscale);

			vdst = _mm_loadu_si128 ((const __m128i *)bl);
			vdst = _mm_add_epi32 (vdst, _mm_unpacklo_epi16 (vlo, vhi));
			_mm_storeu_si128 ((__m128i *)bl, vdst);
			bl += 4;

			vdst = _mm_loadu_si128 ((const __m128i *)bl);
			vdst = _mm_add_epi32 (vdst, _mm_unpackhi_epi16 (vlo, vhi));
			_mm_storeu_si128 ((__m128i *)bl, vdst);
			bl += 4;

			lightmap += 8;
			size -= 8;
		}
	}
#endif // def USE_SSE2

	while (size-- > 0)
		*bl++ += *lightmap++ * scale;
}

/*
===============
R_StoreLightmap

Converts contiguous lightmap info accumulated in 'blocklights'
from RGB32 (with 8 fractional bits) to RGBA8, saturates and
stores the result in 'dest'
===============
*/
static void R_StoreLightmap (byte *dest, int width, int height, int stride)
{
	unsigned *src = blocklights;

#ifdef USE_SSE2
	if (use_simd)
	{
		__m128i vzero = _mm_setzero_si128 ();

		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				__m128i v = _mm_srli_epi32 (_mm_loadu_si128 ((const __m128i *)src), 8);
				v = _mm_packs_epi32 (v, vzero);
				v = _mm_packus_epi16 (v, vzero);
				((uint32_t *)dest)[i] = _mm_cvtsi128_si32 (v) | 0xff000000;
				src += 3;
			}
			dest += stride;
		}
	}
	else
#endif // def USE_SSE2
	{
		stride -= width * 4;
		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				unsigned c;
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				*dest++ = 255;
			}
			dest += stride;
		}
	}
}

/*
===============
R_BuildLightMap -- johnfitz -- revised for lit support via lordhavoc

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int      smax, tmax;
	int      size;
	byte    *lightmap;
	unsigned scale;
	int      maps;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax * tmax;
	lightmap = surf->samples;

	if (cl.worldmodel->lightdata)
	{
		// clear to no light
		memset (&blocklights[0], 0, size * 3 * sizeof (unsigned int)); // johnfitz -- lit support via lordhavoc

		// add all the lightmaps
		if (lightmap)
		{
			for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				surf->cached_light[maps] = scale; // 8.8 fraction
				// johnfitz -- lit support via lordhavoc
				R_AccumulateLightmap (lightmap, scale, size);
				lightmap += size * 3;
				// johnfitz
			}
		}

		// add all the dynamic lights
		if (surf->dlightframe == r_framecount)
			R_AddDynamicLights (surf);
	}
	else
	{
		// set to full bright if no light data
		memset (&blocklights[0], 255, size * 3 * sizeof (unsigned int)); // johnfitz -- lit support via lordhavoc
	}

	R_StoreLightmap (dest, smax, tmax, stride);
}

/*
===============
R_UploadLightmap -- johnfitz -- uploads the modified lightmap to opengl if necessary

assumes lightmap texture is already bound
===============
*/
static void R_UploadLightmap (int lmap)
{
	// RT TODO: lightmap update is disabled for now
	return;

	struct lightmap_s *lm = &lightmaps[lmap];
	if (!Atomic_LoadUInt32(&lm->modified))
		return;

	Atomic_StoreUInt32(&lm->modified, false);

	// const int staging_size = LMBLOCK_WIDTH * lm->rectchange.h * 4;
	// byte *data = lm->data + lm->rectchange.t * LMBLOCK_WIDTH * LIGHTMAP_BYTES;

	RgMaterialUpdateInfo info = 
	{
		.target = lm->texture->rtmaterial,
		.textures =
			{
				.pDataAlbedoAlpha = lm->data,
			},
	};

	RgResult r = rgUpdateMaterialContents (vulkan_globals.instance, &info);
	RG_CHECK (r);

	lm->rectchange.l = LMBLOCK_WIDTH;
	lm->rectchange.t = LMBLOCK_HEIGHT;
	lm->rectchange.h = 0;
	lm->rectchange.w = 0;

	Atomic_IncrementUInt32 (&rs_dynamiclightmaps);
}


/*
=============
R_UpdateLightmaps
=============
*/
void R_UpdateLightmaps (void *unused)
{
	if (CVAR_TO_BOOL (r_gpulightmapupdate))
	{
		assert (false);
		Con_Warning ("Updating lightmaps using GPU is not implemented");
	}
}

void R_UploadLightmaps (void)
{
	int lmap;

	for (lmap = 0; lmap < lightmap_count; lmap++)
	{
		if (!Atomic_LoadUInt32(&lightmaps[lmap].modified))
			continue;

		R_UploadLightmap (lmap);
	}
}
