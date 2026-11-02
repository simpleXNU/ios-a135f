/* ---- inserted by apply-panfrost-darwin.py: the Mali as OSMesa's screen (iOS on the Galaxy A13) ---- */
#include "frontend/winsys_handle.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "panfrost/pan_public.h"
#include <xf86drm.h>

static int gpu_fd = -1;

/* The GPU's driver, where the kernel has a GPU to drive; NULL: none (OSMESA_SOFTWARE=1 forces none). */
static struct pipe_screen *
osmesa_create_gpu_screen(void)
{
   static struct pipe_screen_config config;
   struct pipe_screen *screen;
   int fd;

   if (debug_get_bool_option("OSMESA_SOFTWARE", false))
      return NULL;
   fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return NULL;
   screen = panfrost_create_screen(fd, &config, NULL);
   if (screen)
      gpu_fd = fd;
   return screen ? debug_screen_wrap(screen) : NULL;
}

struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);

/* An IOSurface's memory as a texture: the kernel makes a buffer of it (A135FMali, selector 0x7e). */
struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface)
{
   struct a135f_drm_surface s = { .surface = surface };
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,
   };
   struct winsys_handle handle = { .type = WINSYS_HANDLE_TYPE_FD };

   if (gpu_fd < 0 || a135f_drm_surface(gpu_fd, &s) != 0)
      return NULL;
   if (s.pixel_format != 0x42475241 /* 'BGRA' */ || s.planes != 0)
      return NULL;
   templ.width0 = s.width;
   templ.height0 = s.height;
   handle.handle = surface;          /* arrives as Linux's buffers do, by "file descriptor" (a135f.drm.c) */
   handle.stride = s.row_bytes;
   handle.offset = s.offset;
   handle.size = s.size;
   return screen->resource_from_handle(screen, &templ, &handle, 0);
}
/* ---- end of the inserted part ---- */
