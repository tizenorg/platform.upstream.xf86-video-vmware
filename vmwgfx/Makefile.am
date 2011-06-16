vmwgfx_drv_la_LTLIBRARIES = vmwgfx_drv.la
vmwgfx_drv_la_LDFLAGS = -module -avoid-version
vmwgfx_drv_la_CFLAGS = $(CWARNFLAGS) $(XORG_CFLAGS) @LIBDRM_CFLAGS@ @XATRACKER_CFLAGS@ -I$(top_srcdir)/src -I$(top_srcdir)/saa
vmwgfx_drv_la_LIBADD = @LIBDRM_LIBS@ $(top_srcdir)/saa/.libs/libsaa.la\
	@XATRACKER_LIBS@
vmwgfx_drv_ladir = @moduledir@/drivers

vmwgfx_drv_la_SOURCES = \
	vmwgfx_driver.c \
	vmwgfx_driver.h \
	vmwgfx_crtc.c \
	vmwgfx_output.c \
	vmwgfx_dri2.c \
	vmwgfx_tex_video.c \
	vmwgfx_saa.c \
	vmwgfx_saa.h \
	vmwgfx_drmi.c \
	vmwgfx_drmi.h \
	vmwgfx_bootstrap.c \
	vmwgfx_overlay.c \
	vmwgfx_ctrl.c \
	vmwgfx_ctrl.h



