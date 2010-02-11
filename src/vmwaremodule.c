/**********************************************************
 * Copyright 2010 VMware, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include <xorg-server.h>
#include <xf86.h>
#include <xf86drm.h>


/*
 * Defines and exported module info.
 */

#define VMWARE_DRIVER_NAME    "vmware"
#define VMWGFX_DRIVER_NAME    "vmwgfx"
#define VMWLEGACY_DRIVER_NAME "vmwlegacy"

#define VMW_STRING_INNER(s) #s
#define VMW_STRING(str) VMW_STRING_INNER(str)

#define VMWARE_VERSION_MAJOR 10
#define VMWARE_VERSION_MINOR 16
#define VMWARE_VERSION_PATCH 9
#define VMWARE_VERSION_STRING_MAJOR VMW_STRING(VMWARE_VERSION_MAJOR)
#define VMWARE_VERSION_STRING_MINOR VMW_STRING(VMWARE_VERSION_MINOR)
#define VMWARE_VERSION_STRING_PATCH VMW_STRING(VMWARE_VERSION_PATCH)

#define VMWARE_DRIVER_VERSION \
   (VMWARE_VERSION_MAJOR * 65536 + VMWARE_VERSION_MINOR * 256 + VMWARE_VERSION_PATCH)
#define VMWARE_DRIVER_VERSION_STRING \
    VMWARE_VERSION_STRING_MAJOR "." VMWARE_VERSION_STRING_MINOR \
    "." VMWARE_VERSION_STRING_PATCH

/*
 * Standard four digit version string expected by VMware Tools installer.
 * As the driver's version is only  {major, minor, patchlevel}, simply append an
 * extra zero for the fourth digit.
 */
#ifdef __GNUC__
const char vmware_modinfo[] __attribute__((section(".modinfo"),unused)) =
    "version=" VMWARE_DRIVER_VERSION_STRING ".0";
#endif

static XF86ModuleVersionInfo vmware_version;
static MODULESETUPPROTO(vmware_setup);

_X_EXPORT XF86ModuleData vmwareModuleData = {
    &vmware_version,
    vmware_setup,
    NULL
};


/*
 * Chain loading functions
 */

static Bool
vmware_check_kernel_module()
{
    /* Super simple way of knowing if the kernel driver is loaded */
    int ret = drmOpen("vmwgfx", NULL);
    if (ret < 0)
	return FALSE;

    drmClose(ret);

    return TRUE;
}

static Bool
vmware_chain_module(pointer opts)
{
    int vmwlegacy_devices;
    int vmwgfx_devices;
    int vmware_devices;
    int matched;
    char *driver_name;
    GDevPtr *gdevs;
    GDevPtr gdev;
    int i;

    vmware_devices = xf86MatchDevice(VMWARE_DRIVER_NAME, &gdevs);
    vmwgfx_devices = xf86MatchDevice(VMWGFX_DRIVER_NAME, NULL);
    vmwlegacy_devices = xf86MatchDevice(VMWLEGACY_DRIVER_NAME, NULL);

    if (vmware_check_kernel_module()) {
	driver_name = VMWGFX_DRIVER_NAME;
	matched = vmwgfx_devices;
    } else {
	driver_name = VMWLEGACY_DRIVER_NAME;
	matched = vmwlegacy_devices;
    }

    for (i = 0; i < vmware_devices; i++) {
	gdev = gdevs[i];
	gdev->driver = driver_name;
    }

    xfree(gdevs);

    if (!matched)
	xf86LoadOneModule(driver_name, opts);
}


/*
 * Module info
 */

static XF86ModuleVersionInfo vmware_version = {
    VMWARE_DRIVER_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    VMWARE_VERSION_MAJOR, VMWARE_VERSION_MINOR, VMWARE_VERSION_PATCH,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};

static pointer
vmware_setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool setupDone = 0;
    int ret;

    /* This module should be loaded only once, but check to be sure. */
    if (!setupDone) {
	setupDone = 1;

	/* Chain load the real driver */
	vmware_chain_module(opts);

	return (pointer) 1;
    } else {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	return NULL;
    }
}
