#include "compositor.h"

#include <errno.h>
#include <stdio.h>
#include <wayland-server.h>
#include <zsurface.h>

#include "compositor_global.h"
#include "region.h"
#include "surface.h"
#include "util.h"

static void zwl_compositor_destroy(struct zwl_compositor *compositor);

static void zwl_compositor_handle_destroy(struct wl_resource *resource)
{
  struct zwl_compositor *compositor;

  compositor = wl_resource_get_user_data(resource);

  zwl_compositor_destroy(compositor);
}

static void zwl_compositor_protocol_create_surface(struct wl_client *client, struct wl_resource *resource,
                                                   uint32_t id)
{
  struct zwl_compositor *compositor;
  struct zwl_surface *surface;

  compositor = wl_resource_get_user_data(resource);

  surface = zwl_surface_create(client, id, compositor);
  if (surface == NULL) fprintf(stderr, "failed to create a surface\n");
}

static void zwl_compositor_protocol_create_region(struct wl_client *client, struct wl_resource *resource,
                                                  uint32_t id)
{
  UNUSED(resource);
  struct zwl_region *region;
  region = zwl_region_create(client, id);
  if (region == NULL) fprintf(stderr, "failed to create a region\n");

  // TODO: implement
}

static const struct wl_compositor_interface zwl_compositor_interface = {
    .create_surface = zwl_compositor_protocol_create_surface,
    .create_region = zwl_compositor_protocol_create_region,
};

static void zwl_compositor_global_flush_handler(struct wl_listener *listener, void *data)
{
  UNUSED(data);
  struct zwl_compositor *compositor;

  compositor = wl_container_of(listener, compositor, global_flush_listener);

  zsurface_flush(compositor->zsurface);
}

static int zwl_compositor_zsurface_dispatch(int fd, uint32_t mask, void *data)
{
  UNUSED(fd);
  UNUSED(mask);
  struct zwl_compositor *compositor = data;

  while (zsurface_prepare_read(compositor->zsurface) == -1) {
    if (errno != EAGAIN) {
      wl_resource_post_error(compositor->resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
                             "failed to prepare to read zsurface event");
      return -1;
    }
    if (zsurface_dispatch_pending(compositor->zsurface) == -1) {
      wl_resource_post_error(compositor->resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
                             "failed to dispatch zsurface pending event");
      return -1;
    }
  }

  while (zsurface_flush(compositor->zsurface) == -1) {
    if (errno != EAGAIN) {
      zsurface_cancel_read(compositor->zsurface);
      wl_resource_post_error(compositor->resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
                             "failed to flush zsurface request");
      return -1;
    }
  }

  if (zsurface_read_events(compositor->zsurface) == -1) {
    wl_resource_post_error(compositor->resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
                           "failed to read zsurface event");
    return -1;
  }

  if (zsurface_dispatch_pending(compositor->zsurface) == -1) {
    wl_resource_post_error(compositor->resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
                           "failed to dispatch zsurface pending event");
    return -1;
  }

  return 0;
}

static const struct zsurface_interface zsurface_interface = {
    .seat_capability = NULL,
    .pointer_enter = NULL,
    .pointer_leave = NULL,
    .pointer_motion = NULL,
};

struct zwl_compositor *zwl_compositor_create(struct wl_client *client, uint32_t version, uint32_t id,
                                             struct zwl_compositor_global *compositor_global)
{
  UNUSED(compositor_global);
  struct zwl_compositor *compositor;
  struct wl_resource *resource;
  struct wl_event_loop *loop;
  int fd;

  compositor = zalloc(sizeof *compositor);
  if (compositor == NULL) {
    wl_client_post_no_memory(client);
    goto out;
  }

  compositor->zsurface = zsurface_create("z11-0", compositor, &zsurface_interface);
  if (compositor->zsurface == NULL) {
    wl_client_post_no_memory(client);
    goto out_compositor;
  }

  if (zsurface_check_globals(compositor->zsurface) != 0) {
    wl_client_post_implementation_error(client, "the compositor does not support the needed protocols");
    goto out_zsurface;
  }

  resource = wl_resource_create(client, &wl_compositor_interface, version, id);
  if (resource == NULL) {
    wl_client_post_no_memory(client);
    goto out_zsurface;
  }

  wl_resource_set_implementation(resource, &zwl_compositor_interface, compositor,
                                 zwl_compositor_handle_destroy);

  compositor->resource = resource;
  compositor->display = compositor_global->display;

  loop = wl_display_get_event_loop(compositor->display);
  fd = zsurface_get_fd(compositor->zsurface);

  compositor->event_source =
      wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, zwl_compositor_zsurface_dispatch, compositor);

  compositor->global_flush_listener.notify = zwl_compositor_global_flush_handler;
  wl_signal_add(&compositor_global->flush_signal, &compositor->global_flush_listener);

  wl_signal_init(&compositor->destroy_signal);

  return compositor;

out_zsurface:
  zsurface_destroy(compositor->zsurface);

out_compositor:
  free(compositor);

out:
  return NULL;
}

static void zwl_compositor_destroy(struct zwl_compositor *compositor)
{
  wl_event_source_remove(compositor->event_source);
  wl_signal_emit(&compositor->destroy_signal, compositor);
  wl_list_remove(&compositor->global_flush_listener.link);
  zsurface_destroy(compositor->zsurface);
  free(compositor);
}
