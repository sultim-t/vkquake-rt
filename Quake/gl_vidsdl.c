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
// gl_vidsdl.c -- SDL vid component

#include "quakedef.h"
#include "cfgfile.h"
#include "bgmusic.h"
#include "resource.h"
#include "palette.h"
#include "SDL.h"
#include "SDL_syswm.h"

#define MAX_MODE_LIST  600 // johnfitz -- was 30
#define MAX_BPPS_LIST  5
#define MAX_RATES_LIST 20
#define MAXWIDTH       10000
#define MAXHEIGHT      10000

#define MAX_SWAP_CHAIN_IMAGES 8
#define REQUIRED_COLOR_BUFFER_FEATURES                                                                                             \
	(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
	 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

#define DEFAULT_REFRESHRATE 60

typedef struct
{
	int width;
	int height;
	int refreshrate;
} vmode_t;

static vmode_t *modelist = NULL;
static int      nummodes;

static qboolean vid_initialized = false;
static qboolean has_focus = true;

static SDL_Window   *draw_context;
static SDL_SysWMinfo sys_wm_info;

static qboolean vid_locked = false; // johnfitz
static qboolean vid_changed = false;

static void VID_Menu_Init (void); // johnfitz
static void VID_Menu_f (void);    // johnfitz
static void VID_MenuDraw (cb_context_t *cbx);
static void VID_MenuKey (int key);
static void VID_Restart (qboolean set_mode);
static void VID_Restart_f (void);

static void ClearAllStates (void);

viddef_t        vid; // global video state
modestate_t     modestate = MS_UNINIT;
extern qboolean scr_initialized;
extern cvar_t   r_particles, host_maxfps, r_gpulightmapupdate;

//====================================

// johnfitz -- new cvars
static cvar_t                   vid_fullscreen = {"vid_fullscreen", "0", CVAR_ARCHIVE}; // QuakeSpasm, was "1"
static cvar_t                   vid_width = {"vid_width", "800", CVAR_ARCHIVE};         // QuakeSpasm, was 640
static cvar_t                   vid_height = {"vid_height", "600", CVAR_ARCHIVE};       // QuakeSpasm, was 480
static cvar_t                   vid_refreshrate = {"vid_refreshrate", "60", CVAR_ARCHIVE};
static cvar_t                   vid_vsync = {"vid_vsync", "0", CVAR_ARCHIVE};
static cvar_t                   vid_desktopfullscreen = {"vid_desktopfullscreen", "0", CVAR_ARCHIVE}; // QuakeSpasm
static cvar_t                   vid_borderless = {"vid_borderless", "0", CVAR_ARCHIVE};               // QuakeSpasm
static cvar_t                   vid_palettize = {"vid_palettize", "0", CVAR_ARCHIVE};
cvar_t                          vid_filter = {"vid_filter", "1", CVAR_ARCHIVE};
cvar_t                          vid_gamma = {"gamma", "1", CVAR_ARCHIVE};       // johnfitz -- moved here from view.c
cvar_t                          vid_contrast = {"contrast", "1", CVAR_ARCHIVE}; // QuakeSpasm, MarkV
cvar_t                          r_usesops = {"r_usesops", "1", CVAR_ARCHIVE};   // johnfitz

task_handle_t prev_end_rendering_task = INVALID_TASK_HANDLE;

// RT
static cvar_t rt_shadowrays = {"rt_shadowrays", "2", CVAR_ARCHIVE};

static cvar_t rt_sky_intensity = {"rt_sky_intensity", "0.05", CVAR_ARCHIVE};
static cvar_t rt_sky_saturation = {"rt_sky_saturation", "1", CVAR_ARCHIVE};

cvar_t rt_brush_metal = {"rt_brush_metal", "0.0", CVAR_ARCHIVE};
cvar_t rt_brush_rough = {"rt_brush_rough", "0.9", CVAR_ARCHIVE};
cvar_t rt_model_metal = {"rt_model_metal", "0.0", CVAR_ARCHIVE};
cvar_t rt_model_rough = {"rt_model_rough", "0.9", CVAR_ARCHIVE};

static cvar_t rt_normalmap_stren = {"rt_normalmap_stren", "1", CVAR_ARCHIVE};
static cvar_t rt_emis_mapboost = {"rt_normalmap_stren", "16", CVAR_ARCHIVE};
static cvar_t rt_emis_maxscrcolor = {"rt_emis_maxscrcolor", "125", CVAR_ARCHIVE};
static cvar_t rt_emis_geomdefault = {"rt_emis_geomdefault", "0.01", CVAR_ARCHIVE};

static cvar_t rt_reflrefr_depth = {"rt_reflrefr_depth", "2", CVAR_ARCHIVE};
static cvar_t rt_reflrefr_castshadows = {"rt_reflrefr_castshadows", "0", CVAR_ARCHIVE};
static cvar_t rt_reflrefr_toindir = {"rt_reflrefr_toindir", "0", CVAR_ARCHIVE};
static cvar_t rt_refr_glass = {"rt_refr_glass", "1.52", CVAR_ARCHIVE};
static cvar_t rt_refr_water = {"rt_refr_water", "1.33", CVAR_ARCHIVE};

static cvar_t rt_water_density = {"rt_water_density", "0.1", CVAR_ARCHIVE};
static cvar_t rt_water_colr = {"rt_water_colr", "0.025", CVAR_ARCHIVE};
static cvar_t rt_water_colg = {"rt_water_colg", "0.016", CVAR_ARCHIVE};
static cvar_t rt_water_colb = {"rt_water_colb", "0.011", CVAR_ARCHIVE};
static cvar_t rt_water_speed = {"rt_water_speed", "0.4", CVAR_ARCHIVE};
static cvar_t rt_water_normstren = {"rt_water_normstren", "1", CVAR_ARCHIVE};
static cvar_t rt_water_normsharp = {"rt_water_normsharp", "5", CVAR_ARCHIVE};
static cvar_t rt_water_scale = {"rt_water_scale", "1", CVAR_ARCHIVE};

static cvar_t rt_sharpen = {"rt_sharpen", "0", CVAR_ARCHIVE};
static cvar_t rt_renderscale = {"rt_renderscale", "100", CVAR_ARCHIVE};
static cvar_t rt_upscale_fsr2 = {"rt_upscale_fsr2", "2", CVAR_ARCHIVE};
static cvar_t rt_upscale_dlss = {"rt_upscale_dlss", "0", CVAR_ARCHIVE};

static cvar_t rt_tnmp_minlog = {"rt_tnmp_minlog", "-6", CVAR_ARCHIVE};
static cvar_t rt_tnmp_maxlog = {"rt_tnmp_maxlog", "0", CVAR_ARCHIVE};
static cvar_t rt_tnmp_white = {"rt_tnmp_white", "10", CVAR_ARCHIVE};

static cvar_t rt_bloom_intensity = {"rt_bloom_intensity", "1", CVAR_ARCHIVE};
static cvar_t rt_bloom_threshold = {"rt_bloom_threshold", "15", CVAR_ARCHIVE};
static cvar_t rt_bloom_thresholdlen = {"rt_bloom_thresholdlen", "0.25", CVAR_ARCHIVE};
static cvar_t rt_bloom_emis_mult = {"rt_bloom_emis_mult", "50", CVAR_ARCHIVE};
static cvar_t rt_bloom_satur_bias = {"rt_bloom_satur_bias", "1", CVAR_ARCHIVE};
static cvar_t rt_bloom_sky_mult = {"rt_bloom_sky_mult", "0.05", CVAR_ARCHIVE};

static cvar_t rt_ef_crt = {"rt_ef_crt", "0", CVAR_ARCHIVE};
static cvar_t rt_ef_interlacing = {"rt_ef_interlacing", "0", CVAR_ARCHIVE};
static cvar_t rt_ef_chraber = {"rt_ef_chraber", "0", CVAR_ARCHIVE};

static cvar_t rt_debugflags = {"rt_debugflags", "0", CVAR_ARCHIVE};

static qboolean request_shaders_reload = false;

/*
================
VID_Gamma_Init -- call on init
================
*/
static void VID_Gamma_Init (void)
{
	Cvar_RegisterVariable (&vid_gamma);
	Cvar_RegisterVariable (&vid_contrast);
}

/*
======================
VID_GetCurrentWidth
======================
*/
static int VID_GetCurrentWidth (void)
{
	int w = 0, h = 0;
	SDL_GetWindowSize (draw_context, &w, &h);
	return w;
}

/*
=======================
VID_GetCurrentHeight
=======================
*/
static int VID_GetCurrentHeight (void)
{
	int w = 0, h = 0;
	SDL_GetWindowSize (draw_context, &w, &h);
	return h;
}

/*
====================
VID_GetCurrentRefreshRate
====================
*/
static int VID_GetCurrentRefreshRate (void)
{
	SDL_DisplayMode mode;
	int             current_display;

	current_display = SDL_GetWindowDisplayIndex (draw_context);

	if (0 != SDL_GetCurrentDisplayMode (current_display, &mode))
		return DEFAULT_REFRESHRATE;

	return mode.refresh_rate;
}

/*
====================
VID_GetCurrentBPP
====================
*/
static int VID_GetCurrentBPP (void)
{
	const Uint32 pixelFormat = SDL_GetWindowPixelFormat (draw_context);
	return SDL_BITSPERPIXEL (pixelFormat);
}

/*
====================
VID_GetFullscreen

returns true if we are in regular fullscreen or "desktop fullscren"
====================
*/
static qboolean VID_GetFullscreen (void)
{
	return (SDL_GetWindowFlags (draw_context) & SDL_WINDOW_FULLSCREEN) != 0;
}

/*
====================
VID_GetDesktopFullscreen

returns true if we are specifically in "desktop fullscreen" mode
====================
*/
static qboolean VID_GetDesktopFullscreen (void)
{
	return (SDL_GetWindowFlags (draw_context) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
}

/*
====================
VID_GetWindow

used by pl_win.c
====================
*/
void *VID_GetWindow (void)
{
	return draw_context;
}

/*
====================
VID_HasMouseOrInputFocus
====================
*/
qboolean VID_HasMouseOrInputFocus (void)
{
	return (SDL_GetWindowFlags (draw_context) & (SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS)) != 0;
}

/*
====================
VID_IsMinimized
====================
*/
qboolean VID_IsMinimized (void)
{
	return !(SDL_GetWindowFlags (draw_context) & SDL_WINDOW_SHOWN);
}

/*
================
VID_SDL2_GetDisplayMode

Returns a pointer to a statically allocated SDL_DisplayMode structure
if there is one with the requested params on the default display.
Otherwise returns NULL.

This is passed to SDL_SetWindowDisplayMode to specify a pixel format
with the requested bpp. If we didn't care about bpp we could just pass NULL.
================
*/
static SDL_DisplayMode *VID_SDL2_GetDisplayMode (int width, int height, int refreshrate)
{
	static SDL_DisplayMode mode;
	const int              sdlmodes = SDL_GetNumDisplayModes (0);
	int                    i;

	for (i = 0; i < sdlmodes; i++)
	{
		if (SDL_GetDisplayMode (0, i, &mode) != 0)
			continue;

		if (mode.w == width && mode.h == height && SDL_BITSPERPIXEL (mode.format) >= 24 && mode.refresh_rate == refreshrate)
		{
			return &mode;
		}
	}
	return NULL;
}

/*
================
VID_ValidMode
================
*/
static qboolean VID_ValidMode (int width, int height, int refreshrate, qboolean fullscreen)
{
	// ignore width / height / bpp if vid_desktopfullscreen is enabled
	if (fullscreen && vid_desktopfullscreen.value)
		return true;

	if (width < 320)
		return false;

	if (height < 200)
		return false;

	if (fullscreen && VID_SDL2_GetDisplayMode (width, height, refreshrate) == NULL)
		return false;

	return true;
}

/*
================
VID_SetMode
================
*/
static qboolean VID_SetMode (int width, int height, int refreshrate, qboolean fullscreen)
{
	int    temp;
	Uint32 flags;
	char   caption[50];
	int    previous_display;

	// so Con_Printfs don't mess us up by forcing vid and snd updates
	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;

	CDAudio_Pause ();
	BGM_Pause ();

	q_snprintf (caption, sizeof (caption), "vkQuake " VKQUAKE_VER_STRING);

	/* Create the window if needed, hidden */
	if (!draw_context)
	{
		flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;

		if (vid_borderless.value)
			flags |= SDL_WINDOW_BORDERLESS;
		else if (!fullscreen)
			flags |= SDL_WINDOW_RESIZABLE;

		draw_context = SDL_CreateWindow (caption, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
		if (!draw_context)
			Sys_Error ("Couldn't create window: %s", SDL_GetError ());

		SDL_VERSION (&sys_wm_info.version);
		if (!SDL_GetWindowWMInfo (draw_context, &sys_wm_info))
			Sys_Error ("Couldn't get window wm info: %s", SDL_GetError ());

		previous_display = -1;
	}
	else
	{
		previous_display = SDL_GetWindowDisplayIndex (draw_context);
	}

	/* Ensure the window is not fullscreen */
	if (VID_GetFullscreen ())
	{
		if (SDL_SetWindowFullscreen (draw_context, 0) != 0)
			Sys_Error ("Couldn't set fullscreen state mode: %s", SDL_GetError ());
	}

	/* Set window size and display mode */
	SDL_SetWindowSize (draw_context, width, height);
	if (previous_display >= 0)
		SDL_SetWindowPosition (draw_context, SDL_WINDOWPOS_CENTERED_DISPLAY (previous_display), SDL_WINDOWPOS_CENTERED_DISPLAY (previous_display));
	else
		SDL_SetWindowPosition (draw_context, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_SetWindowDisplayMode (draw_context, VID_SDL2_GetDisplayMode (width, height, refreshrate));
	SDL_SetWindowBordered (draw_context, vid_borderless.value ? SDL_FALSE : SDL_TRUE);

	/* Make window fullscreen if needed, and show the window */

	if (fullscreen)
	{
		const Uint32 flag = vid_desktopfullscreen.value ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
		if (SDL_SetWindowFullscreen (draw_context, flag) != 0)
			Sys_Error ("Couldn't set fullscreen state mode: %s", SDL_GetError ());
	}

	SDL_ShowWindow (draw_context);
	SDL_RaiseWindow (draw_context);

	vid.width = VID_GetCurrentWidth ();
	vid.height = VID_GetCurrentHeight ();
	vid.conwidth = vid.width & 0xFFFFFFF8;
	vid.conheight = vid.conwidth * vid.height / vid.width;

	modestate = VID_GetFullscreen () ? MS_FULLSCREEN : MS_WINDOWED;

	CDAudio_Resume ();
	BGM_Resume ();
	scr_disabled_for_loading = temp;

	// fix the leftover Alt from any Alt-Tab or the like that switched us away
	ClearAllStates ();

	vid.recalc_refdef = 1;

	// no pending changes
	vid_changed = false;

	SCR_UpdateRelativeScale ();

	return true;
}

/*
===================
VID_Changed_f -- kristian -- notify us that a value has changed that requires a vid_restart
===================
*/
static void VID_Changed_f (cvar_t *var)
{
	vid_changed = true;
}

/*
================
VID_Test -- johnfitz -- like vid_restart, but asks for confirmation after switching modes
================
*/
static void VID_Test (void)
{
	int old_width, old_height, old_refreshrate, old_fullscreen;

	if (vid_locked || !vid_changed)
		return;
	//
	// now try the switch
	//
	old_width = VID_GetCurrentWidth ();
	old_height = VID_GetCurrentHeight ();
	old_refreshrate = VID_GetCurrentRefreshRate ();
	old_fullscreen = VID_GetFullscreen () ? 1 : 0;
	VID_Restart (true);

	// pop up confirmation dialoge
	if (!SCR_ModalMessage ("Would you like to keep this\nvideo mode? (y/n)\n", 5.0f))
	{
		// revert cvars and mode
		Cvar_SetValueQuick (&vid_width, old_width);
		Cvar_SetValueQuick (&vid_height, old_height);
		Cvar_SetValueQuick (&vid_refreshrate, old_refreshrate);
		Cvar_SetValueQuick (&vid_fullscreen, old_fullscreen);
		VID_Restart (true);
	}
}

/*
================
VID_Unlock -- johnfitz
================
*/
static void VID_Unlock (void)
{
	vid_locked = false;
	VID_SyncCvars ();
}

/*
================
VID_Lock -- ericw

Subsequent changes to vid_* mode settings, and vid_restart commands, will
be ignored until the "vid_unlock" command is run.

Used when changing gamedirs so the current settings override what was saved
in the config.cfg.
================
*/
void VID_Lock (void)
{
	vid_locked = true;
}

//==============================================================================
//
//	Vulkan Stuff
//
//==============================================================================

static void RT_PrintMessage (const char *pMessage, void *pUserData)
{
	Con_Printf (pMessage);
}

static void RT_ReloadShaders (void)
{
	request_shaders_reload = true;
}

/*
===============
GL_InitInstance
===============
*/
static void GL_InitInstance (void)
{
	SDL_SysWMinfo wmInfo;
	SDL_VERSION (&wmInfo.version);
	SDL_GetWindowWMInfo (draw_context, &wmInfo);

#ifdef RG_USE_SURFACE_WIN32
	RgWin32SurfaceCreateInfo win32Info = {.hinstance = wmInfo.info.win.hinstance, .hwnd = wmInfo.info.win.window};
#elif RG_USE_SURFACE_XLIB
	RgXlibSurfaceCreateInfo x11Info = {.dpy = wmInfo.info.x11.display, .window = wmInfo.info.x11.window};
#endif

	const char pShaderPath[] = RT_OVERRIDEN_FOLDER "shaders/";
	const char pBlueNoisePath[] = RT_OVERRIDEN_FOLDER "BlueNoise_LDR_RGBA_128.ktx2";
	const char pWaterTexturePath[] = RT_OVERRIDEN_FOLDER "WaterNormal_n.ktx2";

	RgInstanceCreateInfo info = {
		.pAppName = "QuakeRT",
		.pAppGUID = "8d1f551a-b0e4-4365-985c-5e1182f3c54a",

#ifdef RG_USE_SURFACE_WIN32
		.pWin32SurfaceInfo = &win32Info,
#elif RG_USE_SURFACE_XLIB
		.pXlibSurfaceCreateInfo = &x11Info,
#endif

		.enableValidationLayer = Cmd_CheckParm ("-rtdebug"),
		.pfnPrint = RT_PrintMessage,

		.pShaderFolderPath = pShaderPath,
		.pBlueNoiseFilePath = pBlueNoisePath,

		.primaryRaysMaxAlbedoLayers = 1,
		.indirectIlluminationMaxAlbedoLayers = 1,
		.rayCullBackFacingTriangles = 1,
		.allowGeometryWithSkyFlag = 1,

		.rasterizedMaxVertexCount = 1 << 20,
		.rasterizedMaxIndexCount = 1 << 21,

		.rasterizedVertexColorGamma = true,
		.rasterizedSkyMaxVertexCount = 1 << 16,
		.rasterizedSkyMaxIndexCount = 1 << 16,

		.rasterizedSkyCubemapSize = 256,

		.maxTextureCount = 4096,
		.textureSamplerForceMinificationFilterLinear = true,

		.pOverridenTexturesFolderPath = RT_OVERRIDEN_FOLDER "mat/",
		.overridenAlbedoAlphaTextureIsSRGB = true,
		.overridenRoughnessMetallicEmissionTextureIsSRGB = false,
		.overridenNormalTextureIsSRGB = false,

		.pWaterNormalTexturePath = pWaterTexturePath,
	};

	RgResult r = rgCreateInstance (&info, &vulkan_globals.instance);
	RG_CHECK (r);


	Cmd_AddCommand ("rt_pfnreloadshaders", &RT_ReloadShaders);
}

/*
=================
GL_BeginRenderingTask
=================
*/
void GL_BeginRenderingTask (void *unused)
{
	RgStartFrameInfo info = {
		.surfaceSize = {vid.width, vid.height},
		.requestVSync = CVAR_TO_BOOL (vid_vsync),
		.requestShaderReload = request_shaders_reload,
		.requestRasterizedSkyGeometryReuse = false,
	};

	RgResult r = rgStartFrame (vulkan_globals.instance, &info);
	RG_CHECK (r);

	request_shaders_reload = false;

	{
		cb_context_t *cbx = &vulkan_globals.primary_cb_context;
		cbx->current_canvas = CANVAS_INVALID;
	}

	for (int cbx_index = 0; cbx_index < CBX_NUM; ++cbx_index)
	{
		cb_context_t *cbx = &vulkan_globals.secondary_cb_contexts[cbx_index];
		cbx->current_canvas = CANVAS_INVALID;

		GL_SetCanvas (cbx, CANVAS_NONE);
	}
}

/*
=================
GL_SynchronizeEndRenderingTask
=================
*/
void GL_SynchronizeEndRenderingTask (void)
{
	if (prev_end_rendering_task != INVALID_TASK_HANDLE)
	{
		Task_Join (prev_end_rendering_task, SDL_MUTEX_MAXWAIT);
		prev_end_rendering_task = INVALID_TASK_HANDLE;
	}
}

/*
=================
GL_BeginRendering
=================
*/
qboolean GL_BeginRendering (qboolean use_tasks, task_handle_t *begin_rendering_task, int *x, int *y, int *width, int *height)
{
	if (!use_tasks)
		GL_SynchronizeEndRenderingTask ();

	if (vid.restart_next_frame)
	{
		VID_Restart (false);
		vid.restart_next_frame = false;
	}

	*x = *y = 0;
	*width = vid.width;
	*height = vid.height;

	if (use_tasks)
		*begin_rendering_task = Task_AllocateAndAssignFunc (GL_BeginRenderingTask, NULL, 0);
	else
		GL_BeginRenderingTask (NULL);

	return true;
}

static RgRenderSharpenTechnique GetSharpenTechniqueFromCvar ()
{
	int t = CVAR_TO_INT32 (rt_sharpen);

	switch (t)
	{
	case 2:
		return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
	case 1:
		return RG_RENDER_SHARPEN_TECHNIQUE_NAIVE;
	default:
		return RG_RENDER_SHARPEN_TECHNIQUE_NONE;
	}
}

static void UpscaleCvarsToRtgl (RgDrawFrameRenderResolutionParams *pDst)
{
	int nvDlss = CVAR_TO_INT32 (rt_upscale_dlss);
	int amdFsr = CVAR_TO_INT32 (rt_upscale_fsr2);

	switch (nvDlss)
	{
	case 1:
		// start with Quality
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY;
		break;
	case 2:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED;
		break;
	case 3:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
		break;
	case 4:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
		break;

	case 5:
		// use DLSS with rt_renderscale
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
		break;

	default:
		nvDlss = 0;
		break;
	}

	switch (amdFsr)
	{
	case 1:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY;
		break;
	case 2:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED;
		break;
	case 3:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
		break;
	case 4:
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
		break;

	case 5:
		// use FSR2 with rt_renderscale
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
		break;

	default:
		amdFsr = 0;
		break;
	}

	// both disabled
	if (nvDlss == 0 && amdFsr == 0)
	{
		pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_LINEAR;
		pDst->resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM;
	}
}

static const char *GetUpscalerOptionName (int i)
{
    switch (i)
    {
	case 0:
		return "Off";
    case 1:
		return "Quality";
	case 2:
		return "Balanced";
	case 3:
		return "Performance";
	case 4:
		return "Ultra Performance";
	default:
		return "Custom";
    }

}

typedef struct end_rendering_parms_s
{
	double  time;
	float   vid_width;
	float   vid_height;
	float	v_blend[4];
} end_rendering_parms_t;

#define DEG2RAD(a) ((a)*M_PI_DIV_180)
extern float  GL_GetCameraNear (float radfovx, float radfovy);
extern float  GL_GetCameraFar (void);
extern float  r_fovx, r_fovy;
extern cvar_t r_fastsky;
extern float  skyflatcolor[3];

/*
=================
GL_EndRenderingTask
=================
*/
static void GL_EndRenderingTask (end_rendering_parms_t *parms)
{
	qboolean is_camera_under_water = false;

	float rscl = CVAR_TO_FLOAT (rt_renderscale) / 100.0f;

	RgDrawFrameRenderResolutionParams resolution_params = {
		.sharpenTechnique = GetSharpenTechniqueFromCvar (),
		.renderSize.width = (uint32_t)(rscl * parms->vid_width),
		.renderSize.height = (uint32_t)(rscl * parms->vid_height),
		.interlacing = CVAR_TO_BOOL (rt_ef_interlacing),
	};
	UpscaleCvarsToRtgl (&resolution_params);

	RgDrawFrameShadowParams shadow_params = {
		.maxBounceShadows = CVAR_TO_UINT32 (rt_shadowrays),
		.polygonalLightSpotlightFactor = 2.0f,
		.sphericalPolygonalLightsFirefliesClamp = 3.0f,
		.lightUniqueIdIgnoreFirstPersonViewerShadows = NULL,
	};

	RgDrawFrameTonemappingParams tm_params = {
		.minLogLuminance = CVAR_TO_FLOAT (rt_tnmp_minlog),
		.maxLogLuminance = CVAR_TO_FLOAT (rt_tnmp_maxlog),
		.luminanceWhitePoint = CVAR_TO_FLOAT (rt_tnmp_white),
	};

	RgDrawFrameBloomParams bloom_params = {
		.bloomIntensity = CVAR_TO_FLOAT (rt_bloom_intensity),
		.inputThreshold = CVAR_TO_FLOAT (rt_bloom_threshold),
		.inputThresholdLength = CVAR_TO_FLOAT (rt_bloom_thresholdlen),
		.upsampleRadius = 1.0f,
		.bloomEmissionMultiplier = CVAR_TO_FLOAT (rt_bloom_emis_mult),
		.bloomEmissionSaturationBias = CVAR_TO_FLOAT (rt_bloom_satur_bias),
		.bloomSkyMultiplier = CVAR_TO_FLOAT (rt_bloom_sky_mult),
	};

	RgDrawFrameReflectRefractParams refl_refr_params = {
		.maxReflectRefractDepth = CVAR_TO_UINT32 (rt_reflrefr_depth),
		.typeOfMediaAroundCamera = is_camera_under_water ? RG_MEDIA_TYPE_WATER : RG_MEDIA_TYPE_VACUUM,
		.reflectRefractCastShadows = CVAR_TO_BOOL (rt_reflrefr_castshadows),
		.reflectRefractToIndirect = CVAR_TO_BOOL (rt_reflrefr_toindir),
		.indexOfRefractionGlass = CVAR_TO_FLOAT (rt_refr_glass),
		.indexOfRefractionWater = CVAR_TO_FLOAT (rt_refr_water),
		.waterWaveSpeed = CVAR_TO_FLOAT (rt_water_speed),
		.waterWaveNormalStrength = CVAR_TO_FLOAT (rt_water_normstren),
		.waterExtinction = {CVAR_TO_FLOAT (rt_water_colr), CVAR_TO_FLOAT (rt_water_colg), CVAR_TO_FLOAT (rt_water_colb)},
		.waterWaveTextureDerivativesMultiplier = CVAR_TO_FLOAT (rt_water_normsharp),
		.waterTextureAreaScale = CVAR_TO_FLOAT (rt_water_scale),};
	VectorScale (refl_refr_params.waterExtinction.data, CVAR_TO_FLOAT (rt_water_density), refl_refr_params.waterExtinction.data);
	
	RgDrawFrameSkyParams sky_params = {
		.skyType = CVAR_TO_BOOL (r_fastsky) ? RG_SKY_TYPE_COLOR : RG_SKY_TYPE_RASTERIZED_GEOMETRY,
		.skyColorDefault = {skyflatcolor[0], skyflatcolor[1], skyflatcolor[2]},
		.skyColorMultiplier = CVAR_TO_FLOAT (rt_sky_intensity),
		.skyColorSaturation = CVAR_TO_FLOAT (rt_sky_saturation),
		.skyViewerPosition = {0},
	};

	RgDrawFrameTexturesParams texture_params = {
		.dynamicSamplerFilter = CVAR_TO_INT32 (vid_filter) == 1 ? RG_SAMPLER_FILTER_NEAREST : RG_SAMPLER_FILTER_LINEAR,
		.normalMapStrength = CVAR_TO_FLOAT (rt_normalmap_stren),
		.emissionMapBoost = CVAR_TO_FLOAT (rt_emis_mapboost),
		.emissionMaxScreenColor = CVAR_TO_FLOAT (rt_emis_maxscrcolor),
	};

	RgDrawFrameLensFlareParams lens_flare_params = {
		.lensFlareBlendFuncSrc = RG_BLEND_FACTOR_SRC_ALPHA,
		.lensFlareBlendFuncDst = RG_BLEND_FACTOR_ONE,
	};

	RgPostEffectCRT crt_effect = {
		.isActive = CVAR_TO_BOOL (rt_ef_crt),
	};

	RgPostEffectChromaticAberration chromatic_aberration_effect = {
		.isActive = CVAR_TO_FLOAT (rt_ef_chraber) > 0.0f,
		.transitionDurationIn = 0,
		.transitionDurationOut = 0,
		.intensity = CVAR_TO_FLOAT (rt_ef_chraber),
	};

	RgPostEffectColorTint tint_effect = {
		.isActive = parms->v_blend[3] > 0,
		.transitionDurationIn = 0.0f,
		.transitionDurationOut = 0.7f,
		.intensity = parms->v_blend[3],
		.color = {parms->v_blend[0], parms->v_blend[1], parms->v_blend[2]},
	};

	RgDrawFrameDebugParams debug_params = {
		.drawFlags = CVAR_TO_UINT32 (rt_debugflags),
	};

	float cameranear = GL_GetCameraNear (DEG2RAD (r_fovx), DEG2RAD (r_fovy));
	float camerafar = GL_GetCameraFar ();

	RgDrawFrameInfo info = {
		.worldUpVector = {0, 0, 1},
		.fovYRadians = DEG2RAD (r_fovy),
		.cameraNear = cameranear,
		.cameraFar = camerafar,
		.rayCullMaskWorld = RG_DRAW_FRAME_RAY_CULL_WORLD_0_BIT | RG_DRAW_FRAME_RAY_CULL_WORLD_1_BIT | RG_DRAW_FRAME_RAY_CULL_SKY_BIT,
		.rayLength = 10000.0f,
		.primaryRayMinDist = cameranear,
		.disableRayTracing = false,
		.disableRasterization = false,
		.currentTime = parms->time,
		.disableEyeAdaptation = false,
		.pRenderResolutionParams = &resolution_params,
		.pShadowParams = &shadow_params,
		.pTonemappingParams = &tm_params,
		.pBloomParams = &bloom_params,
		.pReflectRefractParams = &refl_refr_params,
		.pSkyParams = &sky_params,
		.pTexturesParams = &texture_params,
		.pLensFlareParams = &lens_flare_params,
		.postEffectParams =
			{
				.pChromaticAberration = &chromatic_aberration_effect,
				.pColorTint = &tint_effect,
				.pCRT = &crt_effect,
			},
		.pDebugParams = &debug_params,
	};
	memcpy (info.view, vulkan_globals.view_matrix, 16 * sizeof(float));
	memcpy (info.projection, vulkan_globals.projection_matrix, 16 * sizeof(float));

	RgResult r = rgDrawFrame (vulkan_globals.instance, &info);
	RG_CHECK (r);
}

/*
=================
GL_EndRendering
=================
*/
task_handle_t GL_EndRendering (qboolean use_tasks, qboolean swapchain)
{
	end_rendering_parms_t parms = {
		.time = cl.time,
		.vid_width = (float)vid.width,
		.vid_height = (float)vid.height,
		.v_blend[0] = (float)v_blend[0] / 255.0f,
		.v_blend[1] = (float)v_blend[1] / 255.0f,
		.v_blend[2] = (float)v_blend[2] / 255.0f,
		.v_blend[3] = (float)v_blend[3] / 255.0f,
	};
	task_handle_t end_rendering_task = INVALID_TASK_HANDLE;
	if (use_tasks)
		end_rendering_task = Task_AllocateAndAssignFunc ((task_func_t)GL_EndRenderingTask, &parms, sizeof (parms));
	else
		GL_EndRenderingTask (&parms);
	return end_rendering_task;
}

/*
=================
GL_WaitForDeviceIdle
=================
*/
void GL_WaitForDeviceIdle (void)
{
	assert(!Tasks_IsWorker());
	GL_SynchronizeEndRenderingTask ();
}

/*
=================
VID_Shutdown
=================
*/
void VID_Shutdown (void)
{
	if (vid_initialized)
	{
		SDL_QuitSubSystem (SDL_INIT_VIDEO);
		draw_context = NULL;
		PL_VID_Shutdown ();
	}
}

/*
===================================================================

MAIN WINDOW

===================================================================
*/

/*
================
ClearAllStates
================
*/
static void ClearAllStates (void)
{
	Key_ClearStates ();
	IN_ClearStates ();
}

//==========================================================================
//
//  COMMANDS
//
//==========================================================================

/*
=================
VID_DescribeCurrentMode_f
=================
*/
static void VID_DescribeCurrentMode_f (void)
{
	if (draw_context)
		Con_Printf (
			"%dx%dx%d %dHz %s\n", VID_GetCurrentWidth (), VID_GetCurrentHeight (), VID_GetCurrentBPP (), VID_GetCurrentRefreshRate (),
			VID_GetFullscreen () ? "fullscreen" : "windowed");
}

/*
=================
VID_DescribeModes_f -- johnfitz -- changed formatting, and added refresh rates after each mode.
=================
*/
static void VID_DescribeModes_f (void)
{
	int i;
	int lastwidth, lastheight, count;

	lastwidth = lastheight = count = 0;

	for (i = 0; i < nummodes; i++)
	{
		if (lastwidth != modelist[i].width || lastheight != modelist[i].height)
		{
			if (count > 0)
				Con_SafePrintf ("\n");
			Con_SafePrintf ("   %4i x %4i : %i", modelist[i].width, modelist[i].height, modelist[i].refreshrate);
			lastwidth = modelist[i].width;
			lastheight = modelist[i].height;
			count++;
		}
	}
	Con_Printf ("\n%i modes\n", count);
}

//==========================================================================
//
//  INIT
//
//==========================================================================

/*
=================
VID_InitModelist
=================
*/
static void VID_InitModelist (void)
{
	const int sdlmodes = SDL_GetNumDisplayModes (0);
	int       i;

	modelist = Mem_Realloc (modelist, sizeof (vmode_t) * sdlmodes);
	nummodes = 0;
	for (i = 0; i < sdlmodes; i++)
	{
		SDL_DisplayMode mode;

		if (SDL_GetDisplayMode (0, i, &mode) == 0)
		{
			modelist[nummodes].width = mode.w;
			modelist[nummodes].height = mode.h;
			modelist[nummodes].refreshrate = mode.refresh_rate;
			nummodes++;
		}
	}
}

/*
===================
VID_Init
===================
*/
void VID_Init (void)
{
	static char vid_center[] = "SDL_VIDEO_CENTERED=center";
	int         p, width, height, refreshrate;
	int         display_width, display_height, display_refreshrate;
	qboolean    fullscreen;
	const char *read_vars[] = {"vid_fullscreen",        "vid_width",    "vid_height", "vid_refreshrate", "vid_vsync",
	                           "vid_desktopfullscreen", "vid_borderless"};
#define num_readvars (sizeof (read_vars) / sizeof (read_vars[0]))

	Cvar_RegisterVariable (&vid_fullscreen);  // johnfitz
	Cvar_RegisterVariable (&vid_width);       // johnfitz
	Cvar_RegisterVariable (&vid_height);      // johnfitz
	Cvar_RegisterVariable (&vid_refreshrate); // johnfitz
	Cvar_RegisterVariable (&vid_vsync);       // johnfitz
	Cvar_RegisterVariable (&vid_filter);
	Cvar_RegisterVariable (&vid_desktopfullscreen); // QuakeSpasm
	Cvar_RegisterVariable (&vid_borderless);        // QuakeSpasm
	Cvar_RegisterVariable (&vid_palettize);

	Cvar_SetCallback (&vid_fullscreen, VID_Changed_f);
	Cvar_SetCallback (&vid_width, VID_Changed_f);
	Cvar_SetCallback (&vid_height, VID_Changed_f);
	Cvar_SetCallback (&vid_refreshrate, VID_Changed_f);
	Cvar_SetCallback (&vid_desktopfullscreen, VID_Changed_f);
	Cvar_SetCallback (&vid_borderless, VID_Changed_f);

	// RT
	{
		Cvar_RegisterVariable (&rt_shadowrays);

		Cvar_RegisterVariable (&rt_sky_intensity);
		Cvar_RegisterVariable (&rt_sky_saturation);

		Cvar_RegisterVariable (&rt_brush_metal);
		Cvar_RegisterVariable (&rt_brush_rough);
		Cvar_RegisterVariable (&rt_model_metal);
		Cvar_RegisterVariable (&rt_model_rough);

		Cvar_RegisterVariable (&rt_normalmap_stren);
		Cvar_RegisterVariable (&rt_emis_mapboost);
		Cvar_RegisterVariable (&rt_emis_maxscrcolor);
		Cvar_RegisterVariable (&rt_emis_geomdefault);

		Cvar_RegisterVariable (&rt_reflrefr_depth);
		Cvar_RegisterVariable (&rt_reflrefr_castshadows);
		Cvar_RegisterVariable (&rt_reflrefr_toindir);
		Cvar_RegisterVariable (&rt_refr_glass);
		Cvar_RegisterVariable (&rt_refr_water);

		Cvar_RegisterVariable (&rt_water_density);
		Cvar_RegisterVariable (&rt_water_colr);
		Cvar_RegisterVariable (&rt_water_colg);
		Cvar_RegisterVariable (&rt_water_colb);
		Cvar_RegisterVariable (&rt_water_speed);
		Cvar_RegisterVariable (&rt_water_normstren);
		Cvar_RegisterVariable (&rt_water_normsharp);
		Cvar_RegisterVariable (&rt_water_scale);

		Cvar_RegisterVariable (&rt_sharpen);
		Cvar_RegisterVariable (&rt_renderscale);
		Cvar_RegisterVariable (&rt_upscale_fsr2);
		Cvar_RegisterVariable (&rt_upscale_dlss);

		Cvar_RegisterVariable (&rt_tnmp_minlog);
		Cvar_RegisterVariable (&rt_tnmp_maxlog);
		Cvar_RegisterVariable (&rt_tnmp_white);

		Cvar_RegisterVariable (&rt_bloom_intensity);
		Cvar_RegisterVariable (&rt_bloom_threshold);
		Cvar_RegisterVariable (&rt_bloom_thresholdlen);
		Cvar_RegisterVariable (&rt_bloom_emis_mult);
		Cvar_RegisterVariable (&rt_bloom_satur_bias);
		Cvar_RegisterVariable (&rt_bloom_sky_mult);

		Cvar_RegisterVariable (&rt_ef_crt);
		Cvar_RegisterVariable (&rt_ef_interlacing);
		Cvar_RegisterVariable (&rt_ef_chraber);

		Cvar_RegisterVariable (&rt_debugflags);
	}

	Cmd_AddCommand ("vid_unlock", VID_Unlock);     // johnfitz
	Cmd_AddCommand ("vid_restart", VID_Restart_f); // johnfitz
	Cmd_AddCommand ("vid_test", VID_Test);         // johnfitz
	Cmd_AddCommand ("vid_describecurrentmode", VID_DescribeCurrentMode_f);
	Cmd_AddCommand ("vid_describemodes", VID_DescribeModes_f);

#ifdef _DEBUG
	Cmd_AddCommand ("create_palette_octree", CreatePaletteOctree_f);
#endif

	putenv (vid_center); /* SDL_putenv is problematic in versions <= 1.2.9 */

	if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
		Sys_Error ("Couldn't init SDL video: %s", SDL_GetError ());

	{
		SDL_DisplayMode mode;
		if (SDL_GetDesktopDisplayMode (0, &mode) != 0)
			Sys_Error ("Could not get desktop display mode: %s\n", SDL_GetError ());

		display_width = mode.w;
		display_height = mode.h;
		display_refreshrate = mode.refresh_rate;
	}

	if (CFG_OpenConfig ("config.cfg") == 0)
	{
		CFG_ReadCvars (read_vars, num_readvars);
		CFG_CloseConfig ();
	}
	CFG_ReadCvarOverrides (read_vars, num_readvars);

	VID_InitModelist ();

	width = (int)vid_width.value;
	height = (int)vid_height.value;
	refreshrate = (int)vid_refreshrate.value;
	fullscreen = (int)vid_fullscreen.value;

	if (COM_CheckParm ("-current"))
	{
		width = display_width;
		height = display_height;
		refreshrate = display_refreshrate;
		fullscreen = true;
	}
	else
	{
		p = COM_CheckParm ("-width");
		if (p && p < com_argc - 1)
		{
			width = atoi (com_argv[p + 1]);

			if (!COM_CheckParm ("-height"))
				height = width * 3 / 4;
		}

		p = COM_CheckParm ("-height");
		if (p && p < com_argc - 1)
		{
			height = atoi (com_argv[p + 1]);

			if (!COM_CheckParm ("-width"))
				width = height * 4 / 3;
		}

		p = COM_CheckParm ("-refreshrate");
		if (p && p < com_argc - 1)
			refreshrate = atoi (com_argv[p + 1]);

		if (COM_CheckParm ("-window") || COM_CheckParm ("-w"))
			fullscreen = false;
		else if (COM_CheckParm ("-fullscreen") || COM_CheckParm ("-f"))
			fullscreen = true;
	}

	if (!VID_ValidMode (width, height, refreshrate, fullscreen))
	{
		width = (int)vid_width.value;
		height = (int)vid_height.value;
		refreshrate = (int)vid_refreshrate.value;
		fullscreen = (int)vid_fullscreen.value;
	}

	if (!VID_ValidMode (width, height, refreshrate, fullscreen))
	{
		width = 640;
		height = 480;
		refreshrate = display_refreshrate;
		fullscreen = false;
	}

	vid_initialized = true;

	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	VID_SetMode (width, height, refreshrate, fullscreen);

	// set window icon
	PL_SetWindowIcon ();

	Con_Printf ("\nRay tracing Initialization\n");
	GL_InitInstance ();

	// johnfitz -- removed code creating "glquake" subdirectory

	vid_menucmdfn = VID_Menu_f; // johnfitz
	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

	VID_Gamma_Init (); // johnfitz
	VID_Menu_Init ();  // johnfitz

	// QuakeSpasm: current vid settings should override config file settings.
	// so we have to lock the vid mode from now until after all config files are read.
	vid_locked = true;
}

/*
===================
VID_Restart
===================
*/
static void VID_Restart (qboolean set_mode)
{
	GL_SynchronizeEndRenderingTask ();

	int      width, height, refreshrate;
	qboolean fullscreen;

	width = (int)vid_width.value;
	height = (int)vid_height.value;
	refreshrate = (int)vid_refreshrate.value;
	fullscreen = vid_fullscreen.value ? true : false;

	//
	// validate new mode
	//
	if (set_mode && !VID_ValidMode (width, height, refreshrate, fullscreen))
	{
		Con_Printf ("%dx%d %dHz %s is not a valid mode\n", width, height, refreshrate, fullscreen ? "fullscreen" : "windowed");
		return;
	}

	scr_initialized = false;

	GL_WaitForDeviceIdle ();

	//
	// set new mode
	//
	if (set_mode)
		VID_SetMode (width, height, refreshrate, fullscreen);

	// conwidth and conheight need to be recalculated
	vid.conwidth = (scr_conwidth.value > 0) ? (int)scr_conwidth.value : (scr_conscale.value > 0) ? (int)(vid.width / scr_conscale.value) : vid.width;
	vid.conwidth = CLAMP (320, vid.conwidth, vid.width);
	vid.conwidth &= 0xFFFFFFF8;
	vid.conheight = vid.conwidth * vid.height / vid.width;
	//
	// keep cvars in line with actual mode
	//
	VID_SyncCvars ();

	//
	// update mouse grab
	//
	if (key_dest == key_console || key_dest == key_menu)
	{
		if (modestate == MS_WINDOWED)
			IN_Deactivate (true);
		else if (modestate == MS_FULLSCREEN)
			IN_Activate ();
	}

	SCR_UpdateRelativeScale ();

	scr_initialized = true;
}

/*
===================
VID_Restart_f -- johnfitz -- change video modes on the fly
===================
*/
static void VID_Restart_f (void)
{
	if (vid_locked || !vid_changed)
		return;
	VID_Restart (true);
}

/*
===================
VID_Toggle
new proc by S.A., called by alt-return key binding.
===================
*/
void VID_Toggle (void)
{
	qboolean toggleWorked;
	Uint32   flags = 0;

	S_ClearBuffer ();

	if (!VID_GetFullscreen ())
	{
		flags = vid_desktopfullscreen.value ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
	}

	toggleWorked = SDL_SetWindowFullscreen (draw_context, flags) == 0;
	if (toggleWorked)
	{
		modestate = VID_GetFullscreen () ? MS_FULLSCREEN : MS_WINDOWED;

		VID_SyncCvars ();

		// update mouse grab
		if (key_dest == key_console || key_dest == key_menu)
		{
			if (modestate == MS_WINDOWED)
				IN_Deactivate (true);
			else if (modestate == MS_FULLSCREEN)
				IN_Activate ();
		}
	}
}

// For settings that are not applied during vid_restart
typedef struct
{
	int host_maxfps;
	int r_waterwarp;
	int r_particles;
	int rt_upscale_fsr2;
	int rt_upscale_dlss;
	int rt_renderscale;
	int vid_filter;
	int vid_palettize;
} vid_menu_settings_t;

static vid_menu_settings_t menu_settings;

/*
================
VID_SyncCvars -- johnfitz -- set vid cvars to match current video mode
================
*/
void VID_SyncCvars (void)
{
	if (draw_context)
	{
		if (!VID_GetDesktopFullscreen ())
		{
			Cvar_SetValueQuick (&vid_width, VID_GetCurrentWidth ());
			Cvar_SetValueQuick (&vid_height, VID_GetCurrentHeight ());
		}
		Cvar_SetValueQuick (&vid_refreshrate, VID_GetCurrentRefreshRate ());
		Cvar_SetQuick (&vid_fullscreen, VID_GetFullscreen () ? "1" : "0");
		// don't sync vid_desktopfullscreen, it's a user preference that
		// should persist even if we are in windowed mode.
	}

	menu_settings.host_maxfps = CLAMP (0, host_maxfps.value, 1000);
	menu_settings.r_waterwarp = CLAMP (0, (int)r_waterwarp.value, 2);
	menu_settings.r_particles = CLAMP (0, (int)r_particles.value, 2);
	menu_settings.rt_upscale_fsr2 = CLAMP (0, CVAR_TO_INT32 (rt_upscale_fsr2), 4);
	menu_settings.rt_upscale_dlss = CLAMP (0, CVAR_TO_INT32 (rt_upscale_dlss), 4);
	menu_settings.rt_renderscale = CLAMP (20, CVAR_TO_INT32 (rt_renderscale), 150);
	menu_settings.vid_filter = CLAMP (0, (int)vid_filter.value, 1);
	menu_settings.vid_palettize = CLAMP (0, (int)vid_palettize.value, 1);

	vid_changed = false;
}

//==========================================================================
//
//  NEW VIDEO MENU -- johnfitz
//
//==========================================================================

enum
{
	VID_OPT_MODE,
	VID_OPT_BPP,
	VID_OPT_REFRESHRATE,
	VID_OPT_FULLSCREEN,
	VID_OPT_VSYNC,
	VID_OPT_MAX_FPS,
	VID_OPT_FSR2,
	VID_OPT_DLSS,
	VID_OPT_RENDER_SCALE,
	VID_OPT_FILTER,
	VID_OPT_UNDERWATER,
	VID_OPT_PARTICLES,

	VID_OPT_TEST,
	VID_OPT_APPLY,

	VIDEO_OPTIONS_ITEMS
};

static int video_options_cursor = 0;

typedef struct
{
	int width, height;
} vid_menu_mode;

// TODO: replace these fixed-length arrays with hunk_allocated buffers
static vid_menu_mode vid_menu_modes[MAX_MODE_LIST];
static int           vid_menu_nummodes = 0;

static int vid_menu_rates[MAX_RATES_LIST];
static int vid_menu_numrates = 0;

/*
================
VID_Menu_Init
================
*/
static void VID_Menu_Init (void)
{
	int i, j, h, w;

	for (i = 0; i < nummodes; i++)
	{
		w = modelist[i].width;
		h = modelist[i].height;

		for (j = 0; j < vid_menu_nummodes; j++)
		{
			if (vid_menu_modes[j].width == w && vid_menu_modes[j].height == h)
				break;
		}

		if (j == vid_menu_nummodes)
		{
			vid_menu_modes[j].width = w;
			vid_menu_modes[j].height = h;
			vid_menu_nummodes++;
		}
	}
}

/*
================
VID_Menu_RebuildRateList

regenerates rate list based on current vid_width, vid_height
================
*/
static void VID_Menu_RebuildRateList (void)
{
	int i, j, r;

	vid_menu_numrates = 0;

	for (i = 0; i < nummodes; i++)
	{
		// rate list is limited to rates available with current width/height
		if (modelist[i].width != vid_width.value || modelist[i].height != vid_height.value)
			continue;

		r = modelist[i].refreshrate;

		for (j = 0; j < vid_menu_numrates; j++)
		{
			if (vid_menu_rates[j] == r)
				break;
		}

		if (j == vid_menu_numrates)
		{
			vid_menu_rates[j] = r;
			vid_menu_numrates++;
		}
	}

	// if there are no valid fullscreen refreshrates for this width/height, just pick one
	if (vid_menu_numrates == 0)
	{
		Cvar_SetValue ("vid_refreshrate", (float)modelist[0].refreshrate);
		return;
	}

	// if vid_refreshrate is not in the new list, change vid_refreshrate
	for (i = 0; i < vid_menu_numrates; i++)
		if (vid_menu_rates[i] == (int)(vid_refreshrate.value))
			break;

	if (i == vid_menu_numrates)
		Cvar_SetValue ("vid_refreshrate", (float)vid_menu_rates[0]);
}

/*
================
VID_Menu_ChooseNextMode

chooses next resolution in order, then updates vid_width and
vid_height cvars, then updates refreshrate lists
================
*/
static void VID_Menu_ChooseNextMode (int dir)
{
	int i;

	if (vid_menu_nummodes)
	{
		for (i = 0; i < vid_menu_nummodes; i++)
		{
			if (vid_menu_modes[i].width == vid_width.value && vid_menu_modes[i].height == vid_height.value)
				break;
		}

		if (i == vid_menu_nummodes) // can't find it in list, so it must be a custom windowed res
		{
			i = 0;
		}
		else
		{
			i += dir;
			if (i >= vid_menu_nummodes)
				i = 0;
			else if (i < 0)
				i = vid_menu_nummodes - 1;
		}

		Cvar_SetValueQuick (&vid_width, (float)vid_menu_modes[i].width);
		Cvar_SetValueQuick (&vid_height, (float)vid_menu_modes[i].height);
		VID_Menu_RebuildRateList ();
	}
}

/*
================
VID_Menu_ChooseNextBpp
================
*/
static void VID_Menu_ChooseNextBpp (void)
{
	menu_settings.vid_palettize = (menu_settings.vid_palettize + 1) % 2;
}

/*
================
VID_Menu_ChooseNextMaxFPS
================
*/
static void VID_Menu_ChooseNextMaxFPS (int dir)
{
	menu_settings.host_maxfps = CLAMP (0, ((menu_settings.host_maxfps + (dir * 10)) / 10) * 10, 1000);
}

/*
================
VID_Menu_ChooseNextWaterWarp
================
*/
static void VID_Menu_ChooseNextWaterWarp (int dir)
{
	menu_settings.r_waterwarp = (menu_settings.r_waterwarp + 3 + dir) % 3;
}

/*
================
VID_Menu_ChooseNextParticles
================
*/
static void VID_Menu_ChooseNextParticles (int dir)
{
	if (dir > 0)
	{
		if (menu_settings.r_particles == 0)
			menu_settings.r_particles = 2;
		else if (menu_settings.r_particles == 2)
			menu_settings.r_particles = 1;
		else
			menu_settings.r_particles = 0;
	}
	else
	{
		if (menu_settings.r_particles == 0)
			menu_settings.r_particles = 1;
		else if (menu_settings.r_particles == 2)
			menu_settings.r_particles = 0;
		else
			menu_settings.r_particles = 2;
	}
}

/*
================
VID_Menu_ChooseNextRate

chooses next refresh rate in order, then updates vid_refreshrate cvar
================
*/
static void VID_Menu_ChooseNextRate (int dir)
{
	int i;

	for (i = 0; i < vid_menu_numrates; i++)
	{
		if (vid_menu_rates[i] == vid_refreshrate.value)
			break;
	}

	if (i == vid_menu_numrates) // can't find it in list
	{
		i = 0;
	}
	else
	{
		i += dir;
		if (i >= vid_menu_numrates)
			i = 0;
		else if (i < 0)
			i = vid_menu_numrates - 1;
	}

	Cvar_SetValue ("vid_refreshrate", (float)vid_menu_rates[i]);
}

/*
================
VID_Menu_ChooseNextFullScreenMode
================
*/
static void VID_Menu_ChooseNextFullScreenMode (int dir)
{
	Cvar_SetValueQuick (&vid_fullscreen, (float)(((int)vid_fullscreen.value + 2 + dir) % 2));
}


static void VID_Menu_ChooseNextAA (int vidopt, int dir)
{
	if (vidopt == VID_OPT_FSR2)
	{
		menu_settings.rt_upscale_fsr2 += dir < 0 ? -1 : 1;
	}
	else if (vidopt == VID_OPT_DLSS)
	{
		menu_settings.rt_upscale_dlss += dir < 0 ? -1 : 1;
	}
	else if (vidopt == VID_OPT_RENDER_SCALE)
	{
		menu_settings.rt_renderscale += dir < 0 ? -10 : 10;
	}

	menu_settings.rt_upscale_fsr2 = CLAMP (0, menu_settings.rt_upscale_fsr2, 4);
	menu_settings.rt_upscale_dlss = CLAMP (0, menu_settings.rt_upscale_dlss, 4);
	menu_settings.rt_renderscale = CLAMP (20, menu_settings.rt_renderscale, 150);

	if (menu_settings.rt_upscale_fsr2 > 0)
	{
		menu_settings.rt_upscale_dlss = 0;
		menu_settings.rt_renderscale = 100;
	}
    else if (menu_settings.rt_upscale_dlss > 0)
	{
		menu_settings.rt_upscale_fsr2 = 0;
		menu_settings.rt_renderscale = 100;
	}
	else if (menu_settings.rt_renderscale != 100)
	{
		menu_settings.rt_upscale_fsr2 = 0;
		menu_settings.rt_upscale_dlss = 0;
	}
}


/*
================
VID_MenuKey
================
*/
static void VID_MenuKey (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		VID_SyncCvars (); // sync cvars before leaving menu. FIXME: there are other ways to leave menu
		S_LocalSound ("misc/menu1.wav");
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		video_options_cursor--;
		if (video_options_cursor < 0)
			video_options_cursor = VIDEO_OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		video_options_cursor++;
		if (video_options_cursor >= VIDEO_OPTIONS_ITEMS)
			video_options_cursor = 0;
		break;

	case K_LEFTARROW:
		S_LocalSound ("misc/menu3.wav");
		switch (video_options_cursor)
		{
		case VID_OPT_MODE:
			VID_Menu_ChooseNextMode (1);
			break;
		case VID_OPT_BPP:
			VID_Menu_ChooseNextBpp ();
			break;
		case VID_OPT_REFRESHRATE:
			VID_Menu_ChooseNextRate (1);
			break;
		case VID_OPT_FULLSCREEN:
			VID_Menu_ChooseNextFullScreenMode (-1);
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n"); // kristian
			break;
		case VID_OPT_MAX_FPS:
			VID_Menu_ChooseNextMaxFPS (-1);
			break;
		case VID_OPT_FSR2:
		case VID_OPT_DLSS:
		case VID_OPT_RENDER_SCALE:
			VID_Menu_ChooseNextAA (video_options_cursor , - 1);
			break;
		case VID_OPT_FILTER:
			menu_settings.vid_filter = (menu_settings.vid_filter == 0) ? 1 : 0;
			break;
		case VID_OPT_UNDERWATER:
			VID_Menu_ChooseNextWaterWarp (-1);
			break;
		case VID_OPT_PARTICLES:
			VID_Menu_ChooseNextParticles (-1);
			break;
		default:
			break;
		}
		break;

	case K_RIGHTARROW:
		S_LocalSound ("misc/menu3.wav");
		switch (video_options_cursor)
		{
		case VID_OPT_MODE:
			VID_Menu_ChooseNextMode (-1);
			break;
		case VID_OPT_BPP:
			VID_Menu_ChooseNextBpp ();
			break;
		case VID_OPT_REFRESHRATE:
			VID_Menu_ChooseNextRate (-1);
			break;
		case VID_OPT_FULLSCREEN:
			VID_Menu_ChooseNextFullScreenMode (1);
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n");
			break;
		case VID_OPT_MAX_FPS:
			VID_Menu_ChooseNextMaxFPS (1);
			break;
		case VID_OPT_FSR2:
		case VID_OPT_DLSS:
		case VID_OPT_RENDER_SCALE:
			VID_Menu_ChooseNextAA (video_options_cursor, 1);
			break;
		case VID_OPT_FILTER:
			menu_settings.vid_filter = (menu_settings.vid_filter == 0) ? 1 : 0;
			break;
		case VID_OPT_UNDERWATER:
			VID_Menu_ChooseNextWaterWarp (1);
			break;
		case VID_OPT_PARTICLES:
			VID_Menu_ChooseNextParticles (1);
			break;
		default:
			break;
		}
		break;

	case K_ENTER:
	case K_KP_ENTER:
		m_entersound = true;
		switch (video_options_cursor)
		{
		//case VID_OPT_MODE:
		//	VID_Menu_ChooseNextMode (1);
		//	break;
		//case VID_OPT_BPP:
		//	VID_Menu_ChooseNextBpp ();
		//	break;
		//case VID_OPT_REFRESHRATE:
		//	VID_Menu_ChooseNextRate (1);
		//	break;
		case VID_OPT_FULLSCREEN:
			VID_Menu_ChooseNextFullScreenMode (1);
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n");
			break;
		//case VID_OPT_FSR2:
		//case VID_OPT_DLSS:
		//case VID_OPT_RENDER_SCALE:
		//	VID_Menu_ChooseNextAA (video_options_cursor, 1);
		//	break;
		case VID_OPT_FILTER:
			menu_settings.vid_filter = (menu_settings.vid_filter == 0) ? 1 : 0;
			break;
		case VID_OPT_UNDERWATER:
			VID_Menu_ChooseNextWaterWarp (1);
			break;
		case VID_OPT_PARTICLES:
			VID_Menu_ChooseNextParticles (1);
			break;

		case VID_OPT_TEST:
			Cbuf_AddText ("vid_test\n");
			break;

		case VID_OPT_APPLY:
			Cvar_SetValueQuick (&host_maxfps, menu_settings.host_maxfps);
			Cvar_SetValueQuick (&r_waterwarp, menu_settings.r_waterwarp);
			Cvar_SetValueQuick (&r_particles, menu_settings.r_particles);
			Cvar_SetValueQuick (&rt_upscale_fsr2, menu_settings.rt_upscale_fsr2);
			Cvar_SetValueQuick (&rt_upscale_dlss, menu_settings.rt_upscale_dlss);
			Cvar_SetValueQuick (&rt_renderscale, menu_settings.rt_renderscale);
			Cvar_SetValueQuick (&vid_filter, menu_settings.vid_filter);
			Cvar_SetValueQuick (&vid_palettize, menu_settings.vid_palettize);
			Cbuf_AddText ("vid_restart\n");

			key_dest = key_game;
			m_state = m_none;
			IN_Activate ();
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
}

/*
================
VID_MenuDraw
================
*/
static void VID_MenuDraw (cb_context_t *cbx)
{
	int         i, y;
	qpic_t     *p;
	const char *title;

	y = 4;

	// plaque
	p = Draw_CachePic ("gfx/qplaque.lmp");
	M_DrawTransPic (cbx, 16, y, p);

	// p = Draw_CachePic ("gfx/vidmodes.lmp");
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, y, p);

	y += 28;

	// title
	title = "Video Options";
	M_PrintWhite (cbx, (320 - 8 * strlen (title)) / 2, y, title);

	y += 16;

	// options
	for (i = 0; i < VIDEO_OPTIONS_ITEMS; i++)
	{
		switch (i)
		{
		case VID_OPT_MODE:
			M_Print (cbx, 16, y, "        Video mode");
			M_Print (cbx, 184, y, va ("%ix%i", (int)vid_width.value, (int)vid_height.value));
			break;
		case VID_OPT_BPP:
			M_Print (cbx, 16, y, "       Color depth");
			M_Print (cbx, 184, y, (menu_settings.vid_palettize == 1) ? "classic" : "modern");
			break;
		case VID_OPT_REFRESHRATE:
			M_Print (cbx, 16, y, "      Refresh rate");
			M_Print (cbx, 184, y, va ("%i", (int)vid_refreshrate.value));
			break;
		case VID_OPT_FULLSCREEN:
			M_Print (cbx, 16, y, "        Fullscreen");
			M_Print (cbx, 184, y, ((int)vid_fullscreen.value == 0) ? "off" : (((int)vid_fullscreen.value == 1) ? "on" : "exclusive"));
			break;
		case VID_OPT_VSYNC:
			M_Print (cbx, 16, y, "     Vertical sync");
			M_DrawCheckbox (cbx, 184, y, (int)vid_vsync.value);
			break;
		case VID_OPT_MAX_FPS:
			M_Print (cbx, 16, y, "           Max FPS");
			if (menu_settings.host_maxfps <= 0)
				M_Print (cbx, 184, y, "no limit");
			else
				M_Print (cbx, 184, y, va ("%d", menu_settings.host_maxfps));
			break;
		case VID_OPT_FSR2:
			M_Print (cbx, 16, y, "       AMD FSR 2.0");
			M_Print (cbx, 184, y, GetUpscalerOptionName(menu_settings.rt_upscale_fsr2));
			break;
		case VID_OPT_DLSS:
			M_Print (cbx, 16, y, "       Nvidia DLSS");
			M_Print (cbx, 184, y, GetUpscalerOptionName (menu_settings.rt_upscale_dlss));
			break;
		case VID_OPT_RENDER_SCALE:
			M_Print (cbx, 16, y, "      Render Scale");
			M_Print (cbx, 184, y, va ("%i%%", menu_settings.rt_renderscale) );
			break;
		case VID_OPT_FILTER:
			M_Print (cbx, 16, y, "          Textures");
			M_Print (cbx, 184, y, (menu_settings.vid_filter == 0) ? "smooth" : "classic");
			break;
		case VID_OPT_UNDERWATER:
			M_Print (cbx, 16, y, "     Underwater FX");
			M_Print (cbx, 184, y, (menu_settings.r_waterwarp == 0) ? "off" : ((menu_settings.r_waterwarp == 1) ? "Classic" : "glQuake"));
			break;
		case VID_OPT_PARTICLES:
			M_Print (cbx, 16, y, "         Particles");
			M_Print (cbx, 184, y, (menu_settings.r_particles == 0) ? "off" : ((menu_settings.r_particles == 2) ? "Classic" : "glQuake"));
			break;
		case VID_OPT_TEST:
			y += 8; // separate the test and apply items
			M_Print (cbx, 16, y, "      Test changes");
			break;
		case VID_OPT_APPLY:
			M_Print (cbx, 16, y, "     Apply changes");
			break;
		}

		if (video_options_cursor == i)
			M_DrawCharacter (cbx, 168, y, 12 + ((int)(realtime * 4) & 1));

		y += 8;
	}
}

/*
================
VID_Menu_f
================
*/
static void VID_Menu_f (void)
{
	IN_Deactivate (modestate == MS_WINDOWED);
	key_dest = key_menu;
	m_state = m_video;
	m_entersound = true;

	// set all the cvars to match the current mode when entering the menu
	VID_SyncCvars ();

	// set up bpp and rate lists based on current cvars
	VID_Menu_RebuildRateList ();
}

/*
==============================================================================

SCREEN SHOTS

==============================================================================
*/

static void SCR_ScreenShot_Usage (void)
{
	Con_Printf ("usage: screenshot <format> <quality>\n");
	Con_Printf ("   format must be \"png\" or \"tga\" or \"jpg\"\n");
	Con_Printf ("   quality must be 1-100\n");
	return;
}

/*
==================
SCR_ScreenShot_f -- johnfitz -- rewritten to use Image_WriteTGA
==================
*/
void SCR_ScreenShot_f (void)
{
	Con_Printf ("SCR_ScreenShot_f: Not implemented\n");
}

void VID_FocusGained (void)
{
	has_focus = true;
}

void VID_FocusLost (void)
{
	has_focus = false;
}
