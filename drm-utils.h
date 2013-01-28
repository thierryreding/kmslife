#ifndef DRM_UTILS_H
#define DRM_UTILS_H 1

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

struct dumb_bo {
	int fd;
	uint32_t handle;
	uint32_t size;
	void *ptr;
	int map_count;
	uint32_t pitch;
};

int dumb_bo_create(struct dumb_bo **bop, int fd, unsigned int width,
		   unsigned int height, unsigned int bpp);
int dumb_bo_destroy(struct dumb_bo *bo);
int dumb_bo_map(struct dumb_bo *bo);
int dumb_bo_unmap(struct dumb_bo *bo);

struct screen;

struct surface {
	struct screen *screen;
	struct dumb_bo *bo;
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	uint32_t id;
};

int surface_create(struct surface **surfacep, struct screen *screen,
		   unsigned int width, unsigned int height, unsigned int bpp);
int surface_destroy(struct surface *surface);
int surface_lock(struct surface *surface, void **ptr);
int surface_unlock(struct surface *surface);

struct screen {
	drmModeCrtcPtr original_crtc;
	drmModeModeInfo mode;
	uint32_t connector;
	uint32_t crtc;
	unsigned int pipe;
	unsigned int width;
	unsigned int height;
	struct surface *fb[2];
	unsigned int current;
	int fd;
};

int screen_create(struct screen **screenp, int fd, unsigned int width,
		  unsigned int height);
int screen_free(struct screen *screen);
int screen_swap(struct screen *screen);
int screen_flip(struct screen *screen);

#endif /* DRM_UTILS_H */
