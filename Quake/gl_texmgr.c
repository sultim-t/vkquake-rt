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

// gl_texmgr.c -- fitzquake's texture manager. manages texture images

#include "quakedef.h"
#include "gl_heap.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL.h>
#else
#include "SDL.h"
#endif

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_resize.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static cvar_t gl_max_size = {"gl_max_size", "0", CVAR_NONE};
static cvar_t gl_picmip = {"gl_picmip", "0", CVAR_NONE};

extern cvar_t vid_filter;
extern cvar_t vid_anisotropic;

extern cvar_t rt_emis_fullbright_dflt;

#define MAX_MIPS 16
static int          numgltextures;
static gltexture_t *active_gltextures, *free_gltextures;
gltexture_t        *notexture, *nulltexture, *whitetexture, *greytexture;

unsigned int d_8to24table[256];
unsigned int d_8to24table_fbright[256];
unsigned int d_8to24table_fbright_fence[256];
#if !RT_RENDERER
unsigned int d_8to24table_nobright[256];
unsigned int d_8to24table_nobright_fence[256];
#endif
unsigned int d_8to24table_conchars[256];
unsigned int d_8to24table_shirt[256];
unsigned int d_8to24table_pants[256];


SDL_mutex *texmgr_mutex;


#define RT_CUSTOMTEXTUREINFO_PATH RT_OVERRIDEN_FOLDER"texture_custom_info.txt"
#define RT_CUSTOMTEXTUREINFO_VERSION 1
struct rt_texturecustominfo_s
{
	char   rtname[64];
	vec3_t color;
	float  upoffset;
	int    type;
};
static struct rt_texturecustominfo_s *rt_texturecustominfos = NULL;
static int rt_texturecustominfos_count = 0; // -1: there are no infos, 0: uninitialized

static void RT_FillWithTextureCustomInfo (gltexture_t *dst);
static void RT_ParseTextureCustomInfos (void);


static RgMaterialCreateFlags TexMgr_GetRtFlags (gltexture_t *glt)
{
	RgMaterialCreateFlags fs = 0;

	if (glt->flags & TEXPREF_MIPMAP)
	{
		fs |= RG_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT;
	}

	// if controlled by cvar
	if (!(glt->flags & TEXPREF_NEAREST) && !(glt->flags & TEXPREF_LINEAR))
	{
		fs |= RG_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT;
	}

	if (glt->source_format == SRC_LIGHTMAP)
	{
		fs |= RG_MATERIAL_CREATE_UPDATEABLE_BIT;
	}

	return fs;
}

static RgSamplerFilter TexMgr_GetFilterMode (gltexture_t *glt)
{
	if (glt->flags & TEXPREF_NEAREST)
	{
		return RG_SAMPLER_FILTER_NEAREST;
	}

	if (glt->flags & TEXPREF_LINEAR)
	{
		return RG_SAMPLER_FILTER_LINEAR;
	}

	return CVAR_TO_INT32 (vid_filter) == 1 ? RG_SAMPLER_FILTER_NEAREST : RG_SAMPLER_FILTER_LINEAR;
}

static SDL_mutex *rtspecial_mutex;

static THREAD_LOCAL qboolean     rtspecial_started;
static THREAD_LOCAL qboolean     rtspecial_foundfullbright = false;
static THREAD_LOCAL gltexture_t *rtspecial_target = NULL;
static THREAD_LOCAL byte         rtspecial_default_rough;
static THREAD_LOCAL byte         rtspecial_default_metallic;

static THREAD_LOCAL RgMaterialCreateInfo rtspecial_info = {0};
static THREAD_LOCAL void                *rtspecial_info_albedoAlpha = NULL; // to point to data from rtspecial_info
static THREAD_LOCAL char                 rtspecial_info_pRelativePath[MAX_QPATH];


void TexMgr_RT_SpecialStart (float default_rough, float default_metallic)
{

	assert (!rtspecial_started && !rtspecial_foundfullbright && rtspecial_target == NULL);
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_started = true;
	rtspecial_default_rough = CLAMP( 0, (int)(default_rough * 255), 255);
	rtspecial_default_metallic = CLAMP (0, (int)(default_metallic * 255), 255);
}

static void TexMgr_RT_SpecialSave (gltexture_t *glt, const RgMaterialCreateInfo *info)
{
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_target = glt;
	rtspecial_info = *info;

	{
		size_t sz = sizeof (uint32_t) * glt->width * glt->height;

		rtspecial_info_albedoAlpha = Mem_Alloc (sz);
		memcpy (rtspecial_info_albedoAlpha, info->textures.pDataAlbedoAlpha, sz);
	}

	if (info->pRelativePath)
	{
		q_strlcpy (rtspecial_info_pRelativePath, info->pRelativePath, sizeof (rtspecial_info_pRelativePath));
	}
	else
	{
		rtspecial_info_pRelativePath[0] = '\0';
	}
}

static byte Luminance (byte r, byte g, byte b)
{
	float l = 0.2126f * (float)r / 255.0f + 0.7152f * (float)g / 255.0f + 0.0722f * (float)b / 255.0f;
	int   i = (int)(l * 255);
	    
	return q_min (i, 255);
}

static void FullbrightToRME (unsigned width, unsigned height, byte *fullbright)
{
	size_t pixels = (size_t)width * (size_t)height;

	while (pixels-- > 0)
	{
		byte lum = Luminance (fullbright[0], fullbright[1], fullbright[2]);

		if (lum > 0)
		{
			lum = CLAMP (0, CVAR_TO_UINT32 (rt_emis_fullbright_dflt), 255);
		}
		else
		{
			lum = 0;
		}

		// rough
		fullbright[0] = rtspecial_default_rough;
		// metallic
		fullbright[1] = rtspecial_default_metallic;
		// emissive
		fullbright[2] = lum;

		fullbright += 4;
	}
}

static void TexMgr_RT_SpecialFullbright (unsigned width, unsigned height, uint32_t *fullbright)
{
	assert (rtspecial_target != NULL && rtspecial_info_albedoAlpha != NULL);
	assert (rtspecial_info.size.width > 0 && rtspecial_info.size.height > 0);

	// strange RTGL1 limitation
	if (rtspecial_info.size.width != width || rtspecial_info.size.height != height)
	{
		Con_DWarning ("Ignoring fullbright of \"%s\", as it has different size with albedo", rtspecial_info_pRelativePath);
		assert (0);
		return;
	}

	rtspecial_foundfullbright = true;

	FullbrightToRME (width, height, (byte *)fullbright);

	rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
	rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

	rtspecial_info.textures.pDataRoughnessMetallicEmission = fullbright;

    SDL_LockMutex (rtspecial_mutex);
	RgResult r = rgCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
	RG_CHECK (r);
	SDL_UnlockMutex (rtspecial_mutex);
}

void TexMgr_RT_SpecialEnd ()
{
	assert (rtspecial_started);
	assert (rtspecial_target != NULL && rtspecial_info_albedoAlpha != NULL);

	if (!rtspecial_foundfullbright)
	{
		rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
		rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

		SDL_LockMutex (rtspecial_mutex);
		RgResult r = rgCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
		RG_CHECK (r);
		SDL_UnlockMutex (rtspecial_mutex);

	}

	Mem_Free (rtspecial_info_albedoAlpha);
	
	rtspecial_started=false;
	rtspecial_target = NULL;
	rtspecial_foundfullbright = false;
	memset (&rtspecial_info, 0, sizeof (rtspecial_info));
	rtspecial_info_albedoAlpha = NULL;
	rtspecial_info_pRelativePath[0] = '\0';

}

/*
================================================================================

    COMMANDS

================================================================================
*/

/*
===============
TexMgr_Imagelist_f -- report loaded textures
===============
*/
static void TexMgr_Imagelist_f (void)
{
	float        mb;
	float        texels = 0;
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		Con_SafePrintf ("   %4i x%4i %s\n", glt->width, glt->height, glt->name);
		if (glt->flags & TEXPREF_MIPMAP)
			texels += glt->width * glt->height * 4.0f / 3.0f;
		else
			texels += (glt->width * glt->height);
	}

	mb = (texels * 4) / 0x100000;
	Con_Printf ("%i textures %i pixels %1.1f megabytes\n", numgltextures, (int)texels, mb);
}

/*
================================================================================

    TEXTURE MANAGER

================================================================================
*/

/*
================
TexMgr_FindTexture
================
*/
gltexture_t *TexMgr_FindTexture (qmodel_t *owner, const char *name)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt = NULL;

	if (name)
	{
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			if (glt->owner == owner && !strcmp (glt->name, name))
				goto unlock_mutex;
		}
	}

unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

/*
================
TexMgr_NewTexture
================
*/
gltexture_t *TexMgr_NewTexture (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	glt = free_gltextures;
	free_gltextures = glt->next;
	glt->next = active_gltextures;
	active_gltextures = glt;

	numgltextures++;
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

static void GL_DeleteTexture (gltexture_t *texture);

/*
================
TexMgr_FreeTexture
================
*/
void TexMgr_FreeTexture (gltexture_t *kill)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	if (kill == NULL)
	{
		Con_Printf ("TexMgr_FreeTexture: NULL texture\n");
		goto unlock_mutex;
	}

	if (active_gltextures == kill)
	{
		active_gltextures = kill->next;
		kill->next = free_gltextures;
		free_gltextures = kill;

		GL_DeleteTexture (kill);
		numgltextures--;
		goto unlock_mutex;
	}

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->next == kill)
		{
			glt->next = kill->next;
			kill->next = free_gltextures;
			free_gltextures = kill;

			GL_DeleteTexture (kill);
			numgltextures--;
			goto unlock_mutex;
		}
	}

	Con_Printf ("TexMgr_FreeTexture: not found\n");
unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTextures

compares each bit in "flags" to the one in glt->flags only if that bit is active in "mask"
================
*/
void TexMgr_FreeTextures (unsigned int flags, unsigned int mask)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if ((glt->flags & mask) == (flags & mask))
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTexturesForOwner
================
*/
void TexMgr_FreeTexturesForOwner (qmodel_t *owner)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if (glt && glt->owner == owner)
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_DeleteTextureObjects
================
*/
void TexMgr_DeleteTextureObjects (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		GL_DeleteTexture (glt);
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================================================================================

    INIT

================================================================================
*/

/*
=================
TexMgr_LoadPalette -- johnfitz -- was VID_SetPalette, moved here, renamed, rewritten
=================
*/
void TexMgr_LoadPalette (void)
{
	byte *src, *dst;
	int   i;
	FILE *f;

	COM_FOpenFile ("gfx/palette.lmp", &f, NULL);
	if (!f)
		Sys_Error ("Couldn't load gfx/palette.lmp");

	byte pal[768];
	if (fread (pal, 1, 768, f) != 768)
		Sys_Error ("Couldn't load gfx/palette.lmp");
	fclose (f);

	// standard palette, 255 is transparent
	dst = (byte *)d_8to24table;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	((byte *)&d_8to24table[255])[3] = 0;

	// fullbright palette, 0-223 are black (for additive blending)
	src = pal + 224 * 3;
	dst = (byte *)&d_8to24table_fbright[224];
	for (i = 224; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 0; i < 224; i++)
	{
		dst = (byte *)&d_8to24table_fbright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

#if !RT_RENDERER
	// nobright palette, 224-255 are black (for additive blending)
	dst = (byte *)d_8to24table_nobright;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 224; i < 256; i++)
	{
		dst = (byte *)&d_8to24table_nobright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}
#endif

	// fullbright palette, for fence textures
	memcpy (d_8to24table_fbright_fence, d_8to24table_fbright, 256 * 4);
	d_8to24table_fbright_fence[255] = 0; // Alpha of zero.

#if !RT_RENDERER
	// nobright palette, for fence textures
	memcpy (d_8to24table_nobright_fence, d_8to24table_nobright, 256 * 4);
	d_8to24table_nobright_fence[255] = 0; // Alpha of zero.
#endif

	// conchars palette, 0 and 255 are transparent
	memcpy (d_8to24table_conchars, d_8to24table, 256 * 4);
	((byte *)&d_8to24table_conchars[0])[3] = 0;
}

/*
================
TexMgr_NewGame
================
*/
void TexMgr_NewGame (void)
{
	TexMgr_FreeTextures (0, TEXPREF_PERSIST); // deletes all textures where TEXPREF_PERSIST is unset
	TexMgr_LoadPalette ();
}

/*
================
TexMgr_Init

must be called before any texture loading
================
*/
void TexMgr_Init (void)
{
	int               i;
	static byte       notexture_data[16] = {159, 91, 83, 255, 0, 0, 0, 255, 0, 0, 0, 255, 159, 91, 83, 255};                    // black and pink checker
	static byte       nulltexture_data[16] = {127, 191, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 127, 191, 255, 255};              // black and blue checker
	static byte       whitetexture_data[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}; // white
	static byte       greytexture_data[16] = {127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255};  // 50% grey
	extern texture_t *r_notexture_mip, *r_notexture_mip2;

	texmgr_mutex = SDL_CreateMutex ();
	rtspecial_mutex = SDL_CreateMutex ();

	// init texture list
	free_gltextures = (gltexture_t *)Mem_Alloc (MAX_GLTEXTURES * sizeof (gltexture_t));
	active_gltextures = NULL;
	for (i = 0; i < MAX_GLTEXTURES - 1; i++)
		free_gltextures[i].next = &free_gltextures[i + 1];
	free_gltextures[i].next = NULL;
	numgltextures = 0;

	// palette
	TexMgr_LoadPalette ();

	Cvar_RegisterVariable (&gl_max_size);
	Cvar_RegisterVariable (&gl_picmip);
	Cmd_AddCommand ("imagelist", &TexMgr_Imagelist_f);

	// load notexture images
	notexture = TexMgr_LoadImage (
		NULL, NULL, "notexture", 2, 2, SRC_RGBA, notexture_data, "", (src_offset_t)notexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	nulltexture = TexMgr_LoadImage (
		NULL, NULL, "nulltexture", 2, 2, SRC_RGBA, nulltexture_data, "", (src_offset_t)nulltexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	whitetexture = TexMgr_LoadImage (
		NULL, NULL, "whitetexture", 2, 2, SRC_RGBA, whitetexture_data, "", (src_offset_t)whitetexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	greytexture = TexMgr_LoadImage (
		NULL, NULL, "greytexture", 2, 2, SRC_RGBA, greytexture_data, "", (src_offset_t)greytexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);

	// have to assign these here becuase Mod_Init is called before TexMgr_Init
	r_notexture_mip->gltexture = r_notexture_mip2->gltexture = notexture;
}

/*
================================================================================

    IMAGE LOADING

================================================================================
*/

/*
================
TexMgr_Downsample
================
*/
static unsigned *TexMgr_Downsample (unsigned *data, int in_width, int in_height, int out_width, int out_height)
{
	const int out_size_bytes = out_width * out_height * 4;

	assert ((out_width >= 1) && (out_width < in_width));
	assert ((out_height >= 1) && (out_height < in_height));

	byte *image_resize_buffer;
	TEMP_ALLOC (byte, image_resize_buffer, out_size_bytes);
	stbir_resize_uint8 ((byte *)data, in_width, in_height, 0, image_resize_buffer, out_width, out_height, 0, 4);
	memcpy (data, image_resize_buffer, out_size_bytes);
	TEMP_FREE (image_resize_buffer);

	return data;
}

/*
===============
TexMgr_AlphaEdgeFix

eliminate pink edges on sprites, etc.
operates in place on 32bit data
===============
*/
static void TexMgr_AlphaEdgeFix (byte *data, int width, int height)
{
	int   i, j, n = 0, b, c[3] = {0, 0, 0}, lastrow, thisrow, nextrow, lastpix, thispix, nextpix;
	byte *dest = data;

	for (i = 0; i < height; i++)
	{
		lastrow = width * 4 * ((i == 0) ? height - 1 : i - 1);
		thisrow = width * 4 * i;
		nextrow = width * 4 * ((i == height - 1) ? 0 : i + 1);

		for (j = 0; j < width; j++, dest += 4)
		{
			if (dest[3]) // not transparent
				continue;

			lastpix = 4 * ((j == 0) ? width - 1 : j - 1);
			thispix = 4 * j;
			nextpix = 4 * ((j == width - 1) ? 0 : j + 1);

			b = lastrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}

			// average all non-transparent neighbors
			if (n)
			{
				dest[0] = (byte)(c[0] / n);
				dest[1] = (byte)(c[1] / n);
				dest[2] = (byte)(c[2] / n);

				n = c[0] = c[1] = c[2] = 0;
			}
		}
	}
}

/*
================
TexMgr_8to32
================
*/
static void TexMgr_8to32 (byte *in, unsigned *out, int pixels, unsigned int *usepal)
{
	for (int i = 0; i < pixels; i++)
		*out++ = usepal[*in++];
}

/*
================
TexMgr_DeriveNumMips
================
*/
static int TexMgr_DeriveNumMips (int width, int height)
{
	int num_mips = 0;
	while (width >= 1 && height >= 1)
	{
		width /= 2;
		height /= 2;
		num_mips += 1;
	}
	return num_mips;
}

/*
================
TexMgr_DeriveStagingSize
================
*/
static int TexMgr_DeriveStagingSize (int width, int height)
{
	int size = 0;
	while (width >= 1 && height >= 1)
	{
		size += width * height * 4;
		width /= 2;
		height /= 2;
	}
	return size;
}

/*
================
TexMgr_PreMultiply32
================
*/
static void TexMgr_PreMultiply32 (byte *in, size_t width, size_t height)
{
	size_t pixels = width * height;
	while (pixels-- > 0)
	{
		in[0] = ((int)in[0] * (int)in[3]) >> 8;
		in[1] = ((int)in[1] * (int)in[3]) >> 8;
		in[2] = ((int)in[2] * (int)in[3]) >> 8;
		in += 4;
	}
}

/*
================
TexMgr_LoadImage32 -- handles 32bit source data
================
*/
static void TexMgr_LoadImage32 (gltexture_t *glt, unsigned *data)
{
	GL_DeleteTexture (glt);

	// do this before any rescaling
	if (glt->flags & TEXPREF_PREMULTIPLY)
		TexMgr_PreMultiply32 ((byte *)data, glt->width, glt->height);

	// mipmap down
	int picmip = (glt->flags & TEXPREF_NOPICMIP) ? 0 : q_max ((int)gl_picmip.value, 0);
	int mipwidth = q_max (glt->width >> picmip, 1);
	int mipheight = q_max (glt->height >> picmip, 1);

	int maxsize = 4096;
	if ((mipwidth > maxsize) || (mipheight > maxsize))
	{
		if (mipwidth >= mipheight)
		{
			mipheight = q_max ((mipheight * maxsize) / mipwidth, 1);
			mipwidth = maxsize;
		}
		else
		{
			mipwidth = q_max ((mipwidth * maxsize) / mipheight, 1);
			mipheight = maxsize;
		}
	}

	if ((int)glt->width != mipwidth || (int)glt->height != mipheight)
	{
		TexMgr_Downsample (data, glt->width, glt->height, mipwidth, mipheight);
		glt->width = mipwidth;
		glt->height = mipheight;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix ((byte *)data, glt->width, glt->height);
	}
	int num_mips = (glt->flags & TEXPREF_MIPMAP) ? TexMgr_DeriveNumMips (glt->width, glt->height) : 1;

	SDL_LockMutex (texmgr_mutex);
	const qboolean warp_image = (glt->flags & TEXPREF_WARPIMAGE);
	if (warp_image)
		num_mips = WARPIMAGEMIPS;

	// Check for sanity. This should never be reached.
	if (num_mips > MAX_MIPS)
		Sys_Error ("Texture has over %d mips", MAX_MIPS);

	// const qboolean lightmap = glt->source_format == SRC_LIGHTMAP;
	// const qboolean surface_indices = glt->source_format == SRC_SURF_INDICES;

	// const VkFormat format = !surface_indices ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_UINT;


	RgMaterialCreateInfo info = {
		.flags = TexMgr_GetRtFlags (glt),
		.size = {glt->width, glt->height},
		.textures =
			{
				.pDataAlbedoAlpha = data,
				.pDataRoughnessMetallicEmission = NULL,
				.pDataNormal = NULL,
			},
		.pRelativePath = glt->rtname,
		.filter = TexMgr_GetFilterMode (glt),
		.addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
	};

	if (!rtspecial_started)
	{
		SDL_LockMutex (rtspecial_mutex);
	    RgResult r = rgCreateMaterial (vulkan_globals.instance, &info, &glt->rtmaterial);
	    RG_CHECK (r);
		SDL_UnlockMutex (rtspecial_mutex);
	}
	else
	{
		if (glt->flags & TEXPREF_RT_IS_EMISSIVE)
		{
			TexMgr_RT_SpecialFullbright (glt->width, glt->height, data);
		}
		else
		{
		    TexMgr_RT_SpecialSave (glt, &info);
		}
	}

	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_LoadImage8 -- handles 8bit source data, then passes it to LoadImage32
================
*/
static void TexMgr_LoadImage8 (gltexture_t *glt, byte *data)
{
	GL_DeleteTexture (glt);

	extern cvar_t gl_fullbrights;
	unsigned int *usepal;
	int           i;

	// HACK HACK HACK -- taken from tomazquake
	if (strstr (glt->name, "shot1sid") && glt->width == 32 && glt->height == 32 && CRC_Block (data, 1024) == 65393)
	{
		// This texture in b_shell1.bsp has some of the first 32 pixels painted white.
		// They are invisible in software, but look really ugly in GL. So we just copy
		// 32 pixels from the bottom to make it look nice.
		memcpy (data, data + 32 * 31, 32);
	}

	// detect false alpha cases
	if (glt->flags & TEXPREF_ALPHA && !(glt->flags & TEXPREF_CONCHARS))
	{
		for (i = 0; i < (int)(glt->width * glt->height); i++)
			if (data[i] == 255) // transparent index
				break;
		if (i == (int)(glt->width * glt->height))
			glt->flags -= TEXPREF_ALPHA;
	}

	// choose palette and padbyte
	if (glt->flags & TEXPREF_FULLBRIGHT)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_fbright_fence;
		else
			usepal = d_8to24table_fbright;
	}
#if !RT_RENDERER
	else if (glt->flags & TEXPREF_NOBRIGHT && gl_fullbrights.value)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_nobright_fence;
		else
			usepal = d_8to24table_nobright;
	}
#endif
	else if (glt->flags & TEXPREF_CONCHARS)
	{
		usepal = d_8to24table_conchars;
	}
	else
	{
		usepal = d_8to24table;
	}

	// convert to 32bit
	unsigned *converted;
	TEMP_ALLOC (unsigned, converted, glt->width * glt->height);
	TexMgr_8to32 (data, converted, glt->width * glt->height, usepal);

	// fix edges
	if (glt->flags & TEXPREF_ALPHA)
		TexMgr_AlphaEdgeFix ((byte *)converted, glt->width, glt->height);

	// upload it
	TexMgr_LoadImage32 (glt, (unsigned *)converted);

	TEMP_FREE (converted);
}

/*
================
TexMgr_LoadLightmap -- handles lightmap data
================
*/
static void TexMgr_LoadLightmap (gltexture_t *glt, byte *data)
{
	TexMgr_LoadImage32 (glt, (unsigned *)data);
}

/*
================
TexMgr_LoadImage -- the one entry point for loading all textures
================
*/
gltexture_t *TexMgr_LoadImage (
	const char *rtname,
	qmodel_t *owner, const char *name, int width, int height, enum srcformat format, byte *data, const char *source_file, src_offset_t source_offset,
	unsigned flags)
{
	unsigned short crc = 0;
	gltexture_t   *glt;

	if (isDedicated)
		return NULL;

	RT_ParseTextureCustomInfos ();

	// cache check
	if (flags & TEXPREF_OVERWRITE)
		switch (format)
		{
		case SRC_INDEXED:
			crc = CRC_Block (data, width * height);
			break;
		case SRC_LIGHTMAP:
			crc = CRC_Block (data, width * height * LIGHTMAP_BYTES);
			break;
		case SRC_RGBA:
			crc = CRC_Block (data, width * height * 4);
			break;
		default: /* not reachable but avoids compiler warnings */
			crc = 0;
		}
	if ((flags & TEXPREF_OVERWRITE) && (glt = TexMgr_FindTexture (owner, name)))
	{
		if (glt->source_crc == crc)
			return glt;
	}
	else
		glt = TexMgr_NewTexture ();

	// copy data
	glt->owner = owner;
	q_strlcpy (glt->name, name, sizeof (glt->name));
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->shirt = -1;
	glt->pants = -1;
	q_strlcpy (glt->source_file, source_file, sizeof (glt->source_file));
	glt->source_offset = source_offset;
	glt->source_format = format;
	glt->source_width = width;
	glt->source_height = height;
	glt->source_crc = crc;

	if (rtname)
	{
	    q_strlcpy (glt->rtname, rtname, sizeof (glt->rtname));
	}
	else
	{
		glt->rtname[0] = '\0';
	}

	// upload it
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	RT_FillWithTextureCustomInfo (glt);

	return glt;
}

/*
================================================================================

    COLORMAPPING AND TEXTURE RELOADING

================================================================================
*/

/*
================
TexMgr_ReloadImage -- reloads a texture, and colormaps it if needed
================
*/
void TexMgr_ReloadImage (gltexture_t *glt, int shirt, int pants)
{
	byte  translation[256];
	byte *src, *dst, *data = NULL, *allocated = NULL, *translated = NULL;
	int   size, i;
	//
	// get source data
	//

	if (glt->source_file[0] && glt->source_offset)
	{
		// lump inside file
		FILE *f;
		COM_FOpenFile (glt->source_file, &f, NULL);
		if (!f)
			goto invalid;
		fseek (f, glt->source_offset, SEEK_CUR);
		size = glt->source_width * glt->source_height;
		/* should be SRC_INDEXED, but no harm being paranoid:  */
		if (glt->source_format == SRC_RGBA)
		{
			size *= 4;
		}
		else if (glt->source_format == SRC_LIGHTMAP)
		{
			size *= LIGHTMAP_BYTES;
		}
		allocated = data = (byte *)Mem_Alloc (size);
		if (fread (data, 1, size, f) != size)
			goto invalid;
		fclose (f);
	}
	else if (glt->source_file[0] && !glt->source_offset)
	{
		allocated = data = Image_LoadImage (glt->source_file, (int *)&glt->source_width, (int *)&glt->source_height); // simple file
	}
	else if (!glt->source_file[0] && glt->source_offset)
	{
		data = (byte *)glt->source_offset; // image in memory
	}
	if (!data)
	{
	invalid:
		Con_Printf ("TexMgr_ReloadImage: invalid source for %s\n", glt->name);
		return;
	}

	glt->width = glt->source_width;
	glt->height = glt->source_height;
	//
	// apply shirt and pants colors
	//
	// if shirt and pants are -1,-1, use existing shirt and pants colors
	// if existing shirt and pants colors are -1,-1, don't bother colormapping
	if (shirt > -1 && pants > -1)
	{
		if (glt->source_format == SRC_INDEXED)
		{
			glt->shirt = shirt;
			glt->pants = pants;
		}
		else
			Con_Printf ("TexMgr_ReloadImage: can't colormap a non SRC_INDEXED texture: %s\n", glt->name);
	}
	if (glt->shirt > -1 && glt->pants > -1)
	{
		// create new translation table
		for (i = 0; i < 256; i++)
			translation[i] = i;

		shirt = glt->shirt * 16;
		if (shirt < 128)
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + 15 - i;
		}

		pants = glt->pants * 16;
		if (pants < 128)
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + 15 - i;
		}

		// translate texture
		size = glt->width * glt->height;
		dst = translated = (byte *)Mem_Alloc (size);
		src = data;

		for (i = 0; i < size; i++)
			*dst++ = translation[*src++];

		data = translated;
	}
	//
	// upload it
	//
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	Mem_Free (translated);
	Mem_Free (allocated);
}

/*
================
TexMgr_ReloadNobrightImages -- reloads all texture that were loaded with the nobright palette.  called when gl_fullbrights changes
================
*/
void TexMgr_ReloadNobrightImages (void)
{
#if !RT_RENDERER
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->flags & TEXPREF_NOBRIGHT)
			TexMgr_ReloadImage (glt, -1, -1);
#endif
}

/*
================================================================================

    TEXTURE BINDING / TEXTURE UNIT SWITCHING

================================================================================
*/

/*
================
GL_DeleteTexture
================
*/
static void GL_DeleteTexture (gltexture_t *texture)
{
	SDL_LockMutex (texmgr_mutex);

	if (texture->rtmaterial != RG_NO_MATERIAL)
	{
		RgResult r = rgDestroyMaterial (vulkan_globals.instance, texture->rtmaterial);
		RG_CHECK (r);

		texture->rtmaterial = RG_NO_MATERIAL;
	}

	SDL_UnlockMutex (texmgr_mutex);
}


static struct rt_texturecustominfo_s *RT_PushTexCustom (const char *texname, int type)
{
	struct rt_texturecustominfo_s *dst = &rt_texturecustominfos[rt_texturecustominfos_count];
	rt_texturecustominfos_count++;

	memset (dst, 0, sizeof (*dst));
	{
		strncpy (dst->rtname, texname, sizeof (dst->rtname));
		dst->rtname[sizeof (dst->rtname) - 1] = '\0';

		dst->type = type;
	}

	return dst;
}


static void RT_ParseTextureCustomInfos (void)
{
	if (rt_texturecustominfos_count != 0)
	{
		return;
	}

	rt_texturecustominfos_count = 0;

	FILE *f = fopen (RT_CUSTOMTEXTUREINFO_PATH, "r");

    if (f == NULL)
	{
		Con_Printf ("Couldn't open %s\n", RT_CUSTOMTEXTUREINFO_PATH);
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

	rt_texturecustominfos = malloc (sizeof (struct rt_texturecustominfo_s) * alloccount);
	rewind (f);

	qboolean foundend = false;
	char     curline[256];
	int      curstate = RT_CUSTOMTEXTUREINFO_TYPE_NONE;

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
					Sys_Error (RT_CUSTOMTEXTUREINFO_PATH ": line must be < 256 characters");
				}

				curline[i] = (char)ch;
				i++;
			}

			curline[i] = '\0';
		}

		if (curline[0] == '\0' || curline[0] == '#')
		{
		    continue;
		}

		if (strncmp (curline, "@VERSION", sizeof ("@VERSION") - 1) == 0)
		{
			int version;
			int c = sscanf (curline + (sizeof ("@VERSION") - 1), "%d", &version);

			if (c != 1 || version != RT_CUSTOMTEXTUREINFO_VERSION)
			{
			    Con_Printf (RT_CUSTOMTEXTUREINFO_PATH ": incompatible version");
				rt_texturecustominfos_count = -1;
				fclose (f);

			    return;
			}
		}
		else if (strcmp (curline, "@POLY_LIGHT") == 0)
		{
			curstate = RT_CUSTOMTEXTUREINFO_TYPE_POLY_LIGHT;   
		}
		else if (strcmp (curline, "@RASTER_LIGHT") == 0)
		{
			curstate = RT_CUSTOMTEXTUREINFO_TYPE_RASTER_LIGHT;
		}
		else if (strcmp (curline, "@MIRROR") == 0)
		{
			curstate = RT_CUSTOMTEXTUREINFO_TYPE_MIRROR;
		}
		else if (strcmp (curline, "@EXACT_NORMALS") == 0)
		{
			curstate = RT_CUSTOMTEXTUREINFO_TYPE_EXACT_NORMALS;
		}
		else
		{
			char texname[64];
			char str_hexcolor[8];
			float mult;
			float upoffset;

			if (curstate == RT_CUSTOMTEXTUREINFO_TYPE_MIRROR || curstate == RT_CUSTOMTEXTUREINFO_TYPE_EXACT_NORMALS)
			{
				int c = sscanf (curline, "%s", texname);
				if (c >= 1)
				{
					RT_PushTexCustom (texname, curstate);
				}
			}
			else
			{
				int c = sscanf (curline, "%s %6s %f %f", texname, str_hexcolor, &mult, &upoffset);
			    if (c >= 2)
			    {
				    const char red[] = {str_hexcolor[0], str_hexcolor[1], '\0'};
				    uint32_t   ir = strtoul (red, NULL, 16);

				    const char green[] = {str_hexcolor[2], str_hexcolor[3], '\0'};
				    uint32_t   ig = strtoul (green, NULL, 16);

				    const char blue[] = {str_hexcolor[4], str_hexcolor[5], '\0'};
				    uint32_t   ib = strtoul (blue, NULL, 16);

			        texname[sizeof texname - 1] = '\0';

				    mult = c >= 3 ? mult : 1.0f;

				    
				    struct rt_texturecustominfo_s *dst = RT_PushTexCustom (texname, curstate);
				    {
					    dst->color[0] = (float)ir / 255.0f * mult;
					    dst->color[1] = (float)ig / 255.0f * mult;
					    dst->color[2] = (float)ib / 255.0f * mult;
				    }
					dst->upoffset = c >= 4 ? upoffset : 0.0f;
			    }
			}
		}
	}

	if (rt_texturecustominfos_count == 0)
	{
		rt_texturecustominfos_count = -1;
	}

	fclose (f);
}

static void RT_FillWithTextureCustomInfo (gltexture_t *dst)
{
	for (int i = 0; i < rt_texturecustominfos_count; i++)
	{
		if (strcmp (dst->rtname, rt_texturecustominfos[i].rtname) == 0)
		{
			memcpy (dst->rtlightcolor, rt_texturecustominfos[i].color, sizeof (vec3_t));
			dst->rtcustomtextype = rt_texturecustominfos[i].type;
			dst->rtupoffset = rt_texturecustominfos[i].upoffset;

		    return;
		}
	}

	dst->rtcustomtextype = RT_CUSTOMTEXTUREINFO_TYPE_NONE;
}