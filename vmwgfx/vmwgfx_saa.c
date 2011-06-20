/*
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
 * Author: Thomas Hellstrom <thellstrom@vmware.com>
 */

#include <xorg-server.h>
#include <mi.h>
#include <fb.h>
#include <xf86drmMode.h>
#include <xa_context.h>
#include "vmwgfx_saa.h"
#include "vmwgfx_drmi.h"


#define VMWGFX_PIX_MALLOC  (1 << 0)
#define VMWGFX_PIX_GMR     (1 << 1)
#define VMWGFX_PIX_SURFACE (1 << 2)

struct vmwgfx_saa {
    struct saa_driver driver;
    struct vmwgfx_dma_ctx *ctx;
    struct xa_tracker *xat;
    struct xa_context *xa_ctx;
    ScreenPtr pScreen;
    int drm_fd;
    struct vmwgfx_saa_pixmap *src_vpix;
    struct vmwgfx_saa_pixmap *dst_vpix;
    Bool present_copy;
    Bool diff_valid;
    int xdiff;
    int ydiff;
    RegionRec present_region;
    uint32_t src_handle;
    Bool can_optimize_dma;
    void (*present_flush) (ScreenPtr pScreen);
    struct _WsbmListHead sync_x_list;
};

static inline struct vmwgfx_saa *
to_vmwgfx_saa(struct saa_driver *driver) {
    return (struct vmwgfx_saa *) driver;
}

static Bool
vmwgfx_pixmap_add_damage(PixmapPtr pixmap)
{
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);
    DrawablePtr draw = &pixmap->drawable;
    BoxRec box;

    if (spix->damage)
	return TRUE;

    if (!saa_add_damage(pixmap))
	return FALSE;

    box.x1 = 0;
    box.x2 = draw->width;
    box.y1 = 0;
    box.y2 = draw->height;

    if (vpix->hw) {
	REGION_INIT(draw->pScreen, &spix->dirty_hw, &box, 1);
    } else {
	REGION_INIT(draw->pScreen, &spix->dirty_shadow, &box, 1);
    }

    return TRUE;
}

static void
vmwgfx_pixmap_remove_damage(PixmapPtr pixmap)
{
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);

    if (!spix->damage || (vpix->hw && vpix->gmr))
	return;

    DamageUnregister(&pixmap->drawable, spix->damage);
    DamageDestroy(spix->damage);
    spix->damage = NULL;
}

static void
vmwgfx_pixmap_remove_present(struct vmwgfx_saa_pixmap *vpix)
{
    if (vpix->dirty_present)
	REGION_DESTROY(pixmap->drawable.pScreen, vpix->dirty_present);
    if (vpix->present_damage)
	REGION_DESTROY(pixmap->drawable.pScreen, vpix->present_damage);
    if (vpix->pending_update)
	REGION_DESTROY(pixmap->drawable.pScreen, vpix->pending_update);
    if (vpix->pending_present)
	REGION_DESTROY(pixmap->drawable.pScreen, vpix->pending_present);
    vpix->dirty_present = NULL;
    vpix->present_damage = NULL;
    vpix->pending_update = NULL;
    vpix->pending_present = NULL;
}

static Bool
vmwgfx_pixmap_add_present(PixmapPtr pixmap)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    (void) pScreen;

    vpix->dirty_present = REGION_CREATE(pScreen, NULL, 0);
    if (!vpix->dirty_present)
	return FALSE;
    vpix->present_damage = REGION_CREATE(pScreen, NULL, 0);
    if (!vpix->present_damage)
	goto out_no_present_damage;
    vpix->pending_update = REGION_CREATE(pScreen, NULL, 0);
    if (!vpix->pending_update)
	goto out_no_pending_update;
    vpix->pending_present = REGION_CREATE(pScreen, NULL, 0);
    if (!vpix->pending_present)
	goto out_no_pending_present;
    if (!vmwgfx_pixmap_add_damage(pixmap))
	goto out_no_damage;

    return TRUE;
  out_no_damage:
    REGION_DESTROY(pScreen, vpix->pending_present);
  out_no_pending_present:
    REGION_DESTROY(pScreen, vpix->pending_update);
  out_no_pending_update:
    REGION_DESTROY(pScreen, vpix->present_damage);
  out_no_present_damage:
    REGION_DESTROY(pScreen, vpix->dirty_present);
    return FALSE;
}

static void
vmwgfx_pixmap_free_storage(struct vmwgfx_saa_pixmap *vpix)
{
    if (!(vpix->backing & VMWGFX_PIX_MALLOC) && vpix->malloc) {
	free(vpix->malloc);
	vpix->malloc = NULL;
    }
    if (!(vpix->backing & VMWGFX_PIX_SURFACE) && vpix->hw) {
	xa_surface_destroy(vpix->hw);
	vpix->hw = NULL;
    }
    if (!(vpix->backing & VMWGFX_PIX_GMR) && vpix->gmr) {
	vmwgfx_dmabuf_destroy(vpix->gmr);
	vpix->gmr = NULL;
    }
}

static Bool
vmwgfx_pixmap_create_gmr(struct vmwgfx_saa *vsaa, PixmapPtr pixmap)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    size_t size;
    struct vmwgfx_dmabuf *gmr;
    void *addr;

    if (vpix->gmr)
	return TRUE;

    size = pixmap->devKind * pixmap->drawable.height;
    gmr = vmwgfx_dmabuf_alloc(vsaa->drm_fd, size);
    if (!gmr)
	return FALSE;

    if (vpix->malloc) {

	addr = vmwgfx_dmabuf_map(gmr);
	if (!addr)
	    goto out_no_transfer;
	memcpy(addr, vpix->malloc, size);
	vmwgfx_dmabuf_unmap(gmr);

    } else if (vpix->hw && !vmwgfx_pixmap_add_damage(pixmap))
	goto out_no_transfer;

    vpix->backing |= VMWGFX_PIX_GMR;
    vpix->backing &= ~VMWGFX_PIX_MALLOC;
    vpix->gmr = gmr;

    vmwgfx_pixmap_free_storage(vpix);

    return TRUE;

  out_no_transfer:
    vmwgfx_dmabuf_destroy(gmr);
    return FALSE;
}

static Bool
vmwgfx_pixmap_create_sw(struct vmwgfx_saa *vsaa, PixmapPtr pixmap)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);

    if (!(vpix->backing & (VMWGFX_PIX_MALLOC | VMWGFX_PIX_GMR)))
	return FALSE;

    if (!vpix->malloc && (vpix->backing & VMWGFX_PIX_MALLOC)) {
	vpix->malloc = malloc(pixmap->devKind * pixmap->drawable.height);
	return (vpix->malloc != NULL);
    } else if (vpix->backing & VMWGFX_PIX_GMR)
	return vmwgfx_pixmap_create_gmr(vsaa, pixmap);

    return TRUE;
}


/**
 *
 * Makes sure all presented contents covered by @region are read
 * back and are present in a valid GMR.
 */

static Bool
vmwgfx_pixmap_present_readback(struct vmwgfx_saa *vsaa,
			       PixmapPtr pixmap,
			       RegionPtr region)
{
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);
    RegionRec intersection;
    RegionRec screen_intersection;
    struct _WsbmListHead *list;

    if (!spix->damage || !REGION_NOTEMPTY(vsaa->pScreen, &spix->dirty_hw) ||
	!vpix->dirty_present)
	return TRUE;

    /*
     * Flush dirty stuff to screen.
     */


    vsaa->present_flush(vsaa->pScreen);

    /*
     * Intersect dirty region with region to be read back, if any.
     */

    REGION_NULL(vsaa->pScreen, &intersection);
    REGION_COPY(vsaa->pScreen, &intersection, &spix->dirty_hw);
    REGION_INTERSECT(vsaa->pScreen, &intersection, &intersection,
		     vpix->dirty_present);

    if (region)
	REGION_INTERSECT(vsaa->pScreen, &intersection, &intersection, region);

    if (!REGION_NOTEMPTY(vsaa->pScreen, &intersection))
	goto out;

    /*
     * Make really sure there is a GMR to read back to.
     */

    if (!vmwgfx_pixmap_create_gmr(vsaa, pixmap))
	goto out_err;

    /*
     * Readback regions are not allowed to cross screen boundaries, so
     * loop over all scanouts and make sure all readback calls are completely
     * contained within a scanout bounding box.
     */

    REGION_NULL(vsaa->pScreen, &screen_intersection);
    WSBMLISTFOREACH(list, &vpix->scanout_list) {
	struct vmwgfx_screen_box *box =
	    WSBMLISTENTRY(list, struct vmwgfx_screen_box, scanout_head);

	REGION_RESET(vsaa->pScreen, &screen_intersection, &box->box);
	REGION_INTERSECT(vsaa->pScreen, &screen_intersection,
			 &screen_intersection, &intersection);

	if (vmwgfx_present_readback(vsaa->drm_fd, &intersection) != 0)
	    goto out_readback_err;

	REGION_SUBTRACT(vsaa->pScreen, &intersection, &intersection,
			&screen_intersection);
	REGION_SUBTRACT(vsaa->pScreen, &spix->dirty_hw,
			&spix->dirty_hw, &screen_intersection);
    }

    REGION_UNINIT(vsaa->pScreen, &screen_intersection);
  out:
    REGION_UNINIT(vsaa->pScreen, &intersection);
    return TRUE;

  out_readback_err:
    REGION_UNINIT(vsaa->pScreen, &screen_intersection);
  out_err:
    REGION_UNINIT(vsaa->pScreen, &intersection);
    return FALSE;
}

static Bool
vmwgfx_saa_dma(struct vmwgfx_saa *vsaa,
	       PixmapPtr pixmap,
	       RegionPtr reg,
	       Bool to_hw)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);

    if (!vpix->hw || (!vpix->gmr && !vpix->malloc))
	return TRUE;

    if (vpix->gmr && vsaa->can_optimize_dma) {
	uint32_t handle, dummy;

	if (xa_surface_handle(vpix->hw, &handle, &dummy) != 0)
	    goto out_err;
	if (vmwgfx_dma(0, 0, reg, vpix->gmr, pixmap->devKind, handle,
		       to_hw) != 0)
	    goto out_err;
    } else {
	void *data = vpix->malloc;
	int ret;

	if (vpix->gmr) {
	    data = vmwgfx_dmabuf_map(vpix->gmr);
	    if (!data)
		goto out_err;
	}

	ret = xa_surface_dma(vsaa->xa_ctx, vpix->hw, data, pixmap->devKind,
			     (int) to_hw,
			     (struct xa_box *) REGION_RECTS(reg),
			     REGION_NUM_RECTS(reg));
	if (vpix->gmr)
	    vmwgfx_dmabuf_unmap(vpix->gmr);
	if (ret)
	    goto out_err;
    }
    return TRUE;
  out_err:
    LogMessage(X_ERROR, "DMA %s surface failed.\n",
	       to_hw ? "to" : "from");
    return FALSE;
}


static Bool
vmwgfx_download_from_hw(struct saa_driver *driver, PixmapPtr pixmap,
			RegionPtr readback)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);

    RegionRec intersection;

    if (!vmwgfx_pixmap_present_readback(vsaa, pixmap, readback))
	return FALSE;

    if (!REGION_NOTEMPTY(vsaa->pScreen, &spix->dirty_hw))
	return TRUE;

    if (!vpix->hw)
	return TRUE;

    REGION_NULL(vsaa->pScreen, &intersection);
    REGION_INTERSECT(vsaa->pScreen, &intersection, readback,
		     &spix->dirty_hw);
    readback = &intersection;

    if (!vmwgfx_pixmap_create_sw(vsaa, pixmap))
	goto out_err;

    if (!vmwgfx_saa_dma(vsaa, pixmap, readback, FALSE))
	goto out_err;
    REGION_SUBTRACT(vsaa->pScreen, &spix->dirty_hw, &spix->dirty_hw, readback);
    REGION_UNINIT(vsaa->pScreen, &intersection);
    return TRUE;
 out_err:
    REGION_UNINIT(vsaa->pScreen, &intersection);
    return FALSE;
}


static Bool
vmwgfx_upload_to_hw(struct saa_driver *driver, PixmapPtr pixmap,
		    RegionPtr upload)
{
    return vmwgfx_saa_dma(to_vmwgfx_saa(driver), pixmap, upload, TRUE);
}

static void
vmwgfx_release_from_cpu(struct saa_driver *driver, PixmapPtr pixmap, saa_access_t access)
{
  //    LogMessage(X_INFO, "Release 0x%08lx access 0x%08x\n",
  //	       (unsigned long) pixmap, (unsigned) access);
}

static void *
vmwgfx_sync_for_cpu(struct saa_driver *driver, PixmapPtr pixmap, saa_access_t access)
{
    /*
     * Errors in this functions will turn up in subsequent map
     * calls.
     */

    (void) vmwgfx_pixmap_create_sw(to_vmwgfx_saa(driver), pixmap);

    return NULL;
}

static void *
vmwgfx_map(struct saa_driver *driver, PixmapPtr pixmap, saa_access_t access)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);

    if (vpix->malloc)
	return vpix->malloc;
    else if (vpix->gmr)
	return vmwgfx_dmabuf_map(vpix->gmr);
    else
	return NULL;
}

static void
vmwgfx_unmap(struct saa_driver *driver, PixmapPtr pixmap, saa_access_t access)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);

    if (vpix->gmr)
	return vmwgfx_dmabuf_unmap(vpix->gmr);

//    LogMessage(X_INFO, "Unmap 0x%08lx access 0x%08x\n",
    //       (unsigned long) pixmap, (unsigned) access);
    ;
}

static Bool
vmwgfx_create_pixmap(struct saa_driver *driver, struct saa_pixmap *spix,
		     int w, int h, int depth,
		     unsigned int usage_hint, int bpp, int *new_pitch)
{
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);

    *new_pitch = ((w * bpp + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);

    WSBMINITLISTHEAD(&vpix->sync_x_head);
    WSBMINITLISTHEAD(&vpix->scanout_list);

    return TRUE;
}

void
vmwgfx_flush_dri2(ScreenPtr pScreen)
{
    struct vmwgfx_saa *vsaa =
	to_vmwgfx_saa(saa_get_driver(pScreen));
    struct _WsbmListHead *list, *next;

    WSBMLISTFOREACHSAFE(list, next, &vsaa->sync_x_list) {
	struct vmwgfx_saa_pixmap *vpix =
	    WSBMLISTENTRY(list, struct vmwgfx_saa_pixmap, sync_x_head);
	struct saa_pixmap *spix = &vpix->base;
	PixmapPtr pixmap = spix->pixmap;

	if (vmwgfx_upload_to_hw(&vsaa->driver, pixmap, &spix->dirty_shadow)) {
	    REGION_EMPTY(vsaa->pScreen, &spix->dirty_shadow);
	    WSBMLISTDELINIT(list);
	}
    }
}


static void
vmwgfx_destroy_pixmap(struct saa_driver *driver, PixmapPtr pixmap)
{
    ScreenPtr pScreen = to_vmwgfx_saa(driver)->pScreen;
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    (void) pScreen;

    vpix->backing = 0;
    vmwgfx_pixmap_free_storage(vpix);

    /*
     * Any damage we've registered has already been removed by the server
     * at this point. Any attempt to unregister / destroy it will result
     * in a double free.
     */

    vmwgfx_pixmap_remove_present(vpix);
    WSBMLISTDELINIT(&vpix->sync_x_head);

    if (vpix->hw_is_dri2_fronts)
	LogMessage(X_ERROR, "Incorrect dri2 front count.\n");
}

static Bool
vmwgfx_pixmap_create_hw(struct vmwgfx_saa *vsaa,
			PixmapPtr pixmap, unsigned int flags)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    struct xa_surface *hw;

    if (!vsaa->xat)
	return FALSE;

    if (vpix->hw)
	return TRUE;

    hw = xa_surface_create(vsaa->xat,
			   pixmap->drawable.width,
			   pixmap->drawable.height,
			   pixmap->drawable.depth,
			   xa_type_argb, xa_format_unknown,
			   XA_FLAG_RENDER_TARGET | flags);
    if (hw == NULL)
	return FALSE;

    if ((vpix->gmr || vpix->malloc) && !vmwgfx_pixmap_add_damage(pixmap))
	goto out_no_damage;

    /*
     * Even if we don't have a GMR yet, indicate that when needed it
     * should be created.
     */

    vpix->hw = hw;
    vpix->backing |= VMWGFX_PIX_SURFACE;
    vmwgfx_pixmap_free_storage(vpix);

    return TRUE;

out_no_damage:
    xa_surface_destroy(hw);
    return FALSE;
}


/**
 *
 * Makes sure we have a surface with valid contents.
 */

Bool
vmwgfx_pixmap_validate_hw(PixmapPtr pixmap, RegionPtr region,
			  unsigned int add_flags,
			  unsigned int remove_flags)
{
    struct vmwgfx_saa *vsaa =
	to_vmwgfx_saa(saa_get_driver(pixmap->drawable.pScreen));
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);
    RegionRec intersection;

    if (!vsaa->xat)
	return FALSE;

    if (vpix->hw) {
	if (xa_surface_redefine(vpix->hw,
				pixmap->drawable.width,
				pixmap->drawable.height,
				pixmap->drawable.depth,
				xa_type_argb, xa_format_unknown,
				XA_FLAG_RENDER_TARGET | add_flags,
				remove_flags, 1) != 0)
	    return FALSE;
    } else if (!vmwgfx_pixmap_create_hw(vsaa, pixmap, add_flags))
	return FALSE;


    if (!vmwgfx_pixmap_present_readback(vsaa, pixmap, region))
	return FALSE;

    REGION_NULL(vsaa->pScreen, &intersection);
    REGION_COPY(vsaa->pScreen, &intersection, &spix->dirty_shadow);

    if (vpix->dirty_present)
	REGION_UNION(vsaa->pScreen, &intersection, vpix->dirty_present,
		     &spix->dirty_shadow);

    if (spix->damage && REGION_NOTEMPTY(vsaa->pScreen, &intersection)) {
	RegionPtr upload = &intersection;

	/*
	 * Check whether we need to upload from GMR.
	 */

	if (region) {
	    REGION_INTERSECT(vsaa->pScreen, &intersection, region,
			     &intersection);
	    upload = &intersection;
	}

	if (REGION_NOTEMPTY(vsaa->pScreen, upload)) {
	    Bool ret = vmwgfx_upload_to_hw(&vsaa->driver, pixmap, upload);
	    if (ret) {
		REGION_SUBTRACT(vsaa->pScreen, &spix->dirty_shadow,
				&spix->dirty_shadow, upload);
		if (vpix->dirty_present)
		    REGION_SUBTRACT(vsaa->pScreen, vpix->dirty_present,
				    vpix->dirty_present, upload);
	    } else {
		REGION_UNINIT(vsaa->pScreen, &intersection);
		return FALSE;
	    }
	}
    }
    REGION_UNINIT(vsaa->pScreen, &intersection);
    return TRUE;
}

static void
vmwgfx_copy_stride(uint8_t *dst, uint8_t *src, unsigned int dst_pitch,
		   unsigned int src_pitch, unsigned int dst_height,
		   unsigned int src_height)
{
    unsigned int i;
    unsigned int height = (dst_height < src_height) ? dst_height : src_height;
    unsigned int pitch = (dst_pitch < src_pitch) ? dst_pitch : src_pitch;

    for(i=0; i<height; ++i) {
	memcpy(dst, src, pitch);
	dst += dst_pitch;
	src += src_pitch;
    }
}


static Bool
vmwgfx_pix_resize(PixmapPtr pixmap, unsigned int old_pitch,
		  unsigned int old_height, unsigned int old_width)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(saa_get_driver(pScreen));
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    DrawablePtr draw = &pixmap->drawable;
    unsigned int size = pixmap->devKind * draw->height;

    /*
     * Ignore copying errors. At worst they will show up as rendering
     * artefacts.
     */

    if (vpix->malloc) {

	void *new_malloc = malloc(size);
	if (!new_malloc)
	    return FALSE;

	vmwgfx_copy_stride(new_malloc, vpix->malloc, pixmap->devKind,
			   old_pitch, draw->height,
			   old_height);
	free(vpix->malloc);
	vpix->malloc = new_malloc;
    }

    if (vpix->gmr) {
	struct vmwgfx_dmabuf *gmr;
	void *new_addr;
	void *old_addr;

	gmr = vmwgfx_dmabuf_alloc(vsaa->drm_fd, size);
	if (!gmr)
	    return FALSE;

	new_addr = vmwgfx_dmabuf_map(gmr);
	old_addr = vmwgfx_dmabuf_map(vpix->gmr);

	if (new_addr && old_addr)
	    vmwgfx_copy_stride(new_addr, old_addr, pixmap->devKind,
			       old_pitch, draw->height,
			       old_height);
	else
	    LogMessage(X_ERROR, "Failed pixmap resize copy.\n");

	if (old_addr)
	    vmwgfx_dmabuf_unmap(vpix->gmr);
	if (new_addr)
	    vmwgfx_dmabuf_unmap(gmr);
	vmwgfx_dmabuf_destroy(vpix->gmr);
	vpix->gmr = gmr;
    }

    if (vpix->hw) {
	if (xa_surface_redefine(vpix->hw, draw->width, draw->height,
				draw->depth, xa_type_argb,
				xa_format_unknown, 0, 0, 1) != 0)
	    return FALSE;
    }
    return TRUE;
}


static Bool
vmwgfx_modify_pixmap_header (PixmapPtr pixmap, int w, int h, int depth,
			     int bpp, int devkind, void *pixdata)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    unsigned int old_height;
    unsigned int old_width;
    unsigned int old_pitch;

    if (!vpix) {
	LogMessage(X_ERROR, "Not an SAA pixmap.\n");
	return FALSE;
    }

    if (pixdata) {
	vpix->backing = 0;
	vmwgfx_pixmap_free_storage(vpix);
	return FALSE;
    }

    if (depth <= 0)
	depth = pixmap->drawable.depth;

    if (bpp <= 0)
	bpp = pixmap->drawable.bitsPerPixel;

    if (w <= 0)
	w = pixmap->drawable.width;

    if (h <= 0)
	h = pixmap->drawable.height;

    if (w <= 0 || h <= 0 || depth <= 0)
	return FALSE;

    old_height = pixmap->drawable.height;
    old_width = pixmap->drawable.width;
    old_pitch = pixmap->devKind;

    if (!miModifyPixmapHeader(pixmap, w, h, depth,
			      bpp, devkind, NULL))
	goto out_no_modify;

    if (!vpix->backing)
	vpix->backing = VMWGFX_PIX_MALLOC;

    vmwgfx_pix_resize(pixmap, old_pitch, old_height, old_width);
    vmwgfx_pixmap_add_damage(pixmap);
    vmwgfx_pixmap_free_storage(vpix);
    return TRUE;

  out_no_modify:
    return FALSE;
}

static Bool
vmwgfx_present_prepare(struct vmwgfx_saa *vsaa,
		       struct vmwgfx_saa_pixmap *src_vpix,
		       struct vmwgfx_saa_pixmap *dst_vpix)
{
    ScreenPtr pScreen = vsaa->pScreen;
    unsigned int dummy;

    (void) pScreen;
    if (src_vpix == dst_vpix || !src_vpix->hw ||
	xa_surface_handle(src_vpix->hw, &vsaa->src_handle, &dummy) != 0)
	return FALSE;

    REGION_NULL(pScreen, &vsaa->present_region);
    vsaa->diff_valid = FALSE;
    vsaa->dst_vpix = dst_vpix;
    vsaa->present_flush(pScreen);

    return TRUE;
}

/**
 * Determine whether we should try present copies on this pixmap.
 */

static Bool
vmwgfx_is_present_hw(PixmapPtr pixmap)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    return (vpix->dirty_present != NULL);
}

static void
vmwgfx_check_hw_contents(struct vmwgfx_saa *vsaa,
			 struct vmwgfx_saa_pixmap *vpix,
			 RegionPtr region,
			 Bool *has_dirty_hw,
			 Bool *has_valid_hw)
{
    RegionRec intersection;


    if (!vpix->hw) {
	*has_dirty_hw = FALSE;
	*has_valid_hw = FALSE;
	return;
    }

    if (!region) {
	*has_dirty_hw = REGION_NOTEMPTY(vsaa->pScreen,
					&vpix->base.dirty_hw);
	*has_valid_hw = !REGION_NOTEMPTY(vsaa->pScreen,
					 &vpix->base.dirty_shadow);
	return;
    }

    REGION_NULL(vsaa->pScreen, &intersection);
    REGION_INTERSECT(vsaa->pScreen, &intersection, &vpix->base.dirty_hw,
		     region);
    *has_dirty_hw = REGION_NOTEMPTY(vsaa->pScreen, &intersection);
    REGION_INTERSECT(vsaa->pScreen, &intersection, &vpix->base.dirty_shadow,
		     region);
    *has_valid_hw = !REGION_NOTEMPTY(vsaa->pScreen, &intersection);
    REGION_UNINIT(vsaa->pScreen, &intersection);
}

static Bool
vmwgfx_copy_prepare(struct saa_driver *driver,
		    PixmapPtr src_pixmap,
		    PixmapPtr dst_pixmap,
		    int dx,
		    int dy,
		    int alu,
		    RegionPtr src_reg,
		    uint32_t plane_mask)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);
    struct vmwgfx_saa_pixmap *src_vpix;
    struct vmwgfx_saa_pixmap *dst_vpix;
    Bool has_dirty_hw;
    Bool has_valid_hw;

    if (!vsaa->xat || !SAA_PM_IS_SOLID(&dst_pixmap->drawable, plane_mask) ||
	alu != GXcopy)
	return FALSE;

    src_vpix = vmwgfx_saa_pixmap(src_pixmap);
    dst_vpix = vmwgfx_saa_pixmap(dst_pixmap);

    vmwgfx_check_hw_contents(vsaa, src_vpix, src_reg,
			     &has_dirty_hw, &has_valid_hw);

    if (vmwgfx_is_present_hw(dst_pixmap) &&
	src_vpix->backing & VMWGFX_PIX_SURFACE) {

	if (!has_dirty_hw && !has_valid_hw)
	    return FALSE;

	if (vmwgfx_present_prepare(vsaa, src_vpix, dst_vpix)) {
	    if (!vmwgfx_pixmap_validate_hw(src_pixmap, src_reg, 0, 0))
		return FALSE;
	    vsaa->present_copy = TRUE;
	    return TRUE;
	}
	return FALSE;
    }

    vsaa->present_copy = FALSE;
    if (src_vpix->hw != NULL && src_vpix != dst_vpix) {

	/*
	 * Use hardware acceleration either if source is partially only
	 * in hardware, or if source is entirely in hardware and destination
	 * has a hardware surface.
	 */

	if (!has_dirty_hw && !(has_valid_hw && (dst_vpix->hw != NULL)))
	    return FALSE;
	if (!vmwgfx_pixmap_validate_hw(src_pixmap, src_reg, 0, 0))
	    return FALSE;
	if (!vmwgfx_pixmap_create_hw(vsaa, dst_pixmap, XA_FLAG_RENDER_TARGET))
	    return FALSE;

	if (xa_copy_prepare(vsaa->xa_ctx, dst_vpix->hw, src_vpix->hw) == 0)
	    return TRUE;
    }

    return FALSE;
}


static void
vmwgfx_present_done(struct vmwgfx_saa *vsaa)
{
    ScreenPtr pScreen = vsaa->pScreen;
    struct vmwgfx_saa_pixmap *dst_vpix = vsaa->dst_vpix;

    (void) pScreen;
    if (!vsaa->diff_valid)
	return;

    (void) vmwgfx_present(vsaa->drm_fd, vsaa->xdiff, vsaa->ydiff,
			  &vsaa->present_region, vsaa->src_handle);

    REGION_TRANSLATE(pScreen, &vsaa->present_region, vsaa->xdiff, vsaa->ydiff);
    REGION_UNION(pScreen, dst_vpix->present_damage, dst_vpix->present_damage,
		 &vsaa->present_region);
    vsaa->diff_valid = FALSE;
    REGION_UNINIT(pScreen, &vsaa->present_region);
}

static void
vmwgfx_present_copy(struct vmwgfx_saa *vsaa,
		    int src_x,
		    int src_y,
		    int dst_x,
		    int dst_y,
		    int w,
		    int h)
{
    int xdiff = dst_x - src_x;
    int ydiff = dst_y - src_y;
    BoxRec box;
    RegionRec reg;

    if (vsaa->diff_valid && ((xdiff != vsaa->xdiff) || (ydiff != vsaa->ydiff)))
	(void) vmwgfx_present_done(vsaa);

    if (!vsaa->diff_valid) {
	vsaa->xdiff = xdiff;
	vsaa->ydiff = ydiff;
	vsaa->diff_valid = TRUE;
    }

    box.x1 = src_x;
    box.x2 = src_x + w;
    box.y1 = src_y;
    box.y2 = src_y + h;

    REGION_INIT(pScreen, &reg, &box, 1);
    REGION_UNION(pScreen, &vsaa->present_region, &vsaa->present_region, &reg);
    REGION_UNINIT(pScreen, &reg);
}

static void
vmwgfx_copy(struct saa_driver *driver,
	    int src_x,
	    int src_y,
	    int dst_x,
	    int dst_y,
	    int w,
	    int h)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);

    if (vsaa->present_copy) {
	vmwgfx_present_copy(vsaa, src_x, src_y, dst_x, dst_y, w, h);
	return;
    }
    xa_copy(vsaa->xa_ctx, dst_x, dst_y, src_x, src_y, w, h);
}

static void
vmwgfx_copy_done(struct saa_driver *driver)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);

    if (vsaa->present_copy) {
	vmwgfx_present_done(vsaa);
	return;
    }
    xa_copy_done(vsaa->xa_ctx);
}

static void
vmwgfx_takedown(struct saa_driver *driver)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);

    free(vsaa);
}

static void
vmwgfx_operation_complete(struct saa_driver *driver,
			  PixmapPtr pixmap)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);

    /*
     * Make dri2 drawables up to date, or add them to the flush list
     * executed at glxWaitX().
     */

    if (vpix->hw && vpix->hw_is_dri2_fronts) {
	if (1) {
	    if (!vmwgfx_upload_to_hw(driver, pixmap, &spix->dirty_shadow))
		return;
	    REGION_EMPTY(vsaa->pScreen, &spix->dirty_shadow);
	} else {
	    if (WSBMLISTEMPTY(&vpix->sync_x_head))
		WSBMLISTADDTAIL(&vpix->sync_x_head, &vsaa->sync_x_list);
	}
    }
}


static Bool
vmwgfx_dirty(struct saa_driver *driver, PixmapPtr pixmap,
	     Bool hw, RegionPtr damage)
{
    struct vmwgfx_saa *vsaa = to_vmwgfx_saa(driver);
    struct saa_pixmap *spix = saa_get_saa_pixmap(pixmap);
    struct vmwgfx_saa_pixmap *vpix = to_vmwgfx_saa_pixmap(spix);
    BoxPtr rects;
    int num_rects;

    if (!vmwgfx_is_present_hw(pixmap))
	return TRUE;

    rects = REGION_RECTS(damage);
    num_rects = REGION_NUM_RECTS(damage);

    /*
     * Is the new scanout damage hw or sw?
     */

    if (hw) {
	/*
	 * Dump pending present into present tracking region.
	 */
	if (REGION_NOTEMPTY(vsaa->pScreen, vpix->present_damage)) {
	    REGION_UNION(vsaa->pScreen, vpix->dirty_present,
			 vpix->dirty_present, damage);
	    REGION_EMPTY(vsaa->pScreen, vpix->present_damage);
	} else {
	    if (REGION_NOTEMPTY(vsaa->pScreen, vpix->pending_update)) {
		RegionRec reg;

		REGION_NULL(vsaa->pScreen, &reg);
		REGION_INTERSECT(vsaa->pScreen, &reg, vpix->pending_update,
				 damage);
		if (REGION_NOTEMPTY(vsaa->pScreen, &reg))
		    vsaa->present_flush(vsaa->pScreen);
		REGION_UNINIT(pScreen, &reg);
	    }
	    REGION_UNION(vsaa->pScreen, vpix->pending_present,
			 vpix->pending_present, damage);
	    REGION_SUBTRACT(vsaa->pScreen, vpix->dirty_present,
			    vpix->dirty_present, damage);
	}
    } else {
	    if (REGION_NOTEMPTY(vsaa->pScreen, vpix->pending_present)) {
		RegionRec reg;

		REGION_NULL(vsaa->pScreen, &reg);
		REGION_INTERSECT(vsaa->pScreen, &reg, vpix->pending_present,
				 damage);
		if (REGION_NOTEMPTY(vsaa->pScreen, &reg))
		    vsaa->present_flush(vsaa->pScreen);
		REGION_UNINIT(pScreen, &reg);
	    }
	    REGION_UNION(vsaa->pScreen, vpix->pending_update,
			 vpix->pending_update, damage);
	    REGION_SUBTRACT(vsaa->pScreen, vpix->dirty_present,
			    vpix->dirty_present, damage);
    }

    return TRUE;
}


static const struct saa_driver vmwgfx_saa_driver = {
    .saa_major = SAA_VERSION_MAJOR,
    .saa_minor = SAA_VERSION_MINOR,
    .pixmap_size = sizeof(struct vmwgfx_saa_pixmap),
    .damage = vmwgfx_dirty,
    .operation_complete = vmwgfx_operation_complete,
    .download_from_hw = vmwgfx_download_from_hw,
    .release_from_cpu = vmwgfx_release_from_cpu,
    .sync_for_cpu = vmwgfx_sync_for_cpu,
    .map = vmwgfx_map,
    .unmap = vmwgfx_unmap,
    .create_pixmap = vmwgfx_create_pixmap,
    .destroy_pixmap = vmwgfx_destroy_pixmap,
    .modify_pixmap_header = vmwgfx_modify_pixmap_header,
    .copy_prepare = vmwgfx_copy_prepare,
    .copy = vmwgfx_copy,
    .copy_done = vmwgfx_copy_done,
    .takedown = vmwgfx_takedown,
};


Bool
vmwgfx_saa_init(ScreenPtr pScreen, int drm_fd, struct xa_tracker *xat,
		void (*present_flush)(ScreenPtr pScreen))
{
    struct vmwgfx_saa *vsaa;

    vsaa = calloc(1, sizeof(*vsaa));
    if (!vsaa)
	return FALSE;

    vsaa->pScreen = pScreen;
    vsaa->xat = xat;
    if (xat)
	vsaa->xa_ctx = xa_context_default(xat);
    vsaa->drm_fd = drm_fd;
    vsaa->present_flush = present_flush;
    vsaa->can_optimize_dma = FALSE;
    WSBMINITLISTHEAD(&vsaa->sync_x_list);

    vsaa->driver = vmwgfx_saa_driver;
    if (!saa_driver_init(pScreen, &vsaa->driver))
	goto out_no_saa;

    return TRUE;
  out_no_saa:
    free(vsaa);
    return FALSE;
}

/*
 * *************************************************************************
 * Scanout functions.
 * These do not strictly belong here, but we choose to hide the scanout
 * pixmap private data in the saa pixmaps. Might want to revisit this.
 */

/*
 * Make sure we flush / update this scanout on next update run.
 */

void
vmwgfx_scanout_refresh(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    BoxRec box;

    (void) pScreen;
    box.x1 = 0;
    box.y1 = 0;
    box.x2 = pixmap->drawable.width;
    box.y2 = pixmap->drawable.height;

    REGION_RESET(vsaa->pScreen, vpix->pending_update, &box);
    REGION_SUBTRACT(vsaa->pScreen, vpix->pending_present,
		    &vpix->base.dirty_hw, vpix->dirty_present);
    REGION_SUBTRACT(vsaa->pScreen, vpix->pending_update,
		    vpix->pending_update, &vpix->base.dirty_hw);
}

/*
 * Take a "scanout reference" on a pixmap. If this is the first scanout
 * reference, allocate resources needed for scanout, like proper
 * damage tracking and kms fbs.
 */

uint32_t
vmwgfx_scanout_ref(struct vmwgfx_screen_box  *box)
{
    PixmapPtr pixmap = box->pixmap;
    struct vmwgfx_saa *vsaa =
	to_vmwgfx_saa(saa_get_driver(pixmap->drawable.pScreen));
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pixmap);
    int ret;

    if (WSBMLISTEMPTY(&vpix->scanout_list)) {
	ret = !vmwgfx_pixmap_create_gmr(vsaa, pixmap);
	if (!ret)
	    ret = !vmwgfx_pixmap_add_present(pixmap);
	if (!ret)
	    ret = drmModeAddFB(vsaa->drm_fd,
			       pixmap->drawable.width,
			       pixmap->drawable.height,
			       pixmap->drawable.depth,
			       pixmap->drawable.bitsPerPixel,
			       pixmap->devKind,
			       vpix->gmr->handle,
			       &vpix->fb_id);
	if (ret) {
	    box->pixmap = NULL;
	    vpix->fb_id = -1;
	    goto out_err;
	}

    }
    pixmap->refcnt += 1;
    WSBMLISTADDTAIL(&box->scanout_head, &vpix->scanout_list);

  out_err:
    return vpix->fb_id;
}

/*
 * Free a "scanout reference" on a pixmap. If this was the last scanout
 * reference, free pixmap resources needed for scanout, like
 * damage tracking and kms fbs.
 */
void
vmwgfx_scanout_unref(struct vmwgfx_screen_box *box)
{
    struct vmwgfx_saa *vsaa;
    struct vmwgfx_saa_pixmap *vpix;
    PixmapPtr pixmap = box->pixmap;

    if (!pixmap)
	return;

    vsaa = to_vmwgfx_saa(saa_get_driver(pixmap->drawable.pScreen));
    vpix = vmwgfx_saa_pixmap(pixmap);
    WSBMLISTDELINIT(&box->scanout_head);

    if (WSBMLISTEMPTY(&vpix->scanout_list)) {
	REGION_EMPTY(vsaa->pScreen, vpix->pending_update);
	drmModeRmFB(vsaa->drm_fd, vpix->fb_id);
	vpix->fb_id = -1;
	vmwgfx_pixmap_present_readback(vsaa, pixmap, NULL);
	vmwgfx_pixmap_remove_present(vpix);
	vmwgfx_pixmap_remove_damage(pixmap);
    }

    box->pixmap = NULL;
    pixmap->drawable.pScreen->DestroyPixmap(pixmap);
}
