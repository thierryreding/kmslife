#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "drm-utils.h"

static const char DEFAULT_DEVICE[] = "/dev/dri/card0";

#define KMSLIFE_SCALE 2

struct grid {
	unsigned int width;
	unsigned int pitch;
	unsigned int height;

	void *parents;
	void *cells;
};

#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define ALIGN(x, a) ALIGN_MASK(x, (typeof(x))(a) - 1)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define BIT(x) (1 << (x))

static inline unsigned int wrap(int i, unsigned int max)
{
	if (i < 0)
		return i + max;

	if (i >= max)
		return i - max;

	return i;
}

static struct grid *grid_new(unsigned int width, unsigned int height)
{
	unsigned int pitch = DIV_ROUND_UP(width, 8);
	size_t size = pitch * height;
	struct grid *grid;

	printf("grid: (%u, %u): %u, %zu\n", width, height, pitch, size);

	grid = calloc(1, sizeof(*grid));
	if (!grid)
		return NULL;

	grid->width = width;
	grid->pitch = pitch;
	grid->height = height;

	grid->cells = calloc(1, size);
	if (!grid->cells) {
		free(grid);
		return NULL;
	}

	grid->parents = calloc(1, size);
	if (!grid->parents) {
		free(grid->cells);
		free(grid);
		return NULL;
	}

	return grid;
}

static void grid_free(struct grid *grid)
{
	if (grid) {
		free(grid->parents);
		free(grid->cells);
	}

	free(grid);
}

static inline loff_t grid_offset(struct grid *grid, unsigned int x,
				 unsigned int y)
{
	return y * grid->pitch + (x / 8);
}

static inline loff_t grid_row_offset(struct grid *grid, unsigned int y)
{
	return y * grid->pitch;
}

static void grid_tick_cell(struct grid *grid, unsigned int x, unsigned int y)
{
	unsigned int k, l, count = 0;
	uint8_t *p, *c;
	int i, j;

	for (j = -1; j <= 1; j++) {
		l = wrap(y + j, grid->height);

		p = grid->parents + grid_row_offset(grid, l);

		for (i = -1; i <= 1; i++) {
			if (i == 0 && j == 0)
				continue;

			k = wrap(x + i, grid->width);

			if (p[k / 8] & BIT(k % 8))
				count++;
		}
	}

	p = grid->parents + grid_offset(grid, x, y);
	c = grid->cells + grid_offset(grid, x, y);

	if (*p & BIT(x % 8)) {
		/* cell stays alive */
		if (count == 2 || count == 3)
			*c |= BIT(x % 8);
	} else {
		/* cell is born */
		if (count == 3)
			*c |= BIT(x % 8);
	}
}

static void grid_tick(struct grid *grid)
{
	unsigned int x, y;

	memset(grid->cells, 0, grid->pitch * grid->height);

	for (y = 0; y < grid->height; y++)
		for (x = 0; x < grid->width; x++)
			grid_tick_cell(grid, x, y);
}

static void grid_draw(struct grid *grid, struct screen *screen)
{
	struct surface *fb = screen->fb[screen->current];
	unsigned int x, y;
	void *surface;
	int err;

	err = surface_lock(fb, &surface);
	if (err < 0) {
		fprintf(stderr, "surface_lock() failed\n");
		return;
	}

	for (y = 0; y < grid->height; y++) {
		uint8_t *cells = grid->cells + grid_row_offset(grid, y);
		uint32_t *ptr[KMSLIFE_SCALE];
		unsigned int i, j;

		for (i = 0; i < KMSLIFE_SCALE; i++)
			ptr[i] = surface + (y * KMSLIFE_SCALE + i) *
				 fb->bo->pitch;

		for (x = 0; x < grid->width; x++) {
			uint32_t color;

			if (cells[x / 8] & BIT(x % 8))
				color = 0xffffffff;
			else
				color = 0x00000000;

			for (j = 0; j < KMSLIFE_SCALE; j++)
				for (i = 0; i < KMSLIFE_SCALE; i++)
					ptr[j][x * KMSLIFE_SCALE + i] = color;
		}
	}

	surface_unlock(fb);
}

static void grid_swap(struct grid *grid)
{
	void *tmp = grid->parents;
	grid->parents = grid->cells;
	grid->cells = tmp;
}

static void grid_add_cell(struct grid *grid, unsigned int x, unsigned int y)
{
	uint8_t *p = grid->parents + grid_offset(grid, x, y);

	*p |= BIT(x % 8);
}

static void grid_randomize(struct grid *grid, unsigned int seed)
{
	unsigned int x, y;

	for (y = 0; y < grid->height; y++) {
		for (x = 0; x < grid->width; x++) {
			bool alive = rand_r(&seed) > RAND_MAX / 2;
			if (alive)
				grid_add_cell(grid, x, y);
		}
	}
}

static void grid_add_glider(struct grid *grid, unsigned int x, unsigned int y)
{
	grid_add_cell(grid, x + 1, y + 0);
	grid_add_cell(grid, x + 2, y + 1);
	grid_add_cell(grid, x + 0, y + 2);
	grid_add_cell(grid, x + 1, y + 2);
	grid_add_cell(grid, x + 2, y + 2);
}

static void grid_add_pentomino(struct grid *grid, unsigned int x,
			       unsigned int y)
{
	grid_add_cell(grid, x + 1, y + 0);
	grid_add_cell(grid, x + 2, y + 0);
	grid_add_cell(grid, x + 0, y + 1);
	grid_add_cell(grid, x + 1, y + 1);
	grid_add_cell(grid, x + 1, y + 2);
}

static void grid_add_diehard(struct grid *grid, unsigned int x,
			     unsigned int y)
{
	grid_add_cell(grid, x + 6, y + 0);
	grid_add_cell(grid, x + 0, y + 1);
	grid_add_cell(grid, x + 1, y + 1);
	grid_add_cell(grid, x + 1, y + 2);
	grid_add_cell(grid, x + 5, y + 2);
	grid_add_cell(grid, x + 6, y + 2);
	grid_add_cell(grid, x + 7, y + 2);
}

static bool done = false;

static void signal_handler(int signum)
{
	if (signum == SIGINT)
		done = true;
}

static void usage(FILE *fp, const char *program)
{
	fprintf(fp, "usage: %s [options] DEVICE\n", program);
	fprintf(fp, "\n");
	fprintf(fp, "options:\n");
	fprintf(fp, "  -d, --die-hard	start with die-hard element\n");
	fprintf(fp, "  -g, --glider	start with glider element\n");
	fprintf(fp, "  -h, --help	display this help screen and exit\n");
	fprintf(fp, "  -p, --pentomino	start with r-pentomino element\n");
	fprintf(fp, "  -s, --seed	initial random seed\n");
	fprintf(fp, "\n");
}

enum pattern {
	RANDOM,
	DIE_HARD,
	GLIDER,
	PENTOMINO,
};

int main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "die-hard", 0, NULL, 'd' },
		{ "glider", 0, NULL, 'g' },
		{ "help", 0, NULL, 'h' },
		{ "pentomino", 0, NULL, 'p' },
		{ "seed", 1, NULL, 's' },
		{ NULL, 0, NULL, 0 },
	};
	unsigned int seed = time(NULL);
	enum pattern pattern = RANDOM;
	struct screen *screen;
	struct sigaction sa;
	const char *device;
	bool help = false;
	struct grid *grid;
	unsigned int gen;
	int fd, err, opt;

	while ((opt = getopt_long(argc, argv, "dghps:", options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			pattern = DIE_HARD;
			break;

		case 'g':
			pattern = GLIDER;
			break;

		case 'h':
			help = true;
			break;

		case 'p':
			pattern = PENTOMINO;
			break;

		case 's':
			seed = strtoul(optarg, NULL, 0);
			break;

		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}

	if (help) {
		usage(stdout, argv[0]);
		return 0;
	}

	if (optind >= argc)
		device = DEFAULT_DEVICE;
	else
		device = argv[optind];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: open() failed: %m\n", device);
		return 1;
	}

	err = screen_create(&screen, fd, 0, 0);
	if (err < 0) {
		fprintf(stderr, "screen_create() failed: %s\n", strerror(-err));
		return 1;
	}

	grid = grid_new(screen->width / KMSLIFE_SCALE, screen->height / KMSLIFE_SCALE);
	if (!grid) {
		fprintf(stderr, "grid_new() failed\n");
		return 1;
	}

	switch (pattern) {
	case RANDOM:
		grid_randomize(grid, seed);
		break;

	case DIE_HARD:
		grid_add_diehard(grid, grid->width / 2, grid->height / 2);
		break;

	case GLIDER:
		grid_add_glider(grid, grid->width / 2, grid->height / 2);
		break;

	case PENTOMINO:
		grid_add_pentomino(grid, grid->width / 2, grid->height / 2);
		break;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);

	while (!done) {
		grid_tick(grid);
		grid_draw(grid, screen);
		screen_swap(screen);
		grid_swap(grid);
		usleep(20000);
		gen++;
	}

	grid_free(grid);

	screen_free(screen);
	drmClose(fd);

	return 0;
}
