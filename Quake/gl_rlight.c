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
// r_light.c

#include "quakedef.h"

int r_dlightframecount;

extern cvar_t r_flatlightstyles; // johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_gpulightmapupdate;

extern SDL_mutex *lightcache_mutex;

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int    i, j, k, n;
	double f;

	//
	// light animations
	// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = f = cl.time * 10;
	for (j = 0; j < MAX_LIGHTSTYLES; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		// johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			k = cl_lightstyle[j].map[i % cl_lightstyle[j].length] - 'a';
			n = cl_lightstyle[j].map[(i + 1) % cl_lightstyle[j].length] - 'a';
		}
		if (!r_gpulightmapupdate.value || !r_lerplightstyles.value || (r_lerplightstyles.value < 2 && abs (n - k) >= ('m' - 'a') / 2))
			n = k;
		d_lightstylevalue[j] = (k + (n - k) * (f - i)) * 22;
		// johnfitz
	}
}

/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights -- johnfitz -- rewritten to use LordHavoc's lighting speedup
=============
*/
void R_MarkLights (dlight_t *light, int num, mnode_t *node)
{
	mplane_t    *splitplane;
	msurface_t  *surf;
	vec3_t       impact;
	float        dist, l, maxdist;
	unsigned int i;
	int          j, s, t;

start:

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	if (splitplane->type < 3)
		dist = light->origin[splitplane->type] - splitplane->dist;
	else
		dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		node = node->children[0];
		goto start;
	}
	if (dist < -light->radius)
	{
		node = node->children[1];
		goto start;
	}

	maxdist = light->radius * light->radius;
	// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		for (j = 0; j < 3; j++)
			impact[j] = light->origin[j] - surf->plane->normal[j] * dist;
		// clamp center of light to corner and check brightness
		l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
		s = l + 0.5;
		if (s < 0)
			s = 0;
		else if (s > surf->extents[0])
			s = surf->extents[0];
		s = l - s;
		l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
		t = l + 0.5;
		if (t < 0)
			t = 0;
		else if (t > surf->extents[1])
			t = surf->extents[1];
		t = l - t;
		// compare to minimum light
		if ((s * s + t * t + dist * dist) < maxdist)
		{
			if (surf->dlightframe != r_dlightframecount) // not dynamic until now
			{
				surf->dlightbits[num >> 5] = 1U << (num & 31);
				surf->dlightframe = r_dlightframecount;
			}
			else // already dynamic
				surf->dlightbits[num >> 5] |= 1U << (num & 31);
		}
	}

	if (node->children[0]->contents >= 0)
		R_MarkLights (light, num, node->children[0]);
	if (node->children[1]->contents >= 0)
		R_MarkLights (light, num, node->children[1]);
}

/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int       i;
	dlight_t *l;

	r_dlightframecount = r_framecount + 1; // because the count hasn't
	                                       //  advanced yet for this frame
	l = cl_dlights;

	for (i = 0; i < MAX_DLIGHTS; i++, l++)
	{
		if (l->die < cl.time || !l->radius)
			continue;
		R_MarkLights (l, i, cl.worldmodel->nodes);
	}
}

/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

static void InterpolateLightmap (vec3_t color, msurface_t *surf, int ds, int dt)
{
	byte *lightmap;
	int   maps, line3, dsfrac = ds & 15, dtfrac = dt & 15, r00 = 0, g00 = 0, b00 = 0, r01 = 0, g01 = 0, b01 = 0, r10 = 0, g10 = 0, b10 = 0, r11 = 0, g11 = 0,
					 b11 = 0;
	int scale;
	line3 = ((surf->extents[0] >> 4) + 1) * 3;

	lightmap = surf->samples + ((dt >> 4) * ((surf->extents[0] >> 4) + 1) + (ds >> 4)) * 3; // LordHavoc: *3 for color

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		scale = d_lightstylevalue[surf->styles[maps]];
		r00 += lightmap[0] * scale;
		g00 += lightmap[1] * scale;
		b00 += lightmap[2] * scale;
		r01 += lightmap[3] * scale;
		g01 += lightmap[4] * scale;
		b01 += lightmap[5] * scale;
		r10 += lightmap[line3 + 0] * scale;
		g10 += lightmap[line3 + 1] * scale;
		b10 += lightmap[line3 + 2] * scale;
		r11 += lightmap[line3 + 3] * scale;
		g11 += lightmap[line3 + 4] * scale;
		b11 += lightmap[line3 + 5] * scale;
		lightmap += ((surf->extents[0] >> 4) + 1) * ((surf->extents[1] >> 4) + 1) * 3; // LordHavoc: *3 for colored lighting
	}

	color[0] = ((((((((r11 - r10) * dsfrac) >> 4) + r10) - ((((r01 - r00) * dsfrac) >> 4) + r00)) * dtfrac) >> 4) + ((((r01 - r00) * dsfrac) >> 4) + r00)) *
	           (1.f / 256.f);
	color[1] = ((((((((g11 - g10) * dsfrac) >> 4) + g10) - ((((g01 - g00) * dsfrac) >> 4) + g00)) * dtfrac) >> 4) + ((((g01 - g00) * dsfrac) >> 4) + g00)) *
	           (1.f / 256.f);
	color[2] = ((((((((b11 - b10) * dsfrac) >> 4) + b10) - ((((b01 - b00) * dsfrac) >> 4) + b00)) * dtfrac) >> 4) + ((((b01 - b00) * dsfrac) >> 4) + b00)) *
	           (1.f / 256.f);
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist)
{
	float  front, back, frac;
	vec3_t mid;

loc0:
	if (node->contents < 0)
		return false; // didn't hit anything

	// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct (start, node->plane->normal) - node->plane->dist;
		back = DotProduct (end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
	//		return RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, end, maxdist);
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	// go down front side
	if (RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true; // hit something
	else
	{
		unsigned int i;
		int          ds, dt;
		msurface_t  *surf;

		surf = cl.worldmodel->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			float  sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue; // no lightmaps

			// ericw -- added double casts to force 64-bit precision.
			// Without them the zombie at the start of jam3_ericw.bsp was
			// incorrectly being lit up in SSE builds.
			ds = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			if (surf->plane->type < 3)
			{
				sfront = rayorg[surf->plane->type] - surf->plane->dist;
				sback = end[surf->plane->type] - surf->plane->dist;
			}
			else
			{
				sfront = DotProduct (rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct (end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract (end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength (raydelta);

			if (!surf->samples)
			{
				// We hit a surface that is flagged as lightmapped, but doesn't have actual lightmap info.
				// Instead of just returning black, we'll keep looking for nearby surfaces that do have valid samples.
				// This fixes occasional pitch-black models in otherwise well-lit areas in DOTM (e.g. mge1m1, mge4m1)
				// caused by overlapping surfaces with mixed lighting data.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min (*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = surf - cl.worldmodel->surfaces + 1;
				cache->ds = ds;
				cache->dt = dt;
			}
			else
			{
				cache->surfidx = -1;
			}

			return true; // success
		}

		// go down back side
		return RecursiveLightPoint (cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int R_LightPoint (vec3_t p, lightcache_t *cache, vec3_t *lightcolor)
{
	vec3_t end;
	float  maxdist = 8192.f; // johnfitz -- was 2048

	if (!cl.worldmodel->lightdata)
	{
		(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 255;
		return 255;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - maxdist;

	(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 0;

	SDL_mutex *mtx = cache->mutex ? cache->mutex : lightcache_mutex;
	SDL_LockMutex (mtx);
	if (!cache || cache->surfidx <= 0 // no cache or pitch black
	    || cache->surfidx > cl.worldmodel->numsurfaces || fabsf (cache->pos[0] - p[0]) >= 1.f || fabsf (cache->pos[1] - p[1]) >= 1.f ||
	    fabsf (cache->pos[2] - p[2]) >= 1.f)
	{
		cache->surfidx = 0;
		VectorCopy (p, cache->pos);
		RecursiveLightPoint (cache, cl.worldmodel->nodes, p, p, end, &maxdist);
	}

	if (cache && cache->surfidx > 0)
		InterpolateLightmap (*lightcolor, cl.worldmodel->surfaces + cache->surfidx - 1, cache->ds, cache->dt);
	SDL_UnlockMutex (mtx);

	return (((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]) * (1.0f / 3.0f));
}



extern cvar_t rt_elight_normaliz, rt_elight_default, rt_elight_default_mdl, rt_elight_radius, rt_elight_threshold;
extern cvar_t rt_poi_trigger, rt_poi_func, rt_poi_weapon, rt_poi_pwrup, rt_poi_armor, rt_poi_key, rt_poi_health, rt_poi_ammo;
extern cvar_t rt_poi_distthresh, rt_poi_distthresh_super;


static qboolean StartsWith (const char *val, const char *begin)
{
	return strncmp (val, begin, strlen (begin)) == 0;
}


// look https://www.gamers.org/dEngine/quake/QDP/qmapspec.html#2.3.1
// for classnames

static qboolean IsClassname_Light (const char *classname)
{
	return StartsWith (classname, "light");
}

static qboolean IsClassname_LightWithModel (const char *classname)
{
	// For example,
	//    "light_fluoro"
	//    "light_fluorospark"
	//    "light_globe"
	//    "light_torch_small_walltorch"
	//    "light_flame_small_yellow"
	//    "light_flame_large_yellow"
	//    "light_flame_small_white"
	// but not just "light"
	
	return StartsWith (classname, "light_");
}

static qboolean IsClassname_Offsetted (const char *classname)
{
	// to prevent light source being inside the flame model
	return strcmp (classname, "light_torch_small_walltorch") == 0;
}

static qboolean IsClassname_PointOfInterest (const char *classname, qboolean *out_superimportant)
{
	if (CVAR_TO_BOOL (rt_poi_trigger))
	{
		if (StartsWith (classname, "trigger") || strcmp (classname, "info_teleport_destination") == 0)
		{
			*out_superimportant = true;
			return true;
		}
	}

	if (CVAR_TO_BOOL (rt_poi_func))
	{
		if (StartsWith (classname, "func"))
		{
			*out_superimportant = true;
			return true;
		}
	}

	if (CVAR_TO_BOOL (rt_poi_weapon))
	{
		if (strcmp (classname, "item_weapon") == 0 ||
			StartsWith(classname, "weapon"))
		{
			return true;
		}
	}

	if (StartsWith (classname, "item"))
	{
		if (CVAR_TO_BOOL (rt_poi_pwrup))
		{
			if (StartsWith (classname, "item_artifact"))
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_armor))
		{
			if (StartsWith (classname, "item_armor"))
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_key))
		{
			if (strcmp (classname, "item_sigil") == 0 || 
				StartsWith (classname, "item_key"))
			{
				*out_superimportant = true;
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_ammo))
		{
			if (strcmp (classname, "item_cells") == 0 || 
				strcmp (classname, "item_rockets") == 0 || 
				strcmp (classname, "item_shells") == 0 ||
			    strcmp (classname, "item_spikes") == 0)
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_health))
		{
			if (strcmp (classname, "item_health") == 0)
			{
				return true;
			}
		}
	}

	return false;
}



typedef struct rt_poi_s
{
	vec3_t origin;
	qboolean is_super_imporant;
} rt_poi_t;

rt_poi_t *rt_poi = NULL;
int       rt_poi_count = 0;
int       rt_poi_allocated = 0;

static void RT_ParsePointsOfInterest ()
{
	rt_poi_count = 0;

    char key[128], value[4096];

    if (!cl.worldmodel)
	{
		return;
	}

	const char* data = cl.worldmodel->entities;
	if (!data)
	{
		return;
	}
	
	rt_poi_t cur_values = {0};
	int      cur_state = 0;

    #define CUR_STRUCT_STARTED 1
    #define CUR_IS_POI         2
    #define CUR_FOUND_ORIGIN   4
	    
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error

	    if (com_token[0] == '{')
	    {
			memset (&cur_values, 0, sizeof (cur_values));
			cur_state = CUR_STRUCT_STARTED;
            continue;
	    }
	    else if (com_token[0] == '}')
		{
			if ((cur_state & CUR_STRUCT_STARTED) &&
			    (cur_state & CUR_IS_POI) &&
			    (cur_state & CUR_FOUND_ORIGIN))
			{
				if (rt_poi_count >= rt_poi_allocated)
				{
					rt_poi_allocated += 256;
					rt_poi = Mem_Realloc (rt_poi, sizeof (rt_poi_t) * rt_poi_allocated);
				}

				rt_poi[rt_poi_count] = cur_values;
				rt_poi_count++;
			}

			cur_state = 0; // end of struct
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
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		
		if (strcmp (key, "classname") == 0)
		{
			qboolean is_super = 0;

			if (IsClassname_PointOfInterest (value, &is_super))
			{
				cur_state |= CUR_IS_POI;
				cur_values.is_super_imporant = is_super;
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				cur_values.origin[0] = tmpvec[0];
				cur_values.origin[1] = tmpvec[1];
				cur_values.origin[2] = tmpvec[2];
				cur_state |= CUR_FOUND_ORIGIN;
			}
		}
	}

}

static qboolean IsAroundPOI (vec3_t origin)
{
	float threshold = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_poi_distthresh));
	float threshold_loose = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_poi_distthresh_super));

	threshold *= threshold;
	threshold_loose *= threshold_loose;

	for (int i = 0; i < rt_poi_count;i++)
	{
		const rt_poi_t *src = &rt_poi[i];

		vec3_t v;
		VectorSubtract (src->origin, origin, v);

		float distsq_thresh = src->is_super_imporant ? threshold_loose : threshold;

		if (DotProduct (v, v) < distsq_thresh)
		{
			return true;
		}
	}

	return false;
}



typedef struct rt_elight_s
{
	int      state;
	vec3_t   origin;
	float    intensity;
	int      lightstyle;
	qboolean is_around_poi;
} rt_elight_t;

rt_elight_t *rt_elights = NULL;
int          rt_elights_count = 0;
int          rt_elights_allocated = 0;

// Parse worldmodel->entities, to find static lights
void RT_ParseElights ()
{
	rt_elights_count = 0;

	RT_ParsePointsOfInterest ();


	char key[128], value[4096];

    if (!cl.worldmodel)
	{
		return;
	}

	const char* data = cl.worldmodel->entities;
	if (!data)
	{
		return;
	}
	
	rt_elight_t struct_values = {0};

    #define STRUCT_STATE_STRUCT_STARTED       1
    #define STRUCT_STATE_FOUND_LIGHTCLASSNAME 2
    #define STRUCT_STATE_FOUND_ORIGIN         4
    #define STRUCT_STATE_FOUND_INTENSITY      8
    #define STRUCT_STATE_FOUND_WITH_MODEL     16
    #define STRUCT_STATE_FOUND_LIGHTSTYLE     32
    #define STRUCT_STATE_FOUND_APPLY_OFFSET   64
	
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error

	    if (com_token[0] == '{')
	    {
			memset (&struct_values, 0, sizeof (struct_values));
			struct_values.state = STRUCT_STATE_STRUCT_STARTED;
            continue;
	    }
	    else if (com_token[0] == '}')
		{
			if ((struct_values.state & STRUCT_STATE_STRUCT_STARTED) &&
			    (struct_values.state & STRUCT_STATE_FOUND_LIGHTCLASSNAME) &&
			    (struct_values.state & STRUCT_STATE_FOUND_ORIGIN))
			{
				struct_values.is_around_poi = IsAroundPOI (struct_values.origin);


				if (rt_elights_count >= rt_elights_allocated)
				{
					rt_elights_allocated += 256;
					rt_elights = Mem_Realloc (rt_elights, sizeof (rt_elight_t) * rt_elights_allocated);
				}
				rt_elights[rt_elights_count] = struct_values;
				rt_elights_count++;
			}

			struct_values.state = 0; // end of struct
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
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		
		if (strcmp (key, "classname") == 0)
		{
			if (IsClassname_Light (value))
			{
				struct_values.state |= STRUCT_STATE_FOUND_LIGHTCLASSNAME;
			}

			if (IsClassname_LightWithModel (value))
			{
				struct_values.state |= STRUCT_STATE_FOUND_WITH_MODEL;

				if (IsClassname_Offsetted (value))
				{
					struct_values.state |= STRUCT_STATE_FOUND_APPLY_OFFSET;
				}
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				struct_values.origin[0] = tmpvec[0];
				struct_values.origin[1] = tmpvec[1];
				struct_values.origin[2] = tmpvec[2];
				struct_values.state |= STRUCT_STATE_FOUND_ORIGIN;
			}
		}
		else if (strcmp (key, "light") == 0)
		{
			float tmpval = strtof(value, NULL);

		    if (tmpval > 0.0f)
			{
				struct_values.intensity = tmpval;
				struct_values.state |= STRUCT_STATE_FOUND_INTENSITY;
			}
		}
		else if (strcmp (key, "style") == 0)
		{
			int tmpval = strtol (value, NULL, 10);

			if (tmpval >= 0 && tmpval < MAX_LIGHTSTYLES)
			{
				struct_values.lightstyle = tmpval;
				struct_values.state |= STRUCT_STATE_FOUND_LIGHTSTYLE;
			}
		}
	}
}

void RT_UploadAllElights ()
{
	if (CVAR_TO_FLOAT (rt_elight_normaliz) < 0.5f)
	{
		return;
	}

	for (int i = 0; i < rt_elights_count; i++)
	{
		const rt_elight_t *src = &rt_elights[i];

		assert (src->state & STRUCT_STATE_STRUCT_STARTED);
		assert (src->state & STRUCT_STATE_FOUND_LIGHTCLASSNAME);
		assert (src->state & STRUCT_STATE_FOUND_ORIGIN);

		float quake_intensity;
		if (src->state & STRUCT_STATE_FOUND_INTENSITY)
		{
			quake_intensity = src->intensity;
		}
		else
		{
			quake_intensity = src->state & STRUCT_STATE_FOUND_WITH_MODEL ? CVAR_TO_FLOAT (rt_elight_default_mdl) : CVAR_TO_FLOAT (rt_elight_default);
		}

		qboolean accept = 
			quake_intensity >= CVAR_TO_FLOAT (rt_elight_threshold) &&
			CVAR_TO_FLOAT (rt_elight_threshold) >= 0;

		if (src->state & STRUCT_STATE_FOUND_WITH_MODEL)
		{
			accept = true;
		}

	    if ((src->state & STRUCT_STATE_FOUND_LIGHTSTYLE) && src->lightstyle > 0)
		{
			accept = true;
		}

		if (src->is_around_poi)
		{
			accept = true;
		}

		if (accept)
		{
			float intens = quake_intensity / CVAR_TO_FLOAT (rt_elight_normaliz);

			if (src->state & STRUCT_STATE_FOUND_LIGHTSTYLE)
			{
				float ls = (float)d_lightstylevalue[src->lightstyle] / 256.0f;
				intens *= CLAMP (0.0f, ls, 1.0f);
			}

			vec3_t color;
			RT_INIT_DEFAULT_LIGHT_COLOR (color);
			VectorScale (color, intens, color);
			RT_FIXUP_LIGHT_INTENSITY (color, true);

			RgSphericalLightUploadInfo info = {
				.uniqueID = (uint64_t)UINT16_MAX + i,
				.color = {color[0], color[1], color[2]},
				.position = {src->origin[0], src->origin[1], src->origin[2]},
				.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_elight_radius)),
			};

			// offset up a bit, so light is not inside the model itself
			if (src->state & STRUCT_STATE_FOUND_APPLY_OFFSET)
			{
				info.position.data[2] += METRIC_TO_QUAKEUNIT (0.75f);
			}

			RgResult r = rgUploadSphericalLight (vulkan_globals.instance, &info);
			RG_CHECK (r);
		}
	}
}