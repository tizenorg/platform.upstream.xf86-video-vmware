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
 * The above copyright n<otice and this permission notice (including the
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

#ifndef _VMWGFX_DRIVER_H_
#define _VMWGFX_DRIVER_H_


#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <xorg-server.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86xv.h>
#include <xa_tracker.h>

#define DRV_ERROR(msg)	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, msg);
#define debug_printf(...)

typedef struct
{
    int lastInstance;
    int refCount;
    ScrnInfoPtr pScrn_1;
    ScrnInfoPtr pScrn_2;
} EntRec, *EntPtr;

#define XORG_NR_FENCES 3

enum xorg_throttling_reason {
    THROTTLE_RENDER,
    THROTTLE_SWAP
};

typedef struct _modesettingRec
{
    /* drm */
    int fd;

    /* X */
    EntPtr entityPrivate;

    int Chipset;
    EntityInfoPtr pEnt;
    struct pci_device *PciInfo;

    /* Accel */
    Bool accelerate_2d;
    Bool debug_fallback;

    Bool noAccel;
    Bool SWCursor;
    CursorPtr cursor;
    Bool no3D;
    Bool from_3D;
    Bool isMaster;

    /* Broken-out options. */
    OptionInfoPtr Options;

    ScreenBlockHandlerProcPtr saved_BlockHandler;
    CreateScreenResourcesProcPtr saved_CreateScreenResources;
    CloseScreenProcPtr saved_CloseScreen;
    Bool (*saved_EnterVT)(int, int);
    void (*saved_LeaveVT)(int, int);
    void (*saved_AdjustFrame)(int, int, int, int);

    uint16_t lut_r[256], lut_g[256], lut_b[256];

    Bool check_fb_size;
    size_t max_fb_size;

    struct xa_tracker *xat;
#ifdef DRI2
    Bool dri2_available;
#endif
} modesettingRec, *modesettingPtr;

#define modesettingPTR(p) ((modesettingPtr)((p)->driverPrivate))

void xorg_flush(ScreenPtr pScreen);
/***********************************************************************
 * xorg_dri2.c
 */
Bool
xorg_dri2_init(ScreenPtr pScreen);

void
xorg_dri2_close(ScreenPtr pScreen);


/***********************************************************************
 * xorg_crtc.c
 */
void
xorg_crtc_init(ScrnInfoPtr pScrn);

void
xorg_crtc_cursor_destroy(xf86CrtcPtr crtc);

void
vmwgfx_disable_scanout(ScrnInfoPtr pScrn);

PixmapPtr
crtc_get_scanout(xf86CrtcPtr crtc);


/***********************************************************************
 * xorg_output.c
 */
void
xorg_output_init(ScrnInfoPtr pScrn);

unsigned
xorg_output_get_id(xf86OutputPtr output);


/***********************************************************************
 * xorg_xv.c
 */
void
xorg_xv_init(ScreenPtr pScreen);

XF86VideoAdaptorPtr
vmw_video_init_adaptor(ScrnInfoPtr pScrn);
void
vmw_video_free_adaptor(XF86VideoAdaptorPtr adaptor, Bool free_ports);

int
vmwgfx_present(int drm_fd, unsigned int dst_x, unsigned int dst_y,
	       RegionPtr region, uint32_t handle);

void
vmw_ctrl_ext_init(ScrnInfoPtr pScrn);

#endif /* _XORG_TRACKER_H_ */
