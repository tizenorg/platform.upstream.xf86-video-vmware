/*
 * Copyright 2009-2011 VMWare, Inc.
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
 * Author: Zack Rusin <zackr@vmware.com>
 */

#include "vmwgfx_driver.h"
#include "vmwgfx_drmi.h"
#include "vmwgfx_saa.h"

#include <xf86xv.h>
#include <X11/extensions/Xv.h>
#include <fourcc.h>
#include <xa_tracker.h>
#include <xa_context.h>


/*XXX get these from pipe's texture limits */
#define IMAGE_MAX_WIDTH		2048
#define IMAGE_MAX_HEIGHT	2048

#define RES_720P_X 1280
#define RES_720P_Y 720


/* The ITU-R BT.601 conversion matrix for SDTV. */
/* original, matrix, but we transpose it to
 * make the shader easier
static const float bt_601[] = {
    1.0, 0.0, 1.4075,   ,
    1.0, -0.3455, -0.7169, 0,
    1.0, 1.7790, 0., 0,
};*/
static const float bt_601[] = {
    1.0, 1.0, 1.0,        0.5,
    0.0, -0.3455, 1.7790, 0,
    1.4075, -0.7169, 0.,  0,
};

/* The ITU-R BT.709 conversion matrix for HDTV. */
/* original, but we transpose to make the conversion
 * in the shader easier
static const float bt_709[] = {
    1.0, 0.0, 1.581, 0,
    1.0, -0.1881, -0.47, 0,
    1.0, 1.8629, 0., 0,
};*/
static const float bt_709[] = {
    1.0,   1.0,     1.0,     0.5,
    0.0,  -0.1881,  1.8629,  0,
    1.581,-0.47   , 0.0,     0,
};

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)

static Atom xvBrightness, xvContrast;

#define NUM_TEXTURED_ATTRIBUTES 2
static XF86AttributeRec TexturedAttributes[NUM_TEXTURED_ATTRIBUTES] = {
   {XvSettable | XvGettable, -128, 127, "XV_BRIGHTNESS"},
   {XvSettable | XvGettable, 0, 255, "XV_CONTRAST"}
};

#define NUM_FORMATS 3
static XF86VideoFormatRec Formats[NUM_FORMATS] = {
   {15, TrueColor}, {16, TrueColor}, {24, TrueColor}
};

static XF86VideoEncodingRec DummyEncoding[1] = {
   {
      0,
      "XV_IMAGE",
      IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT,
      {1, 1}
   }
};

#define NUM_IMAGES 3
static XF86ImageRec Images[NUM_IMAGES] = {
   XVIMAGE_UYVY,
   XVIMAGE_YUY2,
   XVIMAGE_YV12,
};

struct xorg_xv_port_priv {
    struct xa_tracker *xat;
    struct xa_context *r;
    struct xa_fence *fence;

    RegionRec clip;

    int brightness;
    int contrast;

    int current_set;
    struct vmwgfx_dmabuf *bounce[2][3];
    struct xa_surface *yuv[3];

    int drm_fd;
};


static void
stop_video(ScrnInfoPtr pScrn, pointer data, Bool shutdown)
{
   struct xorg_xv_port_priv *priv = (struct xorg_xv_port_priv *)data;
   int i, j;

   REGION_EMPTY(pScrn->pScreen, &priv->clip);
   if (shutdown) {

       /*
	* No need to destroy the xa context or xa tracker since
	* they are copied from the screen resources.
	*/

       xa_fence_destroy(priv->fence);
       priv->fence = NULL;

       for (i=0; i<3; ++i) {
	   if (priv->yuv[i]) {
	       xa_surface_destroy(priv->yuv[i]);
	       priv->yuv[i] = NULL;
	   }
	   for (j=0; j<2; ++j) {
	       if (priv->bounce[j][i]) {
		   vmwgfx_dmabuf_destroy(priv->bounce[j][i]);
		   priv->bounce[0][i] = NULL;
	       }
	   }
       }
   }
}

static int
set_port_attribute(ScrnInfoPtr pScrn,
                   Atom attribute, INT32 value, pointer data)
{
   struct xorg_xv_port_priv *priv = (struct xorg_xv_port_priv *)data;

   if (attribute == xvBrightness) {
      if ((value < -128) || (value > 127))
         return BadValue;
      priv->brightness = value;
   } else if (attribute == xvContrast) {
      if ((value < 0) || (value > 255))
         return BadValue;
      priv->contrast = value;
   } else
      return BadMatch;

   return Success;
}

static int
get_port_attribute(ScrnInfoPtr pScrn,
                   Atom attribute, INT32 * value, pointer data)
{
   struct xorg_xv_port_priv *priv = (struct xorg_xv_port_priv *)data;

   if (attribute == xvBrightness)
      *value = priv->brightness;
  else if (attribute == xvContrast)
      *value = priv->contrast;
   else
      return BadMatch;

   return Success;
}

static void
query_best_size(ScrnInfoPtr pScrn,
                Bool motion,
                short vid_w, short vid_h,
                short drw_w, short drw_h,
                unsigned int *p_w, unsigned int *p_h, pointer data)
{
   if (vid_w > (drw_w << 1))
      drw_w = vid_w >> 1;
   if (vid_h > (drw_h << 1))
      drw_h = vid_h >> 1;

   *p_w = drw_w;
   *p_h = drw_h;
}

static int
check_yuv_surfaces(struct xorg_xv_port_priv *priv,  int width, int height)
{
    struct xa_surface **yuv = priv->yuv;
    struct vmwgfx_dmabuf **bounce = priv->bounce[priv->current_set];
    int ret = 0;
    int i;
    size_t size;

    for (i=0; i<3; ++i) {
	if (!yuv[i])
	    yuv[i] = xa_surface_create(priv->xat, width, height, 8,
				       xa_type_yuv_component,
				       xa_format_unknown, 0);
	else
	    ret = xa_surface_redefine(yuv[i], width, height, 8,
				      xa_type_yuv_component,
				      xa_format_unknown, 0, 0);
	if (ret || !yuv[i])
	    return BadAlloc;

	size = width * height;

	if (bounce[i] && (bounce[i]->size < size ||
			  bounce[i]->size > 2*size)) {
	    vmwgfx_dmabuf_destroy(bounce[i]);
	    bounce[i] = NULL;
	}

	if (!bounce[i]) {
	    bounce[i] = vmwgfx_dmabuf_alloc(priv->drm_fd, size);
	    if (!bounce[i])
		return BadAlloc;
	}
    }
    return Success;
}

static int
query_image_attributes(ScrnInfoPtr pScrn,
                       int id,
                       unsigned short *w, unsigned short *h,
                       int *pitches, int *offsets)
{
   int size, tmp;

   if (*w > IMAGE_MAX_WIDTH)
      *w = IMAGE_MAX_WIDTH;
   if (*h > IMAGE_MAX_HEIGHT)
      *h = IMAGE_MAX_HEIGHT;

   *w = (*w + 1) & ~1;
   if (offsets)
      offsets[0] = 0;

   switch (id) {
   case FOURCC_YV12:
      *h = (*h + 1) & ~1;
      size = (*w + 3) & ~3;
      if (pitches) {
         pitches[0] = size;
      }
      size *= *h;
      if (offsets) {
         offsets[1] = size;
      }
      tmp = ((*w >> 1) + 3) & ~3;
      if (pitches) {
         pitches[1] = pitches[2] = tmp;
      }
      tmp *= (*h >> 1);
      size += tmp;
      if (offsets) {
         offsets[2] = size;
      }
      size += tmp;
      break;
   case FOURCC_UYVY:
   case FOURCC_YUY2:
   default:
      size = *w << 1;
      if (pitches)
	 pitches[0] = size;
      size *= *h;
      break;
   }

   return size;
}

static int
copy_packed_data(ScrnInfoPtr pScrn,
                 struct xorg_xv_port_priv *port,
                 int id,
                 unsigned char *buf,
                 int left,
                 int top,
                 unsigned short w, unsigned short h)
{
   int i, j;
   struct vmwgfx_dmabuf **bounce = port->bounce[port->current_set];
   char *ymap, *vmap, *umap;
   unsigned char y1, y2, u, v;
   int yidx, uidx, vidx;
   int y_array_size = w * h;
   int ret = BadAlloc;

   /*
    * Here, we could use xa_surface_[map|unmap], but given the size of
    * the yuv textures, that could stress the xa tracker dma buffer pool,
    * particularaly with multiple videos rendering simultaneously.
    *
    * Instead, cheat and allocate vmwgfx dma buffers directly.
    */

   ymap = (char *)vmwgfx_dmabuf_map(bounce[0]);
   if (!ymap)
       return BadAlloc;
   umap = (char *)vmwgfx_dmabuf_map(bounce[1]);
   if (!umap)
       goto out_no_umap;
   vmap = (char *)vmwgfx_dmabuf_map(bounce[2]);
   if (!vmap)
       goto out_no_vmap;


   yidx = uidx = vidx = 0;

   switch (id) {
   case FOURCC_YV12: {
      int pitches[3], offsets[3];
      unsigned char *y, *u, *v;
      query_image_attributes(pScrn, FOURCC_YV12,
                             &w, &h, pitches, offsets);

      y = buf + offsets[0];
      v = buf + offsets[1];
      u = buf + offsets[2];
      for (i = 0; i < h; ++i) {
         for (j = 0; j < w; ++j) {
            int yoffset = (w*i+j);
            int ii = (i|1), jj = (j|1);
            int vuoffset = (w/2)*(ii/2) + (jj/2);
            ymap[yidx++] = y[yoffset];
            umap[uidx++] = u[vuoffset];
            vmap[vidx++] = v[vuoffset];
         }
      }
   }
      break;
   case FOURCC_UYVY:
      for (i = 0; i < y_array_size; i +=2 ) {
         /* extracting two pixels */
         u  = buf[0];
         y1 = buf[1];
         v  = buf[2];
         y2 = buf[3];
         buf += 4;

         ymap[yidx++] = y1;
         ymap[yidx++] = y2;
         umap[uidx++] = u;
         umap[uidx++] = u;
         vmap[vidx++] = v;
         vmap[vidx++] = v;
      }
      break;
   case FOURCC_YUY2:
      for (i = 0; i < y_array_size; i +=2 ) {
         /* extracting two pixels */
         y1 = buf[0];
         u  = buf[1];
         y2 = buf[2];
         v  = buf[3];

         buf += 4;

         ymap[yidx++] = y1;
         ymap[yidx++] = y2;
         umap[uidx++] = u;
         umap[uidx++] = u;
         vmap[vidx++] = v;
         vmap[vidx++] = v;
      }
      break;
   default:
       ret = BadAlloc;
       break;
   }

   ret = Success;
   vmwgfx_dmabuf_unmap(bounce[2]);
  out_no_vmap:
   vmwgfx_dmabuf_unmap(bounce[1]);
  out_no_umap:
   vmwgfx_dmabuf_unmap(bounce[0]);

   if (ret == Success) {
       struct xa_surface *srf;
       struct vmwgfx_dmabuf *buf;
       uint32_t handle;
       unsigned int stride;
       BoxRec box;
       RegionRec reg;

       box.x1 = 0;
       box.x2 = w;
       box.y1 = 0;
       box.y2 = h;

       REGION_INIT(pScrn->pScreen, &reg, &box, 1);

       for (i=0; i<3; ++i) {
	   srf = port->yuv[i];
	   buf = bounce[i];

	   if (xa_surface_handle(srf, &handle, &stride) != 0) {
	       ret = BadAlloc;
	       break;
	   }

	   if (vmwgfx_dma(0, 0, &reg, buf, w, handle, 1) != 0) {
	       ret = BadAlloc;
	       break;
	   }
       }
       REGION_UNINIT(pScrn->pScreen, &reg);
   }

   return ret;
}


static int
display_video(ScreenPtr pScreen, struct xorg_xv_port_priv *pPriv, int id,
              RegionPtr dstRegion,
              int src_x, int src_y, int src_w, int src_h,
              int dst_x, int dst_y, int dst_w, int dst_h,
              PixmapPtr pPixmap)
{
    struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(pPixmap);
    Bool hdtv;
    RegionRec reg;
    int ret = BadAlloc;
    int blit_ret;
    const float *conv_matrix;

    REGION_NULL(pScreen, &reg);

    if (!vmwgfx_hw_accel_validate(pPixmap, 0, XA_FLAG_RENDER_TARGET, 0, &reg))
	goto out_no_dst;

   hdtv = ((src_w >= RES_720P_X) && (src_h >= RES_720P_Y));
   conv_matrix = (hdtv ? bt_709 : bt_601);

#ifdef COMPOSITE

   /*
    * For redirected windows, we need to fix up the destination coordinates.
    */

   REGION_TRANSLATE(pScreen, dstRegion, -pPixmap->screen_x,
		    -pPixmap->screen_y);
   dst_x -= pPixmap->screen_x;
   dst_y -= pPixmap->screen_y;
#endif

   /*
    * Throttle on previous blit.
    */

   if (pPriv->fence) {
       (void) xa_fence_wait(pPriv->fence, 1000000000ULL);
       xa_fence_destroy(pPriv->fence);
       pPriv->fence = NULL;
   }

   DamageRegionAppend(&pPixmap->drawable, dstRegion);

   blit_ret = xa_yuv_planar_blit(pPriv->r, src_x, src_y, src_w, src_h,
				 dst_x, dst_y, dst_w, dst_h,
				 (struct xa_box *)REGION_RECTS(dstRegion),
				 REGION_NUM_RECTS(dstRegion),
				 conv_matrix,
				 vpix->hw, pPriv->yuv);

   saa_pixmap_dirty(pPixmap, TRUE, dstRegion);
   DamageRegionProcessPending(&pPixmap->drawable);
   ret = Success;

   if (!blit_ret) {
       ret = Success;
       pPriv->fence = xa_fence_get(pPriv->r);
   } else
       ret = BadAlloc;

   out_no_dst:
   REGION_UNINIT(pScreen, &reg);
   return ret;
}

static int
put_image(ScrnInfoPtr pScrn,
          short src_x, short src_y,
          short drw_x, short drw_y,
          short src_w, short src_h,
          short drw_w, short drw_h,
          int id, unsigned char *buf,
          short width, short height,
          Bool sync, RegionPtr clipBoxes, pointer data,
          DrawablePtr pDraw)
{
   struct xorg_xv_port_priv *pPriv = (struct xorg_xv_port_priv *) data;
   ScreenPtr pScreen = screenInfo.screens[pScrn->scrnIndex];
   PixmapPtr pPixmap;
   INT32 x1, x2, y1, y2;
   BoxRec dstBox;
   int ret;

   /* Clip */
   x1 = src_x;
   x2 = src_x + src_w;
   y1 = src_y;
   y2 = src_y + src_h;

   dstBox.x1 = drw_x;
   dstBox.x2 = drw_x + drw_w;
   dstBox.y1 = drw_y;
   dstBox.y2 = drw_y + drw_h;

   if (!xf86XVClipVideoHelper(&dstBox, &x1, &x2, &y1, &y2, clipBoxes,
			      width, height))
      return Success;

   ret = check_yuv_surfaces(pPriv, width, height);
   if (ret)
       return ret;

   ret = copy_packed_data(pScrn, pPriv, id, buf,
			  src_x, src_y, width, height);
   if (ret)
       return ret;

   if (pDraw->type == DRAWABLE_WINDOW) {
      pPixmap = (*pScreen->GetWindowPixmap)((WindowPtr)pDraw);
   } else {
      pPixmap = (PixmapPtr)pDraw;
   }

   display_video(pScrn->pScreen, pPriv, id, clipBoxes,
                 src_x, src_y, src_w, src_h,
                 drw_x, drw_y,
                 drw_w, drw_h, pPixmap);

   pPriv->current_set = (pPriv->current_set + 1) & 1;
   return Success;
}

static struct xorg_xv_port_priv *
port_priv_create(struct xa_tracker *xat, struct xa_context *r,
		 int drm_fd)
{
   struct xorg_xv_port_priv *priv = NULL;

   priv = calloc(1, sizeof(struct xorg_xv_port_priv));

   if (!priv)
      return NULL;

   priv->r = r;
   priv->xat = xat;
   priv->drm_fd = drm_fd;
   REGION_NULL(pScreen, &priv->clip);

   return priv;
}

static void
vmwgfx_free_textured_adaptor(XF86VideoAdaptorPtr adaptor, Bool free_ports)
{
    if (free_ports) {
	int i;

	for(i=0; i<adaptor->nPorts; ++i) {
	    free(adaptor->pPortPrivates[i].ptr);
	}
    }

    free(adaptor->pAttributes);
    free(adaptor->pPortPrivates);
    xf86XVFreeVideoAdaptorRec(adaptor);
}

static XF86VideoAdaptorPtr
xorg_setup_textured_adapter(ScreenPtr pScreen)
{
   ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
   modesettingPtr ms = modesettingPTR(pScrn);
   XF86VideoAdaptorPtr adapt;
   XF86AttributePtr attrs;
   DevUnion *dev_unions;
   int nports = 16, i;
   int nattributes;
   struct xa_context *xar;

   /*
    * Use the XA default context since we don't expect the X server
    * to render from multiple threads.
    */

   xar = xa_context_default(ms->xat);
   nattributes = NUM_TEXTURED_ATTRIBUTES;

   adapt = calloc(1, sizeof(XF86VideoAdaptorRec));
   dev_unions = calloc(nports, sizeof(DevUnion));
   attrs = calloc(nattributes, sizeof(XF86AttributeRec));
   if (adapt == NULL || dev_unions == NULL || attrs == NULL) {
      free(adapt);
      free(dev_unions);
      free(attrs);
      return NULL;
   }

   adapt->type = XvWindowMask | XvInputMask | XvImageMask;
   adapt->flags = 0;
   adapt->name = "XA G3D Textured Video";
   adapt->nEncodings = 1;
   adapt->pEncodings = DummyEncoding;
   adapt->nFormats = NUM_FORMATS;
   adapt->pFormats = Formats;
   adapt->nPorts = 0;
   adapt->pPortPrivates = dev_unions;
   adapt->nAttributes = nattributes;
   adapt->pAttributes = attrs;
   memcpy(attrs, TexturedAttributes, nattributes * sizeof(XF86AttributeRec));
   adapt->nImages = NUM_IMAGES;
   adapt->pImages = Images;
   adapt->PutVideo = NULL;
   adapt->PutStill = NULL;
   adapt->GetVideo = NULL;
   adapt->GetStill = NULL;
   adapt->StopVideo = stop_video;
   adapt->SetPortAttribute = set_port_attribute;
   adapt->GetPortAttribute = get_port_attribute;
   adapt->QueryBestSize = query_best_size;
   adapt->PutImage = put_image;
   adapt->QueryImageAttributes = query_image_attributes;


   for (i = 0; i < nports; i++) {
       struct xorg_xv_port_priv *priv =
	  port_priv_create(ms->xat, xar, ms->fd);

      adapt->pPortPrivates[i].ptr = (pointer) (priv);
      adapt->nPorts++;
   }

   return adapt;
}

void
xorg_xv_init(ScreenPtr pScreen)
{
   ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
   modesettingPtr ms = modesettingPTR(pScrn);
   XF86VideoAdaptorPtr *adaptors, *new_adaptors = NULL;
   XF86VideoAdaptorPtr textured_adapter = NULL, overlay_adaptor = NULL;
   int num_adaptors;

   num_adaptors = xf86XVListGenericAdaptors(pScrn, &adaptors);
   new_adaptors = malloc((num_adaptors + 2) * sizeof(XF86VideoAdaptorPtr *));
   if (new_adaptors == NULL)
       return;

   memcpy(new_adaptors, adaptors, num_adaptors * sizeof(XF86VideoAdaptorPtr));
   adaptors = new_adaptors;

   /* Add the adaptors supported by our hardware.  First, set up the atoms
    * that will be used by both output adaptors.
    */
   xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
   xvContrast = MAKE_ATOM("XV_CONTRAST");

   if (ms->xat) {
       textured_adapter = xorg_setup_textured_adapter(pScreen);
       if (textured_adapter)
	   adaptors[num_adaptors++] = textured_adapter;
   } else {
       xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		  "No 3D acceleration. Not setting up textured video.\n");
   }

   overlay_adaptor = vmw_video_init_adaptor(pScrn);
   if (overlay_adaptor)
       adaptors[num_adaptors++] = overlay_adaptor;

   if (num_adaptors) {
       Bool ret;
       ret = xf86XVScreenInit(pScreen, adaptors, num_adaptors);
       if (textured_adapter)
	   vmwgfx_free_textured_adaptor(textured_adapter, !ret);
       if (overlay_adaptor)
	   vmw_video_free_adaptor(overlay_adaptor, !ret);
       if (!ret)
	   xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		      "Failed to initialize Xv.\n");
   } else {
       xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		  "Disabling Xv because no adaptors could be initialized.\n");
   }


  out_err_mem:
   free(adaptors);
}
