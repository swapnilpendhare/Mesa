/*
 * Mesa 3-D graphics library
 * Version:  7.11
 *
 * Copyright (C) 2011 Benjamin Franzke <benjaminfranzke@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "pipe/p_compiler.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "state_tracker/drm_driver.h"

#include "egllog.h"
#include <errno.h>

#include "native_wayland.h"

/* see get_drm_screen_name */
#include <radeon_drm.h>
#include "radeon/drm/radeon_drm_public.h"

#include <wayland-client.h>
#include "wayland-drm-client-protocol.h"
#include "wayland-egl-priv.h"

#include <xf86drm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

struct wayland_drm_display {
   struct wayland_display base;

   struct native_event_handler *event_handler;

   struct wl_drm *wl_drm;
   int fd;
   char *device_name;
   boolean authenticated;
};

static INLINE struct wayland_drm_display *
wayland_drm_display(const struct native_display *ndpy)
{
   return (struct wayland_drm_display *) ndpy;
}

static void
sync_callback(void *data)
{
   int *done = data;

   *done = 1;
}

static void
force_roundtrip(struct wl_display *display)
{
   int done = 0;

   wl_display_sync_callback(display, sync_callback, &done);
   wl_display_iterate(display, WL_DISPLAY_WRITABLE);
   while (!done)
      wl_display_iterate(display, WL_DISPLAY_READABLE);
}

static void 
wayland_drm_display_destroy(struct native_display *ndpy)
{
   struct wayland_drm_display *drmdpy = wayland_drm_display(ndpy);

   if (drmdpy->fd)
      close(drmdpy->fd);
   if (drmdpy->wl_drm)
      wl_drm_destroy(drmdpy->wl_drm);
   if (drmdpy->device_name)
      FREE(drmdpy->device_name);
   if (drmdpy->base.config)
      FREE(drmdpy->base.config);

   ndpy_uninit(ndpy);

   FREE(drmdpy);
}

static struct wl_buffer *
wayland_create_drm_buffer(struct wayland_display *display,
                          struct wayland_surface *surface,
                          enum native_attachment attachment)
{
   struct wayland_drm_display *drmdpy = (struct wayland_drm_display *) display;
   struct pipe_screen *screen = drmdpy->base.base.screen;
   struct pipe_resource *resource;
   struct winsys_handle wsh;
   uint width, height;
   struct wl_visual *visual;

   resource = resource_surface_get_single_resource(surface->rsurf, attachment);
   resource_surface_get_size(surface->rsurf, &width, &height);

   wsh.type = DRM_API_HANDLE_TYPE_SHARED;
   screen->resource_get_handle(screen, resource, &wsh);

   pipe_resource_reference(&resource, NULL);

   switch (surface->type) {
   case WL_WINDOW_SURFACE:
      visual = surface->win->visual;
      break;
   case WL_PIXMAP_SURFACE:
      visual = surface->pix->visual;
      break;
   default:
      return NULL;
   }

   return wl_drm_create_buffer(drmdpy->wl_drm, wsh.handle,
                               width, height, wsh.stride, visual);
}

static const char *
get_drm_screen_name(int fd, drmVersionPtr version)
{
   const char *name = version->name;

   if (name && !strcmp(name, "radeon")) {
      int chip_id;
      struct drm_radeon_info info;

      memset(&info, 0, sizeof(info));
      info.request = RADEON_INFO_DEVICE_ID;
      info.value = pointer_to_intptr(&chip_id);
      if (drmCommandWriteRead(fd, DRM_RADEON_INFO, &info, sizeof(info)) != 0)
         return NULL;

      name = is_r3xx(chip_id) ? "r300" : "r600";
   }

   return name;
}

static void
drm_handle_device(void *data, struct wl_drm *drm, const char *device)
{
   struct wayland_drm_display *drmdpy = data;
   drm_magic_t magic;

   drmdpy->device_name = strdup(device);
   if (!drmdpy->device_name)
      return;

   drmdpy->fd = open(drmdpy->device_name, O_RDWR);
   if (drmdpy->fd == -1) {
      _eglLog(_EGL_WARNING, "wayland-egl: could not open %s (%s)",
              drmdpy->device_name, strerror(errno));
      return;
   }

   drmGetMagic(drmdpy->fd, &magic);
   wl_drm_authenticate(drmdpy->wl_drm, magic);
}

static void
drm_handle_authenticated(void *data, struct wl_drm *drm)
{
   struct wayland_drm_display *drmdpy = data;

   drmdpy->authenticated = true;
}

static const struct wl_drm_listener drm_listener = {
   drm_handle_device,
   drm_handle_authenticated
};

static boolean
wayland_drm_display_init_screen(struct native_display *ndpy)
{
   struct wayland_drm_display *drmdpy = wayland_drm_display(ndpy);
   drmVersionPtr version;
   const char *driver_name;
   uint32_t id;

   id = wl_display_get_global(drmdpy->base.dpy, "wl_drm", 1);
   if (id == 0)
      wl_display_iterate(drmdpy->base.dpy, WL_DISPLAY_READABLE);
   id = wl_display_get_global(drmdpy->base.dpy, "wl_drm", 1);
   if (id == 0)
      return FALSE;

   drmdpy->wl_drm = wl_drm_create(drmdpy->base.dpy, id, 1);
   if (!drmdpy->wl_drm)
      return FALSE;

   wl_drm_add_listener(drmdpy->wl_drm, &drm_listener, drmdpy);
   force_roundtrip(drmdpy->base.dpy);
   if (drmdpy->fd == -1)
      return FALSE;

   force_roundtrip(drmdpy->base.dpy);
   if (!drmdpy->authenticated)
      return FALSE;

   version = drmGetVersion(drmdpy->fd);
   if (!version) {
      _eglLog(_EGL_WARNING, "invalid fd %d", drmdpy->fd);
      return FALSE;
   }

   /* FIXME: share this with native_drm or egl_dri2 */
   driver_name = get_drm_screen_name(drmdpy->fd, version);

   drmdpy->base.base.screen =
      drmdpy->event_handler->new_drm_screen(&drmdpy->base.base,
                                            driver_name, drmdpy->fd);
   drmFreeVersion(version);

   if (!drmdpy->base.base.screen) {
      _eglLog(_EGL_WARNING, "failed to create DRM screen");
      return FALSE;
   }

   return TRUE;
}

static struct pipe_resource *
wayland_drm_display_import_buffer(struct native_display *ndpy,
                                  const struct pipe_resource *templ,
                                  void *buf)
{
   return ndpy->screen->resource_from_handle(ndpy->screen,
                                             templ, (struct winsys_handle *) buf);
}

static boolean
wayland_drm_display_export_buffer(struct native_display *ndpy,
                                  struct pipe_resource *res,
                                  void *buf)
{
   return ndpy->screen->resource_get_handle(ndpy->screen,
                                            res, (struct winsys_handle *) buf);
}

static struct native_display_buffer wayland_drm_display_buffer = {
   wayland_drm_display_import_buffer,
   wayland_drm_display_export_buffer
};

struct wayland_display *
wayland_create_drm_display(struct wl_display *dpy,
                           struct native_event_handler *event_handler,
                           void *user_data)
{
   struct wayland_drm_display *drmdpy;

   drmdpy = CALLOC_STRUCT(wayland_drm_display);
   if (!drmdpy)
      return NULL;

   drmdpy->event_handler = event_handler;
   drmdpy->base.base.user_data = user_data;

   drmdpy->base.dpy = dpy;
   if (!drmdpy->base.dpy) {
      wayland_drm_display_destroy(&drmdpy->base.base);
      return NULL;
   }

   if (!wayland_drm_display_init_screen(&drmdpy->base.base)) {
      wayland_drm_display_destroy(&drmdpy->base.base);
      return NULL;
   }
   drmdpy->base.base.destroy = wayland_drm_display_destroy;
   drmdpy->base.base.buffer = &wayland_drm_display_buffer;

   drmdpy->base.create_buffer = wayland_create_drm_buffer;

   return &drmdpy->base;
}

/* vim: set sw=3 ts=8 sts=3 expandtab: */