/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2011 VMWare, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Author: Alan Hourihane <alanh@tungstengraphics.com>
 * Author: Jakob Bornecrantz <wallbraker@gmail.com>
 * Author: Thomas Hellstrom <thellstrom@vmware.com>
 */


#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"
#include "compiler.h"
#include "xf86PciInfo.h"
#include "xf86Pci.h"
#include "mipointer.h"
#include "micmap.h"
#include <X11/extensions/randr.h>
#include "fb.h"
#include "edid.h"
#include "xf86i2c.h"
#include "xf86Crtc.h"
#include "miscstruct.h"
#include "dixstruct.h"
#include "xf86cmap.h"
#include "xf86xv.h"
#include "xorgVersion.h"
#ifndef XSERVER_LIBPCIACCESS
#error "libpciaccess needed"
#endif

#include <pciaccess.h>

#include "vmwgfx_driver.h"

#include <saa.h>
#include "vmwgfx_saa.h"

#define XA_VERSION_MINOR_REQUIRED 0
#define DRM_VERSION_MAJOR_REQUIRED 2
#define DRM_VERSION_MINOR_REQUIRED 3

/*
 * Some macros to deal with function wrapping.
 */
#define vmwgfx_wrap(priv, real, mem, func) {\
	(priv)->saved_##mem = (real)->mem;	\
	(real)->mem = func;			\
}

#define vmwgfx_unwrap(priv, real, mem) {\
	(real)->mem = (priv)->saved_##mem;	\
}

#define vmwgfx_swap(priv, real, mem) {\
	void *tmp = (priv)->saved_##mem;		\
	(priv)->saved_##mem = (real)->mem;	\
	(real)->mem = tmp;			\
}

/*
 * Functions and symbols exported to Xorg via pointers.
 */

static Bool drv_pre_init(ScrnInfoPtr pScrn, int flags);
static Bool drv_screen_init(int scrnIndex, ScreenPtr pScreen, int argc,
			    char **argv);
static Bool drv_switch_mode(int scrnIndex, DisplayModePtr mode, int flags);
static void drv_adjust_frame(int scrnIndex, int x, int y, int flags);
static Bool drv_enter_vt(int scrnIndex, int flags);
static void drv_leave_vt(int scrnIndex, int flags);
static void drv_free_screen(int scrnIndex, int flags);
static ModeStatus drv_valid_mode(int scrnIndex, DisplayModePtr mode, Bool verbose,
			         int flags);

extern void xorg_tracker_set_functions(ScrnInfoPtr scrn);
extern const OptionInfoRec * xorg_tracker_available_options(int chipid,
							   int busid);


typedef enum
{
    OPTION_SW_CURSOR,
    OPTION_2D_ACCEL,
    OPTION_DEBUG_FALLBACK,
    OPTION_THROTTLE_SWAP,
    OPTION_THROTTLE_DIRTY,
    OPTION_3D_ACCEL
} drv_option_enums;

static const OptionInfoRec drv_options[] = {
    {OPTION_SW_CURSOR, "SWcursor", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_2D_ACCEL, "2DAccel", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_DEBUG_FALLBACK, "DebugFallback", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_THROTTLE_SWAP, "SwapThrottling", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_THROTTLE_DIRTY, "DirtyThrottling", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_3D_ACCEL, "3DAccel", OPTV_BOOLEAN, {0}, FALSE},
    {-1, NULL, OPTV_NONE, {0}, FALSE}
};


/*
 * Exported Xorg driver functions to winsys
 */

const OptionInfoRec *
xorg_tracker_available_options(int chipid, int busid)
{
    return drv_options;
}

void
xorg_tracker_set_functions(ScrnInfoPtr scrn)
{
    scrn->PreInit = drv_pre_init;
    scrn->ScreenInit = drv_screen_init;
    scrn->SwitchMode = drv_switch_mode;
    scrn->FreeScreen = drv_free_screen;
    scrn->ValidMode = drv_valid_mode;
}

/*
 * Internal function definitions
 */

static Bool drv_close_screen(int scrnIndex, ScreenPtr pScreen);

/*
 * Internal functions
 */

static Bool
drv_get_rec(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate)
	return TRUE;

    pScrn->driverPrivate = xnfcalloc(1, sizeof(modesettingRec));

    return TRUE;
}

static void
drv_free_rec(ScrnInfoPtr pScrn)
{
    if (!pScrn)
	return;

    if (!pScrn->driverPrivate)
	return;

    free(pScrn->driverPrivate);

    pScrn->driverPrivate = NULL;
}

static void
drv_probe_ddc(ScrnInfoPtr pScrn, int index)
{
    ConfiguredMonitor = NULL;
}

static Bool
drv_crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    ScreenPtr pScreen = pScrn->pScreen;
    int old_width, old_height;
    PixmapPtr rootPixmap;

    if (width == pScrn->virtualX && height == pScrn->virtualY)
	return TRUE;

    if (ms->check_fb_size) {
	size_t size = width*(pScrn->bitsPerPixel / 8) * height + 1024;

	if (size > ms->max_fb_size) {
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		       "Requested framebuffer size %dx%dx%d will not fit "
		       "in display memory.\n",
		       width, height, pScrn->bitsPerPixel);
	    return FALSE;
	}
    }

    old_width = pScrn->virtualX;
    old_height = pScrn->virtualY;
    pScrn->virtualX = width;
    pScrn->virtualY = height;

    /* ms->create_front_buffer will remove the old front buffer */

    rootPixmap = pScreen->GetScreenPixmap(pScreen);
    vmwgfx_disable_scanout(pScrn);
    if (!pScreen->ModifyPixmapHeader(rootPixmap, width, height, -1, -1, -1, NULL))
	goto error_modify;

    pScrn->displayWidth = rootPixmap->devKind / (rootPixmap->drawable.bitsPerPixel / 8);

    xf86SetDesiredModes(pScrn);
    return TRUE;

    /*
     * FIXME: Try out this error recovery path and fix problems.

     */
    //error_create:
    if (!pScreen->ModifyPixmapHeader(rootPixmap, old_width, old_height, -1, -1, -1, NULL))
	FatalError("failed to resize rootPixmap error path\n");

    pScrn->displayWidth = rootPixmap->devKind /
	(rootPixmap->drawable.bitsPerPixel / 8);


error_modify:
    pScrn->virtualX = old_width;
    pScrn->virtualY = old_height;

    if (xf86SetDesiredModes(pScrn))
	return FALSE;

    FatalError("failed to setup old framebuffer\n");
    return FALSE;
}

static const xf86CrtcConfigFuncsRec crtc_config_funcs = {
    .resize = drv_crtc_resize
};

static Bool
drv_init_drm(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);

    /* deal with server regeneration */
    if (ms->fd < 0) {
	char *BusID;

	BusID = malloc(64);
	sprintf(BusID, "PCI:%d:%d:%d",
		((ms->PciInfo->domain << 8) | ms->PciInfo->bus),
		ms->PciInfo->dev, ms->PciInfo->func
	    );


	ms->fd = drmOpen("vmwgfx", BusID);
	ms->isMaster = TRUE;
	free(BusID);

	if (ms->fd >= 0) {
	    drmVersionPtr ver = drmGetVersion(ms->fd);

	    if (ver == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Could not determine DRM version.\n");
		return FALSE;
	    }

	    ms->drm_major = ver->version_major;
	    ms->drm_minor = ver->version_minor;
	    ms->drm_patch = ver->version_patchlevel;

	    drmFreeVersion(ver);
	    return TRUE;
	}

	return FALSE;
    }

    return TRUE;
}

static Bool
drv_pre_init(ScrnInfoPtr pScrn, int flags)
{
    xf86CrtcConfigPtr xf86_config;
    modesettingPtr ms;
    rgb defaultWeight = { 0, 0, 0 };
    EntityInfoPtr pEnt;
    EntPtr msEnt = NULL;
    Bool use3D;

    if (pScrn->numEntities != 1)
	return FALSE;

    pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

    if (flags & PROBE_DETECT) {
	drv_probe_ddc(pScrn, pEnt->index);
	return TRUE;
    }

    pScrn->driverPrivate = NULL;

    /* Allocate driverPrivate */
    if (!drv_get_rec(pScrn))
	return FALSE;

    ms = modesettingPTR(pScrn);
    ms->pEnt = pEnt;

    pScrn->displayWidth = 640;	       /* default it */

    if (ms->pEnt->location.type != BUS_PCI)
	return FALSE;

    ms->PciInfo = xf86GetPciInfoForEntity(ms->pEnt->index);

    /* Allocate an entity private if necessary */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	FatalError("Entity");
#if 0
	msEnt = xf86GetEntityPrivate(pScrn->entityList[0],
				     modesettingEntityIndex)->ptr;
	ms->entityPrivate = msEnt;
#else
	(void)msEnt;
#endif
    } else
	ms->entityPrivate = NULL;

    if (xf86IsEntityShared(pScrn->entityList[0])) {
	if (xf86IsPrimInitDone(pScrn->entityList[0])) {
	    /* do something */
	} else {
	    xf86SetPrimInitDone(pScrn->entityList[0]);
	}
    }

    ms->fd = -1;
    if (!drv_init_drm(pScrn))
	return FALSE;

    if (ms->drm_major != DRM_VERSION_MAJOR_REQUIRED ||
	ms->drm_minor < DRM_VERSION_MINOR_REQUIRED) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "DRM driver version is %d.%d.%d\n",
		   ms->drm_major, ms->drm_minor, ms->drm_patch);
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "But this driver needs %d.%d.x to work. Giving up.\n",
		   DRM_VERSION_MAJOR_REQUIRED,
		   DRM_VERSION_MINOR_REQUIRED);
	return FALSE;
    } else {
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		   "DRM driver version is %d.%d.%d\n",
		   ms->drm_major, ms->drm_minor, ms->drm_patch);
    }

    ms->check_fb_size = (vmwgfx_max_fb_size(ms->fd, &ms->max_fb_size) == 0);

    pScrn->monitor = pScrn->confScreen->monitor;
    pScrn->progClock = TRUE;
    pScrn->rgbBits = 8;

    if (!xf86SetDepthBpp
	(pScrn, 0, 0, 0,
	 PreferConvert24to32 | SupportConvert24to32 | Support32bppFb))
	return FALSE;

    switch (pScrn->depth) {
    case 8:
    case 15:
    case 16:
    case 24:
	break;
    default:
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Given depth (%d) is not supported by the driver\n",
		   pScrn->depth);
	return FALSE;
    }
    xf86PrintDepthBpp(pScrn);

    if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
	return FALSE;
    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;

    /* Process the options */
    xf86CollectOptions(pScrn, NULL);
    if (!(ms->Options = malloc(sizeof(drv_options))))
	return FALSE;
    memcpy(ms->Options, drv_options, sizeof(drv_options));
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, ms->Options);

    use3D = TRUE;
    ms->from_3D = xf86GetOptValBool(ms->Options, OPTION_3D_ACCEL,
				    &use3D) ?
	X_CONFIG : X_PROBED;

    ms->no3D = !use3D;

    /* Allocate an xf86CrtcConfig */
    xf86CrtcConfigInit(pScrn, &crtc_config_funcs);
    xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

    /* get max width and height */
    {
	drmModeResPtr res;
	int max_width, max_height;

	res = drmModeGetResources(ms->fd);
	max_width = res->max_width;
	max_height = res->max_height;

#if 0 /* Gallium fix */
	if (ms->screen) {
	    int max;

	    max = ms->screen->get_param(ms->screen,
					PIPE_CAP_MAX_TEXTURE_2D_LEVELS);
	    max = 1 << (max - 1);
	    max_width = max < max_width ? max : max_width;
	    max_height = max < max_height ? max : max_height;
	}
#endif

	xf86CrtcSetSizeRange(pScrn, res->min_width,
			     res->min_height, max_width, max_height);
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		   "Min width %d, Max Width %d.\n",
		   res->min_width, max_width);
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		   "Min height %d, Max Height %d.\n",
		   res->min_height, max_height);
	drmModeFreeResources(res);
    }


    if (xf86ReturnOptValBool(ms->Options, OPTION_SW_CURSOR, FALSE)) {
	ms->SWCursor = TRUE;
    }

    xorg_crtc_init(pScrn);
    xorg_output_init(pScrn);

    ms->initialization = TRUE;
    if (!xf86InitialConfiguration(pScrn, TRUE)) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes.\n");
	return FALSE;
    }
    ms->initialization = FALSE;

    /*
     * If the driver can do gamma correction, it should call xf86SetGamma() here.
     */
    {
	Gamma zeros = { 0.0, 0.0, 0.0 };

	if (!xf86SetGamma(pScrn, zeros)) {
	    return FALSE;
	}
    }

    if (pScrn->modes == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
	return FALSE;
    }

    pScrn->currentMode = pScrn->modes;

    /* Set display resolution */
    xf86SetDpi(pScrn, 0, 0);

    /* Load the required sub modules */
    if (!xf86LoadSubModule(pScrn, "fb"))
	return FALSE;

#ifdef DRI2
    if (!xf86LoadSubModule(pScrn, "dri2"))
	return FALSE;
#else
    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
	       "Driver compiled without dri2 support."
#endif

    return TRUE;
}

static Bool
vmwgfx_scanout_update(int drm_fd, int fb_id, RegionPtr dirty)
{
    unsigned num_cliprects = REGION_NUM_RECTS(dirty);
    drmModeClip *clip = alloca(num_cliprects * sizeof(drmModeClip));
    BoxPtr rect = REGION_RECTS(dirty);
    int i, ret;

    if (!num_cliprects)
	return TRUE;

    for (i = 0; i < num_cliprects; i++, rect++) {
	clip[i].x1 = rect->x1;
	clip[i].y1 = rect->y1;
	clip[i].x2 = rect->x2;
	clip[i].y2 = rect->y2;
    }

    ret = drmModeDirtyFB(drm_fd, fb_id, clip, num_cliprects);
    if (ret)
	LogMessage(X_ERROR, "%s: failed to send dirty (%i, %s)\n",
		   __func__, ret, strerror(-ret));
    return (ret == 0);
}

static Bool
vmwgfx_scanout_present(ScreenPtr pScreen, int drm_fd,
		       struct vmwgfx_saa_pixmap *vpix,
		       RegionPtr dirty)
{
    uint32_t handle;
    unsigned int dummy;

    if (!REGION_NOTEMPTY(pScreen, dirty))
	return TRUE;

    if (!vpix->hw) {
	LogMessage(X_ERROR, "No surface to present from.\n");
	return FALSE;
    }

    if (xa_surface_handle(vpix->hw, &handle, &dummy) != 0) {
	LogMessage(X_ERROR, "Could not get present surface handle.\n");
	return FALSE;
    }

    if (vmwgfx_present(drm_fd, vpix->fb_id, 0, 0, dirty, handle) != 0) {
	LogMessage(X_ERROR, "Could not get present surface handle.\n");
	return FALSE;
    }

    return TRUE;
}

void xorg_flush(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
    PixmapPtr pixmap = NULL;
    struct vmwgfx_saa_pixmap *vpix;
    int i;
    xf86CrtcPtr crtc;
    PixmapPtr *pixmaps = calloc(config->num_crtc, sizeof(*pixmaps));
    unsigned int num_scanout = 0;
    unsigned int j;

    if (!pixmaps) {
	LogMessage(X_ERROR, "Failed memory allocation during screen "
		   "update.\n");
	return;
    }

    /*
     * Get an array of pixmaps from which we scan out.
     */
    for (i=0; i<config->num_crtc; ++i) {
	crtc = config->crtc[i];
	if (crtc->enabled) {
	    pixmap = crtc_get_scanout(crtc);
	    if (pixmap) {
		unsigned int j;

		/*
		 * Remove duplicates.
		 */
		for (j=0; j<num_scanout; ++j) {
		    if (pixmap == pixmaps[j])
			break;
		}

		if (j == num_scanout)
		    pixmaps[num_scanout++] = pixmap;
	    }
	}
    }

    if (!num_scanout)
	return;

    for (j=0; j<num_scanout; ++j) {
	pixmap = pixmaps[j];
	vpix = vmwgfx_saa_pixmap(pixmap);

	if (vpix->fb_id != -1) {
	    if (vpix->pending_update) {
		(void) vmwgfx_scanout_update(ms->fd, vpix->fb_id,
					     vpix->pending_update);
		REGION_EMPTY(pScreen, vpix->pending_update);
	    }
	    if (vpix->pending_present) {
		(void) vmwgfx_scanout_present(pScreen, ms->fd, vpix,
					      vpix->pending_present);
		REGION_EMPTY(pScreen, vpix->pending_present);
	    }
	}
    }
    free(pixmaps);
}

static void drv_block_handler(int i, pointer blockData, pointer pTimeout,
                              pointer pReadmask)
{
    ScreenPtr pScreen = screenInfo.screens[i];
    modesettingPtr ms = modesettingPTR(xf86Screens[pScreen->myNum]);

    vmwgfx_swap(ms, pScreen, BlockHandler);
    pScreen->BlockHandler(i, blockData, pTimeout, pReadmask);
    vmwgfx_swap(ms, pScreen, BlockHandler);

    xorg_flush(pScreen);
}

static Bool
drv_create_screen_resources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    Bool ret;

    vmwgfx_swap(ms, pScreen, CreateScreenResources);
    ret = pScreen->CreateScreenResources(pScreen);
    vmwgfx_swap(ms, pScreen, CreateScreenResources);

    drv_adjust_frame(pScrn->scrnIndex, pScrn->frameX0, pScrn->frameY0, 0);

    return drv_enter_vt(pScreen->myNum, 1);
}

static Bool
drv_set_master(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);

    if (!ms->isMaster && drmSetMaster(ms->fd) != 0) {
	if (errno == EINVAL) {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "drmSetMaster failed: 2.6.29 or newer kernel required for "
		       "multi-server DRI\n");
	} else {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "drmSetMaster failed: %s\n", strerror(errno));
	}
	return FALSE;
    }

    ms->isMaster = TRUE;
    return TRUE;
}

/**
 * vmwgfx_use_hw_cursor_argb - wrapper around hw argb cursor check.
 *
 * screen: Pointer to the current screen metadata.
 * cursor: Pointer to the current cursor metadata.
 *
 * In addition to the default test, also check whether we might be
 * needing more than one hw cursor (which we don't support).
 */
static Bool
vmwgfx_use_hw_cursor_argb(ScreenPtr screen, CursorPtr cursor)
{
    ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86CursorInfoPtr cursor_info = xf86_config->cursor_info;
    modesettingPtr ms = modesettingPTR(pScrn);
    Bool ret;

    vmwgfx_swap(ms, cursor_info, UseHWCursorARGB);
    ret = cursor_info->UseHWCursorARGB(screen, cursor);
    vmwgfx_swap(ms, cursor_info, UseHWCursorARGB);
    if (!ret)
	return FALSE;

    /*
     * If there is a chance we might need two cursors,
     * revert to sw cursor.
     */
    return !vmwgfx_output_explicit_overlap(pScrn);
}

/**
 * vmwgfx_use_hw_cursor - wrapper around hw cursor check.
 *
 * screen: Pointer to the current screen metadata.
 * cursor: Pointer to the current cursor metadata.
 *
 * In addition to the default test, also check whether we might be
 * needing more than one hw cursor (which we don't support).
 */
static Bool
vmwgfx_use_hw_cursor(ScreenPtr screen, CursorPtr cursor)
{
    ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86CursorInfoPtr cursor_info = xf86_config->cursor_info;
    modesettingPtr ms = modesettingPTR(pScrn);
    Bool ret;

    vmwgfx_swap(ms, cursor_info, UseHWCursor);
    ret = cursor_info->UseHWCursor(screen, cursor);
    vmwgfx_swap(ms, cursor_info, UseHWCursor);
    if (!ret)
	return FALSE;

    /*
     * If there is a chance we might need two simultaneous cursors,
     * revert to sw cursor.
     */
    return !vmwgfx_output_explicit_overlap(pScrn);
}

/**
 * vmwgfx_wrap_use_hw_cursor - Wrap functions that check for hw cursor
 * support.
 *
 * pScrn: Pointer to current screen info.
 *
 * Enables the device-specific hw cursor support check functions.
 */
static void vmwgfx_wrap_use_hw_cursor(ScrnInfoPtr pScrn)
{
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86CursorInfoPtr	cursor_info = xf86_config->cursor_info;
    modesettingPtr ms = modesettingPTR(pScrn);

    vmwgfx_wrap(ms, cursor_info, UseHWCursor, vmwgfx_use_hw_cursor);
    vmwgfx_wrap(ms, cursor_info, UseHWCursorARGB, vmwgfx_use_hw_cursor_argb);
}


static void drv_load_palette(ScrnInfoPtr pScrn, int numColors,
			     int *indices, LOCO *colors, VisualPtr pVisual)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    modesettingPtr ms = modesettingPTR(pScrn);
    int index, j, i;
    int c;

    switch(pScrn->depth) {
    case 15:
	for (i = 0; i < numColors; i++) {
	    index = indices[i];
	    for (j = 0; j < 8; j++) {
		ms->lut_r[index * 8 + j] = colors[index].red << 8;
		ms->lut_g[index * 8 + j] = colors[index].green << 8;
		ms->lut_b[index * 8 + j] = colors[index].blue << 8;
	    }
	}
	break;
    case 16:
	for (i = 0; i < numColors; i++) {
	    index = indices[i];

	    if (index < 32) {
		for (j = 0; j < 8; j++) {
		    ms->lut_r[index * 8 + j] = colors[index].red << 8;
		    ms->lut_b[index * 8 + j] = colors[index].blue << 8;
		}
	    }

	    for (j = 0; j < 4; j++) {
		ms->lut_g[index * 4 + j] = colors[index].green << 8;
	    }
	}
	break;
    default:
	for (i = 0; i < numColors; i++) {
	    index = indices[i];
	    ms->lut_r[index] = colors[index].red << 8;
	    ms->lut_g[index] = colors[index].green << 8;
	    ms->lut_b[index] = colors[index].blue << 8;
	}
	break;
    }

    for (c = 0; c < xf86_config->num_crtc; c++) {
	xf86CrtcPtr crtc = xf86_config->crtc[c];

	/* Make the change through RandR */
#ifdef RANDR_12_INTERFACE
	if (crtc->randr_crtc)
	    RRCrtcGammaSet(crtc->randr_crtc, ms->lut_r, ms->lut_g, ms->lut_b);
	else
#endif
	    crtc->funcs->gamma_set(crtc, ms->lut_r, ms->lut_g, ms->lut_b, 256);
    }
}


static Bool
drv_screen_init(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    VisualPtr visual;

    if (!drv_set_master(pScrn))
	return FALSE;

    pScrn->pScreen = pScreen;

    /* HW dependent - FIXME */
    pScrn->displayWidth = pScrn->virtualX;

    miClearVisualTypes();

    if (!miSetVisualTypes(pScrn->depth,
			  miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	return FALSE;

    if (!miSetPixmapDepths())
	return FALSE;

    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;

    if (!fbScreenInit(pScreen, NULL,
		      pScrn->virtualX, pScrn->virtualY,
		      pScrn->xDpi, pScrn->yDpi,
		      pScrn->displayWidth, pScrn->bitsPerPixel))
	return FALSE;

    if (pScrn->bitsPerPixel > 8) {
	/* Fixup RGB ordering */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
	    if ((visual->class | DynamicClass) == DirectColor) {
		visual->offsetRed = pScrn->offset.red;
		visual->offsetGreen = pScrn->offset.green;
		visual->offsetBlue = pScrn->offset.blue;
		visual->redMask = pScrn->mask.red;
		visual->greenMask = pScrn->mask.green;
		visual->blueMask = pScrn->mask.blue;
	    }
	}
    }

    fbPictureInit(pScreen, NULL, 0);

    vmwgfx_wrap(ms, pScreen, BlockHandler, drv_block_handler);
    vmwgfx_wrap(ms, pScreen, CreateScreenResources,
		drv_create_screen_resources);

    xf86SetBlackWhitePixels(pScreen);

    ms->accelerate_2d = xf86ReturnOptValBool(ms->Options, OPTION_2D_ACCEL, FALSE);
    ms->debug_fallback = xf86ReturnOptValBool(ms->Options, OPTION_DEBUG_FALLBACK, ms->accelerate_2d);

    vmw_ctrl_ext_init(pScrn);

    ms->xat = xa_tracker_create(ms->fd);
    if (!ms->xat)
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "Failed to initialize Gallium3D Xa. No 3D available.\n");
    else {
	int major, minor, patch;

	xa_tracker_version(&major, &minor, &patch);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Gallium3D XA version: %d.%d.%d.\n",
		   major, minor, patch);

	if (XA_TRACKER_VERSION_MAJOR == 0) {
	    if (minor != XA_TRACKER_VERSION_MINOR) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Expecting XA version 0.%d.x.\n",
			   XA_TRACKER_VERSION_MINOR);
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "No 3D available.\n");
		xa_tracker_destroy(ms->xat);
		ms->xat = NULL;
	    }
	} else if (major != XA_TRACKER_VERSION_MAJOR ||
		   minor < XA_VERSION_MINOR_REQUIRED) {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "Expecting %d.%d.x >= XA version < %d.0.0.\n",
		       XA_TRACKER_VERSION_MAJOR,
		       XA_VERSION_MINOR_REQUIRED,
		       XA_TRACKER_VERSION_MAJOR + 1);
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "No 3D available.\n");
	    xa_tracker_destroy(ms->xat);
	    ms->xat = NULL;
	}
    }

    if (!vmwgfx_saa_init(pScreen, ms->fd, ms->xat, &xorg_flush)) {
	FatalError("Failed to initialize SAA.\n");
    }

#ifdef DRI2
    ms->dri2_available = FALSE;
    if (ms->xat) {
	ms->dri2_available = xorg_dri2_init(pScreen);
	if (!ms->dri2_available)
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		       "Failed to initialize DRI2. "
		       "No direct rendring available.\n");
    }
#endif

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "#################################\n");
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "# Useful debugging info follows #\n");
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "#################################\n");
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Using libkms backend.\n");
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "2D Acceleration is %s.\n",
	       ms->accelerate_2d ? "enabled" : "disabled");
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Fallback debugging is %s.\n",
	       ms->debug_fallback ? "enabled" : "disabled");
#ifdef DRI2
    xf86DrvMsg(pScrn->scrnIndex, ms->from_3D, "3D Acceleration is disabled.\n");
#else
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "3D Acceleration is disabled.\n");
#endif
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "##################################\n");

    miInitializeBackingStore(pScreen);
    xf86SetBackingStore(pScreen);
    xf86SetSilkenMouse(pScreen);
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    /* Need to extend HWcursor support to handle mask interleave */
    if (!ms->SWCursor) {
	xf86_cursors_init(pScreen, 64, 64,
			  HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
			  HARDWARE_CURSOR_ARGB |
			  HARDWARE_CURSOR_UPDATE_UNHIDDEN);
	vmwgfx_wrap_use_hw_cursor(pScrn);
    }

    /* Must force it before EnterVT, so we are in control of VT and
     * later memory should be bound when allocating, e.g rotate_mem */
    pScrn->vtSema = TRUE;

    pScreen->SaveScreen = xf86SaveScreen;
    vmwgfx_wrap(ms, pScreen, CloseScreen, drv_close_screen);

    if (!xf86CrtcScreenInit(pScreen))
	return FALSE;

    if (!miCreateDefColormap(pScreen))
	return FALSE;
    if (!xf86HandleColormaps(pScreen, 256, 8, drv_load_palette, NULL,
			     CMAP_PALETTED_TRUECOLOR |
			     CMAP_RELOAD_ON_MODE_SWITCH))
	return FALSE;

    xf86DPMSInit(pScreen, xf86DPMSSet, 0);

    if (serverGeneration == 1)
	xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);


    vmwgfx_wrap(ms, pScrn, EnterVT, drv_enter_vt);
    vmwgfx_wrap(ms, pScrn, LeaveVT, drv_leave_vt);
    vmwgfx_wrap(ms, pScrn, AdjustFrame, drv_adjust_frame);

    /*
     * Must be called _after_ function wrapping.
     */
    xorg_xv_init(pScreen);

    return TRUE;
}

static void
drv_adjust_frame(int scrnIndex, int x, int y, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86OutputPtr output = config->output[config->compat_output];
    xf86CrtcPtr crtc = output->crtc;

    if (crtc && crtc->enabled) {
      //	crtc->funcs->set_mode_major(crtc, pScrn->currentMode,
      //				    RR_Rotate_0, x, y);
	crtc->x = output->initial_x + x;
	crtc->y = output->initial_y + y;
    }
}

static void
drv_free_screen(int scrnIndex, int flags)
{
    drv_free_rec(xf86Screens[scrnIndex]);
}

static void
drv_leave_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    modesettingPtr ms = modesettingPTR(pScrn);

    vmwgfx_cursor_bypass(ms->fd, 0, 0);
    vmwgfx_disable_scanout(pScrn);

    if (drmDropMaster(ms->fd))
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "drmDropMaster failed: %s\n", strerror(errno));

    ms->isMaster = FALSE;
    pScrn->vtSema = FALSE;
}

/*
 * This gets called when gaining control of the VT, and from ScreenInit().
 */
static Bool
drv_enter_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    if (!drv_set_master(pScrn))
	return FALSE;

    if (!xf86SetDesiredModes(pScrn))
	return FALSE;

    return TRUE;
}

static Bool
drv_switch_mode(int scrnIndex, DisplayModePtr mode, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

static Bool
drv_close_screen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    modesettingPtr ms = modesettingPTR(pScrn);

    if (ms->cursor) {
       FreeCursor(ms->cursor, None);
       ms->cursor = NULL;
    }

#ifdef DRI2
    if (ms->dri2_available)
	xorg_dri2_close(pScreen);
#endif

    if (pScrn->vtSema)
	pScrn->LeaveVT(scrnIndex, 0);

    pScrn->vtSema = FALSE;

    vmwgfx_unwrap(ms, pScrn, EnterVT);
    vmwgfx_unwrap(ms, pScrn, LeaveVT);
    vmwgfx_unwrap(ms, pScrn, AdjustFrame);
    vmwgfx_unwrap(ms, pScreen, CloseScreen);
    vmwgfx_unwrap(ms, pScreen, BlockHandler);
    vmwgfx_unwrap(ms, pScreen, CreateScreenResources);

    if (ms->xat)
	xa_tracker_destroy(ms->xat);

    return (*pScreen->CloseScreen) (scrnIndex, pScreen);
}

static ModeStatus
drv_valid_mode(int scrnIndex, DisplayModePtr mode, Bool verbose, int flags)
{
    return MODE_OK;
}

/* vim: set sw=4 ts=8 sts=4: */
