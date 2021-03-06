#include <errno.h>
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

struct grid {
	unsigned int width;
	unsigned int pitch;
	unsigned int height;
	unsigned int scale;

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

static struct grid *grid_new(unsigned int width, unsigned int height,
			     unsigned int scale)
{
	unsigned int pitch = DIV_ROUND_UP(width / scale, 8);
	size_t size = pitch * height / scale;
	struct grid *grid;

	grid = calloc(1, sizeof(*grid));
	if (!grid)
		return NULL;

	grid->width = width / scale;
	grid->pitch = pitch;
	grid->height = height / scale;
	grid->scale = scale;

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
		uint32_t *ptr[grid->scale];
		unsigned int i, j;

		for (i = 0; i < grid->scale; i++)
			ptr[i] = surface + (y * grid->scale + i) *
				 fb->bo->pitch;

		for (x = 0; x < grid->width; x++) {
			uint32_t color;

			if (cells[x / 8] & BIT(x % 8))
				color = 0xffffffff;
			else
				color = 0x00000000;

			for (j = 0; j < grid->scale; j++)
				for (i = 0; i < grid->scale; i++)
					ptr[j][x * grid->scale + i] = color;
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

static void grid_add_gun(struct grid *grid, unsigned int x, unsigned int y)
{
	grid_add_cell(grid, x +  0, y + 0);
	grid_add_cell(grid, x +  1, y + 0);
	grid_add_cell(grid, x +  0, y + 1);
	grid_add_cell(grid, x +  1, y + 1);

	grid_add_cell(grid, x + 10, y + 0);
	grid_add_cell(grid, x + 10, y + 1);
	grid_add_cell(grid, x + 10, y + 2);

	grid_add_cell(grid, x + 11, y - 1);
	grid_add_cell(grid, x + 11, y + 3);

	grid_add_cell(grid, x + 12, y - 2);
	grid_add_cell(grid, x + 12, y + 4);

	grid_add_cell(grid, x + 13, y - 2);
	grid_add_cell(grid, x + 13, y + 4);

	grid_add_cell(grid, x + 14, y + 1);

	grid_add_cell(grid, x + 15, y - 1);
	grid_add_cell(grid, x + 15, y + 3);

	grid_add_cell(grid, x + 16, y + 0);
	grid_add_cell(grid, x + 16, y + 1);
	grid_add_cell(grid, x + 16, y + 2);

	grid_add_cell(grid, x + 17, y + 1);

	grid_add_cell(grid, x + 20, y + 0);
	grid_add_cell(grid, x + 20, y - 1);
	grid_add_cell(grid, x + 20, y - 2);

	grid_add_cell(grid, x + 21, y + 0);
	grid_add_cell(grid, x + 21, y - 1);
	grid_add_cell(grid, x + 21, y - 2);

	grid_add_cell(grid, x + 22, y - 3);
	grid_add_cell(grid, x + 22, y + 1);

	grid_add_cell(grid, x + 24, y - 4);
	grid_add_cell(grid, x + 24, y - 3);
	grid_add_cell(grid, x + 24, y + 1);
	grid_add_cell(grid, x + 24, y + 2);

	grid_add_cell(grid, x + 34, y - 2);
	grid_add_cell(grid, x + 34, y - 1);

	grid_add_cell(grid, x + 35, y - 2);
	grid_add_cell(grid, x + 35, y - 1);
}

static void grid_add_acorn(struct grid *grid, unsigned int x, unsigned int y)
{
	grid_add_cell(grid, x + 0, y + 0);
	grid_add_cell(grid, x + 1, y + 0);
	grid_add_cell(grid, x + 1, y - 2);
	grid_add_cell(grid, x + 3, y - 1);
	grid_add_cell(grid, x + 4, y + 0);
	grid_add_cell(grid, x + 5, y + 0);
	grid_add_cell(grid, x + 6, y + 0);
}

static int grid_load_rle(struct grid *grid, const char *filename,
			 unsigned int x, unsigned int y)
{
	unsigned int width = 0, height = 0, s = 0, t = 0;
	char *line = NULL, *rule = NULL, *end, *ptr;
	unsigned long count;
	size_t len = 0, i;
	ssize_t num;
	FILE *fp;

	fp = fopen(filename, "r");
	if (!fp)
		return -errno;

	while ((num = getline(&line, &len, fp)) != -1) {
		if (line[0] == '#') {
			printf("comment: %s", line);
			continue;
		}

		if (line[0] == 'x') {
			sscanf(line, "x = %u, y = %u, rule = %ms", &width,
			       &height, &rule);
			printf("size: %ux%u\n", width, height);
			printf("rule: %s\n", rule);
			free(rule);
			continue;
		}

		line[num - 1] = '\0';
		ptr = line;
		end = NULL;

		while (*ptr) {
			printf("| %s\n", ptr);

			count = strtoul(ptr, &end, 10);
			if (end == ptr)
				count = 1;

			ptr = end;

			switch (*ptr) {
			case 'o':
				for (i = 0; i < count; i++)
					grid_add_cell(grid, x + s + i, y + t);

				s += count;
				break;

			case 'b':
				s += count;
				break;

			case '$':
				t += count;
				s = 0;
				break;
			}

			ptr++;
		}
	}

	fclose(fp);
	return 0;
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
	fprintf(fp, "  -a, --acorn	start with acorn element\n");
	fprintf(fp, "  -d, --die-hard	start with die-hard element\n");
	fprintf(fp, "  -f, --framerate	set framerate\n");
	fprintf(fp, "  -F, --file	start with element from file\n");
	fprintf(fp, "  -g, --glider	start with glider element\n");
	fprintf(fp, "  -G, --gun	start with glider gun\n");
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
	GUN,
	ACORN,
};

int main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "acorn", 0, NULL, 'a' },
		{ "die-hard", 0, NULL, 'd' },
		{ "framerate", 1, NULL, 'f' },
		{ "file", 1, NULL, 'F' },
		{ "glider", 0, NULL, 'g' },
		{ "gun", 0, NULL, 'G' },
		{ "help", 0, NULL, 'h' },
		{ "pentomino", 0, NULL, 'p' },
		{ "seed", 1, NULL, 's' },
		{ "scale", 1, NULL, 'S' },
		{ NULL, 0, NULL, 0 },
	};
	static const char opts[] = "adf:F:gGhps:S:";
	unsigned int seed = time(NULL);
	enum pattern pattern = RANDOM;
	unsigned int gen, scale = 1;
	unsigned int framerate = 60;
	const char *filename = NULL;
	struct screen *screen;
	struct sigaction sa;
	const char *device;
	bool help = false;
	struct grid *grid;
	unsigned int x, y;
	int fd, err, opt;

	while ((opt = getopt_long(argc, argv, opts, options, NULL)) != -1) {
		switch (opt) {
		case 'a':
			pattern = ACORN;
			break;

		case 'd':
			pattern = DIE_HARD;
			break;

		case 'f':
			framerate = strtoul(optarg, NULL, 0);
			break;

		case 'F':
			filename = optarg;
			break;

		case 'g':
			pattern = GLIDER;
			break;

		case 'G':
			pattern = GUN;
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

		case 'S':
			scale = strtoul(optarg, NULL, 0);
			if (!scale) {
				fprintf(stderr, "invalid scale: %s\n", optarg);
				return 1;
			}
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

	grid = grid_new(screen->width, screen->height, scale);
	if (!grid) {
		fprintf(stderr, "grid_new() failed\n");
		return 1;
	}

	x = grid->width / 2;
	y = grid->height / 2;

	if (filename) {
		err = grid_load_rle(grid, filename, x, y);
		if (err < 0) {
			fprintf(stderr, "grid_load_rle() failed: %d\n", err);
			return 1;
		}
	} else {
		switch (pattern) {
		case RANDOM:
			grid_randomize(grid, seed);
			break;

		case DIE_HARD:
			grid_add_diehard(grid, x, y);
			break;

		case GLIDER:
			grid_add_glider(grid, x, y);
			break;

		case PENTOMINO:
			grid_add_pentomino(grid, x, y);
			break;

		case GUN:
			grid_add_gun(grid, x, y);
			break;

		case ACORN:
			grid_add_acorn(grid, x, y);
			break;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);

	while (!done) {
		if (framerate > 0)
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
