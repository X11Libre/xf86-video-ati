/*
 * Copyright © 2014 Intel Corporation
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "radeon.h"

#ifdef HAVE_PRESENT_H

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include "radeon_video.h"

#include "present.h"

struct radeon_present_vblank_event {
    uint64_t event_id;
};

static uint32_t crtc_select(int crtc_id)
{
    if (crtc_id > 1)
	return crtc_id << DRM_VBLANK_HIGH_CRTC_SHIFT;
    else if (crtc_id > 0)
	return DRM_VBLANK_SECONDARY;
    else
	return 0;
}

static RRCrtcPtr
radeon_present_get_crtc(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
    xf86CrtcPtr crtc;
    RRCrtcPtr randr_crtc = NULL;

    crtc = radeon_pick_best_crtc(pScrn, FALSE,
				 window->drawable.x,
				 window->drawable.x + window->drawable.width,
				 window->drawable.y,
				 window->drawable.y + window->drawable.height);

    /* Make sure the CRTC is valid and this is the real front buffer */
    if (crtc != NULL && !crtc->rotatedData)
	randr_crtc = crtc->randr_crtc;

    return randr_crtc;
}

static int
radeon_present_get_ust_msc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    return drmmode_crtc_get_ust_msc(crtc->devPrivate, ust, msc);
}

/*
 * Flush the DRM event queue when full; this
 * makes space for new requests
 */
static Bool
radeon_present_flush_drm_events(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = xf86_config->crtc[0]->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    struct pollfd p = { .fd = drmmode->fd, .events = POLLIN };
    int r;

    do {
	r = poll(&p, 1, 0);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));

    if (r <= 0)
	return 0;

    return drmHandleEvent(drmmode->fd, &drmmode->event_context) >= 0;
}

/*
 * Called when the queued vblank event has occurred
 */
static void
radeon_present_vblank_handler(ScrnInfoPtr scrn, unsigned int msc,
			      uint64_t usec, void *data)
{
    struct radeon_present_vblank_event *event = data;

    present_event_notify(event->event_id, usec, msc);
    free(event);
}

/*
 * Called when the queued vblank is aborted
 */
static void
radeon_present_vblank_abort(ScrnInfoPtr scrn, void *data)
{
    struct radeon_present_vblank_event *event = data;

    free(event);
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has past
 */
static int
radeon_present_queue_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr xf86_crtc = crtc->devPrivate;
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    RADEONInfoPtr info = RADEONPTR(scrn);
    int crtc_id = drmmode_get_crtc_id(xf86_crtc);
    struct radeon_present_vblank_event *event;
    struct radeon_drm_queue_entry *queue;
    drmVBlank vbl;
    int ret;

    event = calloc(sizeof(struct radeon_present_vblank_event), 1);
    if (!event)
	return BadAlloc;
    event->event_id = event_id;
    queue = radeon_drm_queue_alloc(scrn, RADEON_DRM_QUEUE_CLIENT_DEFAULT,
				   event_id, event,
				   radeon_present_vblank_handler,
				   radeon_present_vblank_abort);
    if (!queue) {
	free(event);
	return BadAlloc;
    }

    vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | crtc_select(crtc_id);
    vbl.request.sequence = msc;
    vbl.request.signal = (unsigned long)queue;
    for (;;) {
	ret = drmWaitVBlank(info->dri2.drm_fd, &vbl);
	if (!ret)
	    break;
	if (errno != EBUSY || !radeon_present_flush_drm_events(screen)) {
	    radeon_drm_abort_entry(queue);
	    return BadAlloc;
	}
    }

    return Success;
}

/*
 * Remove a pending vblank event from the DRM queue so that it is not reported
 * to the extension
 */
static void
radeon_present_abort_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    radeon_drm_abort_id(event_id);
}

/*
 * Flush our batch buffer when requested by the Present extension.
 */
static void
radeon_present_flush(WindowPtr window)
{
    radeon_cs_flush_indirect(xf86ScreenToScrn(window->drawable.pScreen));
}

static drmmode_crtc_private_ptr
get_drmmode_crtc(ScrnInfoPtr scrn, RRCrtcPtr crtc)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int i;

    for (i = 0; i < config->num_crtc; i++) {
	xf86CrtcPtr xf86crtc = config->crtc[i];

	if (xf86crtc->randr_crtc == crtc)
	    return xf86crtc->driver_private;
    }

    return NULL;
}

static Bool
radeon_present_get_pixmap_handle(PixmapPtr pixmap, uint32_t *handle)
{
    struct radeon_bo *bo = radeon_get_pixmap_bo(pixmap);

    if (bo) {
	*handle = bo->handle;
	return TRUE;
    }

    return FALSE;
}

/*
 * Test to see if page flipping is possible on the target crtc
 */
static Bool
radeon_present_check_flip(RRCrtcPtr crtc, WindowPtr window, PixmapPtr pixmap,
			  Bool sync_flip)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    RADEONInfoPtr info = RADEONPTR(scrn);

    if (!scrn->vtSema)
	return FALSE;

    if (!info->allowPageFlip)
	return FALSE;

    if (crtc) {
	drmmode_crtc_private_ptr drmmode_crtc = get_drmmode_crtc(scrn, crtc);

	if (!drmmode_crtc ||
	    drmmode_crtc->rotate_bo != NULL ||
	    drmmode_crtc->dpms_mode != DPMSModeOn)
	    return FALSE;
    }

    return TRUE;
}

/*
 * Once the flip has been completed on all CRTCs, notify the
 * extension code telling it when that happened
 */
static void
radeon_present_flip_event(ScrnInfoPtr scrn, uint32_t msc, uint64_t ust, void *pageflip_data)
{
    struct radeon_present_vblank_event *event = pageflip_data;

    present_event_notify(event->event_id, ust, msc);
    free(event);
}

/*
 * The flip has been aborted, free the structure
 */
static void
radeon_present_flip_abort(ScrnInfoPtr scrn, void *pageflip_data)
{
    struct radeon_present_vblank_event *event = pageflip_data;

    free(event);
}

/*
 * Queue a flip on 'crtc' to 'pixmap' at 'target_msc'. If 'sync_flip' is true,
 * then wait for vblank. Otherwise, flip immediately
 */
static Bool
radeon_present_flip(RRCrtcPtr crtc, uint64_t event_id, uint64_t target_msc,
                   PixmapPtr pixmap, Bool sync_flip)
{
    ScreenPtr screen = crtc->pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct radeon_present_vblank_event *event;
    drmmode_crtc_private_ptr drmmode_crtc = get_drmmode_crtc(scrn, crtc);
    int crtc_id = drmmode_crtc->mode_crtc->crtc_id;
    uint32_t handle;
    Bool ret;

    if (!sync_flip)
	return FALSE;

    if (!radeon_present_check_flip(crtc, screen->root, pixmap, sync_flip))
	return FALSE;

    if (!radeon_present_get_pixmap_handle(pixmap, &handle))
	return FALSE;

    event = calloc(1, sizeof(struct radeon_present_vblank_event));
    if (!event)
	return FALSE;

    event->event_id = event_id;

    ret = radeon_do_pageflip(scrn, RADEON_DRM_QUEUE_CLIENT_DEFAULT, handle,
			     event_id, event, crtc_id,
			     radeon_present_flip_event,
			     radeon_present_flip_abort);
    if (!ret)
	xf86DrvMsg(scrn->scrnIndex, X_ERROR, "present flip failed\n");

    return ret;
}

/*
 * Queue a flip back to the normal frame buffer
 */
static void
radeon_present_unflip(ScreenPtr screen, uint64_t event_id)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct radeon_present_vblank_event *event;
    PixmapPtr pixmap = screen->GetScreenPixmap(screen);
    uint32_t handle;
    Bool ret;

    if (!radeon_present_check_flip(NULL, screen->root, pixmap, TRUE))
	return;

    if (!radeon_present_get_pixmap_handle(pixmap, &handle))
	return;

    event = calloc(1, sizeof(struct radeon_present_vblank_event));
    if (!event)
	return;

    event->event_id = event_id;

    ret = radeon_do_pageflip(scrn, RADEON_DRM_QUEUE_CLIENT_DEFAULT, handle,
			     event_id, event, -1, radeon_present_flip_event,
			     radeon_present_flip_abort);
    if (!ret)
	xf86DrvMsg(scrn->scrnIndex, X_ERROR, "present unflip failed\n");
}

static present_screen_info_rec radeon_present_screen_info = {
    .version = 0,

    .get_crtc = radeon_present_get_crtc,
    .get_ust_msc = radeon_present_get_ust_msc,
    .queue_vblank = radeon_present_queue_vblank,
    .abort_vblank = radeon_present_abort_vblank,
    .flush = radeon_present_flush,

    .capabilities = PresentCapabilityNone,
    .check_flip = radeon_present_check_flip,
    .flip = radeon_present_flip,
    .unflip = radeon_present_unflip,
};

static Bool
radeon_present_has_async_flip(ScreenPtr screen)
{
#ifdef DRM_CAP_ASYNC_PAGE_FLIP
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    RADEONInfoPtr info = RADEONPTR(scrn);
    int ret;
    uint64_t value;

    ret = drmGetCap(info->dri2.drm_fd, DRM_CAP_ASYNC_PAGE_FLIP, &value);
    if (ret == 0)
	return value == 1;
#endif
    return FALSE;
}

Bool
radeon_present_screen_init(ScreenPtr screen)
{
    if (radeon_present_has_async_flip(screen))
	radeon_present_screen_info.capabilities |= PresentCapabilityAsync;

    if (!present_screen_init(screen, &radeon_present_screen_info)) {
	xf86DrvMsg(xf86ScreenToScrn(screen)->scrnIndex, X_WARNING,
		   "Present extension disabled because present_screen_init failed\n");
	return FALSE;
    }

    xf86DrvMsg(xf86ScreenToScrn(screen)->scrnIndex, X_INFO,
	       "Present extension enabled\n");

    return TRUE;
}

#else /* !HAVE_PRESENT_H */

Bool
radeon_present_screen_init(ScreenPtr screen)
{
    xf86DrvMsg(xf86ScreenToScrn(screen)->scrnIndex, X_INFO,
	       "Present extension disabled because present.h not available at "
	       "build time\n");

    return FALSE;
}

#endif
