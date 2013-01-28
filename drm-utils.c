#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

#include <xf86drm.h>

#include "drm-utils.h"

int dumb_bo_create(struct dumb_bo **bop, int fd, unsigned int width,
		   unsigned int height, unsigned int bpp)
{
	struct drm_mode_create_dumb arg;
	struct dumb_bo *bo;
	int err;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return -ENOMEM;

	memset(&arg, 0, sizeof(arg));
	arg.width = width;
	arg.height = height;
	arg.bpp = bpp;

	err = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
	if (err < 0) {
		err = -errno;
		free(bo);
		return err;
	}

	bo->handle = arg.handle;
	bo->size = arg.size;
	bo->pitch = arg.pitch;
	bo->fd = fd;

	*bop = bo;

	return 0;
}

int dumb_bo_destroy(struct dumb_bo *bo)
{
	struct drm_mode_destroy_dumb arg;
	int err;

	if (bo->ptr) {
		munmap(bo->ptr, bo->size);
		bo->ptr = NULL;
	}

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	err = drmIoctl(bo->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
	if (err < 0)
		return -errno;

	free(bo);

	return 0;
}

int dumb_bo_map(struct dumb_bo *bo)
{
	struct drm_mode_map_dumb arg;
	void *map;
	int err;

	if (bo->ptr) {
		bo->map_count++;
		return 0;
	}

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	err = drmIoctl(bo->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
	if (err < 0)
		return -errno;

	map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->fd,
		   arg.offset);
	if (map == MAP_FAILED)
		return -errno;

	bo->ptr = map;

	return 0;
}

int dumb_bo_unmap(struct dumb_bo *bo)
{
	bo->map_count--;

	return 0;
}

int surface_create(struct surface **surfacep, struct screen *screen,
		   unsigned int width, unsigned int height, unsigned int bpp)
{
	struct surface *surface;
	int err;

	surface = calloc(1, sizeof(*surface));
	if (!surface)
		return -ENOMEM;

	surface->screen = screen;
	surface->width = width;
	surface->height = height;
	surface->bpp = bpp;

	err = dumb_bo_create(&surface->bo, screen->fd, width, height, bpp);
	if (err < 0) {
		free(surface);
		return err;
	}

	err = drmModeAddFB(screen->fd, width, height, 24, 32,
			   surface->bo->pitch, surface->bo->handle,
			   &surface->id);
	if (err < 0) {
		dumb_bo_destroy(surface->bo);
		free(surface);
		return -errno;
	}

	*surfacep = surface;

	return 0;
}

int surface_destroy(struct surface *surface)
{
	if (!surface)
		return -EINVAL;

	dumb_bo_destroy(surface->bo);
	free(surface);

	return 0;
}

int surface_lock(struct surface *surface, void **ptr)
{
	int err;

	if (!surface || !ptr)
		return -EINVAL;

	err = dumb_bo_map(surface->bo);
	if (err < 0)
		return err;

	*ptr = surface->bo->ptr;

	return 0;
}

int surface_unlock(struct surface *surface)
{
	int err;

	if (!surface)
		return -EINVAL;

	err = dumb_bo_unmap(surface->bo);
	if (err < 0)
		return err;

	return 0;
}

static int screen_choose_output(struct screen *screen)
{
	int ret = -ENODEV;
	drmModeRes *res;
	uint32_t i;

	if (!screen)
		return -EINVAL;

	res = drmModeGetResources(screen->fd);
	if (!res)
		return -ENODEV;

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *connector;
		drmModeEncoder *encoder;

		connector = drmModeGetConnector(screen->fd, res->connectors[i]);
		if (!connector)
			continue;

		if (connector->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(connector);
			continue;
		}

		encoder = drmModeGetEncoder(screen->fd, connector->encoder_id);
		if (!encoder) {
			drmModeFreeConnector(connector);
			continue;
		}

		screen->connector = res->connectors[i];
		screen->mode = connector->modes[0];
		screen->crtc = encoder->crtc_id;

		drmModeFreeEncoder(encoder);
		drmModeFreeConnector(connector);
		ret = 0;
		break;
	}

	for (i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *crtc;

		crtc = drmModeGetCrtc(screen->fd, res->crtcs[i]);
		if (!crtc)
			continue;

		if (crtc->crtc_id == screen->crtc) {
			drmModeFreeCrtc(crtc);
			screen->pipe = i;
			break;
		}

		drmModeFreeCrtc(crtc);
	}

	drmModeFreeResources(res);
	return ret;
}

int screen_create(struct screen **screenp, int fd, unsigned int width,
		  unsigned int height)
{
	struct screen *screen;
	unsigned int i;
	int err;

	err = drmSetMaster(fd);
	if (err < 0)
		return -errno;

	screen = calloc(1, sizeof(*screen));
	if (!screen)
		return -ENOMEM;

	screen->fd = fd;

	err = screen_choose_output(screen);
	if (err < 0)
		return err;

	screen->original_crtc = drmModeGetCrtc(screen->fd, screen->crtc);

	if (!width || !height) {
		screen->width = screen->mode.hdisplay;
		screen->height = screen->mode.vdisplay;
	} else {
		screen->width = width;
		screen->height = height;
	}

	for (i = 0; i < 2; i++) {
		err = surface_create(&screen->fb[i], screen, screen->width,
				     screen->height, 32);
		if (err < 0) {
			fprintf(stderr, "surface_create() failed: %d\n", err);
			return err;
		}
	}

	*screenp = screen;

	return 0;
}

int screen_free(struct screen *screen)
{
	drmModeCrtcPtr crtc;
	unsigned int i;

	if (!screen)
		return -EINVAL;

	crtc = screen->original_crtc;
	drmModeSetCrtc(screen->fd, crtc->crtc_id, crtc->buffer_id, crtc->x,
		       crtc->y, &screen->connector, 1, &crtc->mode);
	drmModeFreeCrtc(crtc);

	for (i = 0; i < 2; i++)
		surface_destroy(screen->fb[i]);

	drmDropMaster(screen->fd);
	free(screen);

	return 0;
}

int screen_swap(struct screen *screen)
{
	struct surface *fb = screen->fb[screen->current];
	int err;

	if (!screen)
		return -EINVAL;

	err = drmModeSetCrtc(screen->fd, screen->crtc, fb->id, 0, 0,
			     &screen->connector, 1, &screen->mode);
	if (err < 0)
		return -errno;

	screen->current ^= 1;

	return 0;
}

int screen_flip(struct screen *screen)
{
	struct surface *fb = screen->fb[screen->current];
	int err;

	if (!screen)
		return -EINVAL;

	err = drmModePageFlip(screen->fd, screen->crtc, fb->id,
			      DRM_MODE_PAGE_FLIP_EVENT, screen);
	if (err < 0)
		return -errno;

	screen->current ^= 1;

	return 0;
}
