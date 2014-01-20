/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2011 Dave Airlie
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
 * Original Author: Alan Hourihane <alanh@tungstengraphics.com>
 * Rewrite: Dave Airlie <airlied@redhat.com> 
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include "xf86.h"
#include "xf86_OSproc.h"
#include "compiler.h"
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
#include "shadow.h"
#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include <xorg-server.h>
#ifdef XSERVER_PLATFORM_BUS
#include "xf86platformBus.h"
#endif
#if XSERVER_LIBPCIACCESS
#include <pciaccess.h>
#endif

#include "compat-api.h"
#include "driver.h"

#include "sh3224.h"
static void AdjustFrame(ADJUST_FRAME_ARGS_DECL);
static Bool CloseScreen(CLOSE_SCREEN_ARGS_DECL);
static Bool EnterVT(VT_FUNC_ARGS_DECL);
static void Identify(int flags);
static const OptionInfoRec *AvailableOptions(int chipid, int busid);
static ModeStatus ValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose,
			    int flags);
static void FreeScreen(FREE_SCREEN_ARGS_DECL);
static void LeaveVT(VT_FUNC_ARGS_DECL);
static Bool SwitchMode(SWITCH_MODE_ARGS_DECL);
static Bool ScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool PreInit(ScrnInfoPtr pScrn, int flags);

static Bool Probe(DriverPtr drv, int flags);
static Bool ms_pci_probe(DriverPtr driver,
			 int entity_num, struct pci_device *device,
			 intptr_t match_data);
static Bool ms_driver_func(ScrnInfoPtr scrn, xorgDriverFuncOp op,
			   void *data);

#ifdef XSERVER_LIBPCIACCESS
static const struct pci_id_match ms_device_match[] = {
    {
	PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00030000, 0x00ff0000, 0
    },

    { 0, 0, 0 },
};
#endif

#ifdef XSERVER_PLATFORM_BUS
static Bool ms_platform_probe(DriverPtr driver,
                          int entity_num, int flags, struct xf86_platform_device *device,
			  intptr_t match_data);
#endif

_X_EXPORT DriverRec modesetting = {
    1,
    "modesetting",
    Identify,
    Probe,
    AvailableOptions,
    NULL,
    0,
    ms_driver_func,
    ms_device_match,
    ms_pci_probe,
#ifdef XSERVER_PLATFORM_BUS
    ms_platform_probe,
#endif
};

static SymTabRec Chipsets[] = {
    {0, "kms" },
    {-1, NULL}
};

typedef enum
{
    OPTION_SW_CURSOR,
    OPTION_DEVICE_PATH,
    OPTION_SHADOW_FB,
} modesettingOpts;

static const OptionInfoRec Options[] = {
    {OPTION_SW_CURSOR, "SWcursor", OPTV_BOOLEAN, {0}, FALSE},
    {OPTION_DEVICE_PATH, "kmsdev", OPTV_STRING, {0}, FALSE },
    {OPTION_SHADOW_FB, "ShadowFB", OPTV_BOOLEAN, {0}, FALSE },
    {-1, NULL, OPTV_NONE, {0}, FALSE}
};

int modesettingEntityIndex = -1;

static MODULESETUPPROTO(Setup);

static XF86ModuleVersionInfo VersRec = {
    "modesetting",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData modesettingModuleData = { &VersRec, Setup, NULL };

static pointer
Setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool setupDone = 0;

    /* This module should be loaded only once, but check to be sure.
     */
    if (!setupDone) {
	setupDone = 1;
	xf86AddDriver(&modesetting, module, HaveDriverFuncs);

	/*
	 * The return value must be non-NULL on success even though there
	 * is no TearDownProc.
	 */
	return (pointer) 1;
    } else {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	return NULL;
    }
}

static void
Identify(int flags)
{
    xf86PrintChipsets("modesetting", "Driver for Modesetting Kernel Drivers",
		      Chipsets);
}

static int open_hw(const char *dev)
{
    int fd;
    if (dev)
	fd = open(dev, O_RDWR, 0);
    else {
	dev = getenv("KMSDEVICE");
	if ((NULL == dev) || ((fd = open(dev, O_RDWR, 0)) == -1)) {
	    dev = "/dev/dri/card0";
	    fd = open(dev,O_RDWR, 0);
	}
    }
    if (fd == -1)
	xf86DrvMsg(-1, X_ERROR,"open %s: %s\n", dev, strerror(errno));

    return fd;
}

static int check_outputs(int fd)
{
    drmModeResPtr res = drmModeGetResources(fd);
    int ret;

    if (!res)
        return FALSE;
    ret = res->count_connectors > 0;
    drmModeFreeResources(res);
    return ret;
}

static Bool probe_hw(const char *dev)
{
    int fd = open_hw(dev);
    if (fd != -1) {
        int ret = check_outputs(fd);
        close(fd);
        return ret;
    }
    return FALSE;
}

static char *
ms_DRICreatePCIBusID(const struct pci_device *dev)
{
    char *busID;

    if (asprintf(&busID, "pci:%04x:%02x:%02x.%d",
                 dev->domain, dev->bus, dev->dev, dev->func) == -1)
        return NULL;

    return busID;
}


static Bool probe_hw_pci(const char *dev, struct pci_device *pdev)
{
    int ret = FALSE, fd = open_hw(dev);
    char *id, *devid;
    drmSetVersion sv;

    if (fd == -1)
	return FALSE;

    sv.drm_di_major = 1;
    sv.drm_di_minor = 4;
    sv.drm_dd_major = -1;
    sv.drm_dd_minor = -1;
    if (drmSetInterfaceVersion(fd, &sv)) {
        close(fd);
        return FALSE;
    }


    id = drmGetBusid(fd);
    devid = ms_DRICreatePCIBusID(pdev);
    close(fd);

    if (id && devid && !strcmp(id, devid))
        ret = check_outputs(fd);

    free(id);
    free(devid);
    return ret;
}
static const OptionInfoRec *
AvailableOptions(int chipid, int busid)
{
    return Options;
}

static Bool
ms_driver_func(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data)
{
    xorgHWFlags *flag;
    
    switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
	    flag = (CARD32 *)data;
	    (*flag) = 0;
	    return TRUE;
	default:
	    return FALSE;
    }
}

#if XSERVER_LIBPCIACCESS
static Bool
ms_pci_probe(DriverPtr driver,
	     int entity_num, struct pci_device *dev, intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;

    scrn = xf86ConfigPciEntity(scrn, 0, entity_num, NULL,
			       NULL, NULL, NULL, NULL, NULL);
    if (scrn) {
	const char *devpath;
	GDevPtr devSection = xf86GetDevFromEntity(scrn->entityList[0],
						  scrn->entityInstanceList[0]);

	devpath = xf86FindOptionValue(devSection->options, "kmsdev");
	if (probe_hw_pci(devpath, dev)) {
	    scrn->driverVersion = 1;
	    scrn->driverName = "modesetting";
	    scrn->name = "modeset";
	    scrn->Probe = NULL;
	    scrn->PreInit = PreInit;
	    scrn->ScreenInit = ScreenInit;
	    scrn->SwitchMode = SwitchMode;
	    scrn->AdjustFrame = AdjustFrame;
	    scrn->EnterVT = EnterVT;
	    scrn->LeaveVT = LeaveVT;
	    scrn->FreeScreen = FreeScreen;
	    scrn->ValidMode = ValidMode;

	    xf86DrvMsg(scrn->scrnIndex, X_CONFIG,
		       "claimed PCI slot %d@%d:%d:%d\n", 
		       dev->bus, dev->domain, dev->dev, dev->func);
	    xf86DrvMsg(scrn->scrnIndex, X_INFO,
		       "using %s\n", devpath ? devpath : "default device");
	} else
	    scrn = NULL;
    }
    return scrn != NULL;
}
#endif

#ifdef XSERVER_PLATFORM_BUS
static Bool
ms_platform_probe(DriverPtr driver,
              int entity_num, int flags, struct xf86_platform_device *dev, intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;
    const char *path = xf86_get_platform_device_attrib(dev, ODEV_ATTRIB_PATH);
    int scr_flags = 0;

    if (flags & PLATFORM_PROBE_GPU_SCREEN)
            scr_flags = XF86_ALLOCATE_GPU_SCREEN;

    if (probe_hw(path)) {
        scrn = xf86AllocateScreen(driver, scr_flags);
        xf86AddEntityToScreen(scrn, entity_num);

        scrn->driverName = "modesetting";
        scrn->name = "modesetting";
        scrn->PreInit = PreInit;
        scrn->ScreenInit = ScreenInit;
        scrn->SwitchMode = SwitchMode;
        scrn->AdjustFrame = AdjustFrame;
        scrn->EnterVT = EnterVT;
        scrn->LeaveVT = LeaveVT;
        scrn->FreeScreen = FreeScreen;
        scrn->ValidMode = ValidMode;
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                   "using drv %s\n", path ? path : "default device");
    }

    return scrn != NULL;
}
#endif

static Bool
Probe(DriverPtr drv, int flags)
{
    int i, numDevSections;
    GDevPtr *devSections;
    Bool foundScreen = FALSE;
    const char *dev;
    ScrnInfoPtr scrn = NULL;

    /* For now, just bail out for PROBE_DETECT. */
    if (flags & PROBE_DETECT)
	return FALSE;

    /*
     * Find the config file Device sections that match this
     * driver, and return if there are none.
     */
    if ((numDevSections = xf86MatchDevice("modesetting", &devSections)) <= 0) {
	return FALSE;
    }

    for (i = 0; i < numDevSections; i++) {

	dev = xf86FindOptionValue(devSections[i]->options,"kmsdev");
	if (probe_hw(dev)) {
	    int entity;
	    entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
	    scrn = xf86ConfigFbEntity(scrn, 0, entity,
				  NULL, NULL, NULL, NULL);
	}

	if (scrn) {
	    foundScreen = TRUE;
	    scrn->driverVersion = 1;
	    scrn->driverName = "modesetting";
	    scrn->name = "modesetting";
	    scrn->Probe = Probe;
	    scrn->PreInit = PreInit;
	    scrn->ScreenInit = ScreenInit;
	    scrn->SwitchMode = SwitchMode;
	    scrn->AdjustFrame = AdjustFrame;
	    scrn->EnterVT = EnterVT;
	    scrn->LeaveVT = LeaveVT;
	    scrn->FreeScreen = FreeScreen;
	    scrn->ValidMode = ValidMode;

	    xf86DrvMsg(scrn->scrnIndex, X_INFO,
			   "using %s\n", dev ? dev : "default device");
	}
    }

    free(devSections);

    return foundScreen;
}

static Bool
GetRec(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate)
	return TRUE;

    pScrn->driverPrivate = xnfcalloc(sizeof(modesettingRec), 1);

    return TRUE;
}

static int dispatch_dirty_region(ScrnInfoPtr scrn,
				 PixmapPtr pixmap,
				 DamagePtr damage,
				 int fb_id)
{
    modesettingPtr ms = modesettingPTR(scrn);
    RegionPtr dirty = DamageRegion(damage);
    unsigned num_cliprects = REGION_NUM_RECTS(dirty);

    if (num_cliprects) {
	drmModeClip *clip = malloc(num_cliprects * sizeof(drmModeClip));
	BoxPtr rect = REGION_RECTS(dirty);
	int i, ret;
	    
	if (!clip)
	    return -ENOMEM;

	/* XXX no need for copy? */
	for (i = 0; i < num_cliprects; i++, rect++) {
	    clip[i].x1 = rect->x1;
	    clip[i].y1 = rect->y1;
	    clip[i].x2 = rect->x2;
	    clip[i].y2 = rect->y2;
	}

	/* TODO query connector property to see if this is needed */
	ret = drmModeDirtyFB(ms->fd, fb_id, clip, num_cliprects);
	free(clip);
	DamageEmpty(damage);
	if (ret) {
	    if (ret == -EINVAL)
		return ret;
	}
    }
    return 0;
}

static void dispatch_dirty(ScreenPtr pScreen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);
    PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);
    int fb_id = ms->drmmode.fb_id;
    int ret;

    ret = dispatch_dirty_region(scrn, pixmap, ms->damage, fb_id);
    if (ret == -EINVAL || ret == -ENOSYS) {
	ms->dirty_enabled = FALSE;
	DamageUnregister(&pScreen->GetScreenPixmap(pScreen)->drawable, ms->damage);
	DamageDestroy(ms->damage);
	ms->damage = NULL;
	xf86DrvMsg(scrn->scrnIndex, X_INFO, "Disabling kernel dirty updates, not required.\n");
	return;
    }
}

#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
static void dispatch_dirty_crtc(ScrnInfoPtr scrn, xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(scrn);
    PixmapPtr pixmap = crtc->randr_crtc->scanout_pixmap;
    msPixmapPrivPtr ppriv = msGetPixmapPriv(&ms->drmmode, pixmap);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    DamagePtr damage = drmmode_crtc->slave_damage;
    int fb_id = ppriv->fb_id;
    int ret;

    ret = dispatch_dirty_region(scrn, pixmap, damage, fb_id);
    if (ret) {

    }
}

static void dispatch_slave_dirty(ScreenPtr pScreen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int c;

    for (c = 0; c < xf86_config->num_crtc; c++) {
	xf86CrtcPtr crtc = xf86_config->crtc[c];

	if (!crtc->randr_crtc)
	    continue;
	if (!crtc->randr_crtc->scanout_pixmap)
	    continue;

	dispatch_dirty_crtc(scrn, crtc);
    }
}
#endif

static void msBlockHandler(BLOCKHANDLER_ARGS_DECL)
{
    SCREEN_PTR(arg);
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(pScreen));

    pScreen->BlockHandler = ms->BlockHandler;
    pScreen->BlockHandler(BLOCKHANDLER_ARGS);
    pScreen->BlockHandler = msBlockHandler;
#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
    if (pScreen->isGPU)
        dispatch_slave_dirty(pScreen);
    else
#endif
    if (ms->dirty_enabled)
        dispatch_dirty(pScreen);
}

static void
FreeRec(ScrnInfoPtr pScrn)
{
    modesettingPtr ms;

    if (!pScrn)
        return;

    ms = modesettingPTR(pScrn);
    if (!ms)
        return;
    pScrn->driverPrivate = NULL;

    if (ms->fd > 0) {
        int ret;

        if (ms->pEnt->location.type == BUS_PCI)
            ret = drmClose(ms->fd);
        else
            ret = close(ms->fd);
        (void) ret;
    }
    free(ms->Options);
    free(ms);

}

static Bool
PreInit(ScrnInfoPtr pScrn, int flags)
{
    modesettingPtr ms;
    rgb defaultWeight = { 0, 0, 0 };
    EntityInfoPtr pEnt;
    EntPtr msEnt = NULL;
    char *BusID = NULL;
    const char *devicename;
    Bool prefer_shadow = TRUE;
    uint64_t value = 0;
    int ret;
    int bppflags;
    int defaultdepth, defaultbpp;

    if (pScrn->numEntities != 1)
	return FALSE;

    pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

    if (flags & PROBE_DETECT) {
	return FALSE;
    }

    /* Allocate driverPrivate */
    if (!GetRec(pScrn))
	return FALSE;

    ms = modesettingPTR(pScrn);
    ms->SaveGeneration = -1;
    ms->pEnt = pEnt;

    pScrn->displayWidth = 640;	       /* default it */

    /* Allocate an entity private if necessary */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	msEnt = xf86GetEntityPrivate(pScrn->entityList[0],
				     modesettingEntityIndex)->ptr;
	ms->entityPrivate = msEnt;
    } else
	ms->entityPrivate = NULL;

    if (xf86IsEntityShared(pScrn->entityList[0])) {
	if (xf86IsPrimInitDone(pScrn->entityList[0])) {
	    /* do something */
	} else {
	    xf86SetPrimInitDone(pScrn->entityList[0]);
	}
    }

    pScrn->monitor = pScrn->confScreen->monitor;
    pScrn->progClock = TRUE;
    pScrn->rgbBits = 8;

#if XSERVER_PLATFORM_BUS
    if (pEnt->location.type == BUS_PLATFORM) {
        char *path = xf86_get_platform_device_attrib(pEnt->location.id.plat, ODEV_ATTRIB_PATH);
        ms->fd = open_hw(path);
    }
    else 
#endif
    if (pEnt->location.type == BUS_PCI) {
        ms->PciInfo = xf86GetPciInfoForEntity(ms->pEnt->index);
        if (ms->PciInfo) {
            BusID = malloc(64);
            sprintf(BusID, "PCI:%d:%d:%d",
#if XSERVER_LIBPCIACCESS
                    ((ms->PciInfo->domain << 8) | ms->PciInfo->bus),
                    ms->PciInfo->dev, ms->PciInfo->func
#else
                    ((pciConfigPtr) ms->PciInfo->thisCard)->busnum,
                    ((pciConfigPtr) ms->PciInfo->thisCard)->devnum,
                    ((pciConfigPtr) ms->PciInfo->thisCard)->funcnum
#endif
                );
        }
        ms->fd = drmOpen(NULL, BusID);
    } else {
        devicename = xf86FindOptionValue(ms->pEnt->device->options, "kmsdev");
        ms->fd = open_hw(devicename);
    }
    if (ms->fd < 0)
	return FALSE;

    ms->drmmode.fd = ms->fd;

#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
    pScrn->capabilities = 0;
#ifdef DRM_CAP_PRIME
    ret = drmGetCap(ms->fd, DRM_CAP_PRIME, &value);
    if (ret == 0) {
        if (value & DRM_PRIME_CAP_IMPORT)
            pScrn->capabilities |= RR_Capability_SinkOutput;
    }
#endif
#endif
    drmmode_get_default_bpp(pScrn, &ms->drmmode, &defaultdepth, &defaultbpp);
    if (defaultdepth == 24 && defaultbpp == 24) {
	ms->drmmode.force_24_32 = TRUE;
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "No native 24bpp using shadow to workaround\n");
	defaultbpp = 32;
	bppflags = PreferConvert24to32 | SupportConvert24to32 | Support32bppFb;
    } else
	    bppflags = PreferConvert24to32 | SupportConvert24to32 | Support32bppFb;
    
    if (!xf86SetDepthBpp
	(pScrn, defaultdepth, defaultdepth, defaultbpp, bppflags))
	return FALSE;

    switch (pScrn->depth) {
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

    /* Process the options */
    xf86CollectOptions(pScrn, NULL);
    if (!(ms->Options = malloc(sizeof(Options))))
	return FALSE;
    memcpy(ms->Options, Options, sizeof(Options));
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, ms->Options);

    if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
	return FALSE;
    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;

    if (xf86ReturnOptValBool(ms->Options, OPTION_SW_CURSOR, FALSE)) {
	ms->drmmode.sw_cursor = TRUE;
    }

    if (ms->drmmode.force_24_32) {
	    prefer_shadow = TRUE;
	    ms->drmmode.shadow_enable = TRUE;
    } else {
	    ret = drmGetCap(ms->fd, DRM_CAP_DUMB_PREFER_SHADOW, &value);
	    if (!ret) {
		    prefer_shadow = !!value;
	    }

	    ms->drmmode.shadow_enable = xf86ReturnOptValBool(ms->Options, OPTION_SHADOW_FB, prefer_shadow);
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "ShadowFB: preferred %s, enabled %s\n", prefer_shadow ? "YES" : "NO", ms->drmmode.shadow_enable ? (ms->drmmode.force_24_32 ? "FORCE" : "YES") : "NO");
    if (drmmode_pre_init(pScrn, &ms->drmmode, pScrn->bitsPerPixel / 8) == FALSE) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KMS setup failed\n");
	goto fail;
    }

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
    if (!xf86LoadSubModule(pScrn, "fb")) {
	return FALSE;
    }

    if (ms->drmmode.shadow_enable) {
	if (!xf86LoadSubModule(pScrn, "shadow")) {
	    return FALSE;
	}
    }

    return TRUE;
    fail:
    return FALSE;
}

static void *
msShadowWindow(ScreenPtr screen, CARD32 row, CARD32 offset, int mode,
	       CARD32 *size, void *closure)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(pScrn);
    int stride;
    int kernbpp = (ms->drmmode.force_24_32 && pScrn->bitsPerPixel == 32) ? 24 : 32;

    stride = (pScrn->displayWidth * kernbpp) / 8;
    *size = stride;

    return ((uint8_t *)ms->drmmode.front_bo->ptr + row * stride + offset);
}

static Bool
CreateScreenResources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    PixmapPtr rootPixmap;
    Bool ret;
    void *pixels;
    Bool use_ms_shadow = ms->drmmode.force_24_32 && pScrn->bitsPerPixel == 32;
    pScreen->CreateScreenResources = ms->createScreenResources;
    ret = pScreen->CreateScreenResources(pScreen);
    pScreen->CreateScreenResources = CreateScreenResources;
    
    if (!drmmode_set_desired_modes(pScrn, &ms->drmmode))
      return FALSE;

    drmmode_uevent_init(pScrn, &ms->drmmode);

    if (!ms->SWCursor)
        drmmode_map_cursor_bos(pScrn, &ms->drmmode);
    pixels = drmmode_map_front_bo(&ms->drmmode);
    if (!pixels)
	return FALSE;

    rootPixmap = pScreen->GetScreenPixmap(pScreen);

    if (ms->drmmode.shadow_enable)
	pixels = ms->drmmode.shadow_fb;
    
    if (!pScreen->ModifyPixmapHeader(rootPixmap, -1, -1, -1, -1, -1, pixels))
	FatalError("Couldn't adjust screen pixmap\n");

    if (ms->drmmode.shadow_enable) {
	if (!shadowAdd(pScreen, rootPixmap, use_ms_shadow ? ms_shadowUpdate32to24 : shadowUpdatePackedWeak(),
		       msShadowWindow, 0, 0))
	    return FALSE;
    }

    ms->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE,
                              pScreen, rootPixmap);

    if (ms->damage) {
	DamageRegister(&rootPixmap->drawable, ms->damage);
	ms->dirty_enabled = TRUE;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Damage tracking initialized\n");
    } else {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Failed to create screen damage record\n");
	return FALSE;
    }
    return ret;
}

static Bool
msShadowInit(ScreenPtr pScreen)
{
    if (!shadowSetup(pScreen)) {
	return FALSE;
    }
    return TRUE;
}

#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
static Bool
msSetSharedPixmapBacking(PixmapPtr ppix, void *fd_handle)
{
    ScreenPtr screen = ppix->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    Bool ret;
    int size = ppix->devKind * ppix->drawable.height;
    int ihandle = (int)(long)fd_handle;

    ret = drmmode_SetSlaveBO(ppix, &ms->drmmode, ihandle, ppix->devKind, size);
    if (ret == FALSE)
	    return ret;

    return TRUE;
}
#endif

static Bool
ScreenInit(SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    VisualPtr visual;
    int ret;
    pScrn->pScreen = pScreen;

    ret = drmSetMaster(ms->fd);
    if (ret) {
        ErrorF("Unable to set master\n");
        return FALSE;
    }
      
    /* HW dependent - FIXME */
    pScrn->displayWidth = pScrn->virtualX;
    if (!drmmode_create_initial_bos(pScrn, &ms->drmmode))
	return FALSE;

    if (ms->drmmode.shadow_enable) {
	ms->drmmode.shadow_fb = calloc(1, pScrn->displayWidth * pScrn->virtualY *
			       ((pScrn->bitsPerPixel + 7) >> 3));
	if (!ms->drmmode.shadow_fb)
	    ms->drmmode.shadow_enable = FALSE;
    }	
    
    miClearVisualTypes();

    if (!miSetVisualTypes(pScrn->depth,
			  miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	return FALSE;

    if (!miSetPixmapDepths())
	return FALSE;

#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
    if (!dixRegisterScreenSpecificPrivateKey(pScreen, &ms->drmmode.pixmapPrivateKeyRec,
        PRIVATE_PIXMAP, sizeof(msPixmapPrivRec))) { 
	return FALSE;
    }
#endif

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

    if (ms->drmmode.shadow_enable && !msShadowInit(pScreen)) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "shadow fb init failed\n");
	return FALSE;
    }
  
    ms->createScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = CreateScreenResources;

    xf86SetBlackWhitePixels(pScreen);

    xf86SetBackingStore(pScreen);
    xf86SetSilkenMouse(pScreen);
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    /* Need to extend HWcursor support to handle mask interleave */
    if (!ms->drmmode.sw_cursor)
	xf86_cursors_init(pScreen, 64, 64,
			  HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
			  HARDWARE_CURSOR_ARGB);

    /* Must force it before EnterVT, so we are in control of VT and
     * later memory should be bound when allocating, e.g rotate_mem */
    pScrn->vtSema = TRUE;

    pScreen->SaveScreen = xf86SaveScreen;
    ms->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = CloseScreen;

    ms->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = msBlockHandler;

#ifdef MODESETTING_OUTPUT_SLAVE_SUPPORT
    pScreen->SetSharedPixmapBacking = msSetSharedPixmapBacking;
#endif

    if (!xf86CrtcScreenInit(pScreen))
	return FALSE;

    if (!miCreateDefColormap(pScreen))
	return FALSE;

    xf86DPMSInit(pScreen, xf86DPMSSet, 0);

    if (serverGeneration == 1)
	xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

    return EnterVT(VT_FUNC_ARGS);
}

static void
AdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    modesettingPtr ms = modesettingPTR(pScrn);

    drmmode_adjust_frame(pScrn, &ms->drmmode, x, y);
}

static void
FreeScreen(FREE_SCREEN_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    FreeRec(pScrn);
}

static void
LeaveVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86_hide_cursors(pScrn);

    pScrn->vtSema = FALSE;

    drmDropMaster(ms->fd);
}

/*
 * This gets called when gaining control of the VT, and from ScreenInit().
 */
static Bool
EnterVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    modesettingPtr ms = modesettingPTR(pScrn);

    pScrn->vtSema = TRUE;

    if (drmSetMaster(ms->fd)) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "drmSetMaster failed: %s\n",
                   strerror(errno));
    }

    if (!drmmode_set_desired_modes(pScrn, &ms->drmmode))
	return FALSE;

    return TRUE;
}

static Bool
SwitchMode(SWITCH_MODE_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);

    return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

static Bool
CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    if (ms->damage) {
	DamageUnregister(&pScreen->GetScreenPixmap(pScreen)->drawable, ms->damage);
	DamageDestroy(ms->damage);
	ms->damage = NULL;
    }

    if (ms->drmmode.shadow_enable) {
	shadowRemove(pScreen, pScreen->GetScreenPixmap(pScreen));
	free(ms->drmmode.shadow_fb);
	ms->drmmode.shadow_fb = NULL;
    }
    drmmode_uevent_fini(pScrn, &ms->drmmode);

    drmmode_free_bos(pScrn, &ms->drmmode);

    if (pScrn->vtSema) {
        LeaveVT(VT_FUNC_ARGS);
    }

    pScreen->CreateScreenResources = ms->createScreenResources;
    pScreen->BlockHandler = ms->BlockHandler;

    pScrn->vtSema = FALSE;
    pScreen->CloseScreen = ms->CloseScreen;
    return (*pScreen->CloseScreen) (CLOSE_SCREEN_ARGS);
}

static ModeStatus
ValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose, int flags)
{
    return MODE_OK;
}
