#define TB_IMPL
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <mpv/client.h>

#include "termbox2.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define MPV_FORMAT_OSD_STRING 2

#define ENTER 13
#define ESC 27
#define BACKSPACE 127

enum state { NORMAL, SEARCH, EDIT_N, EDIT_U };

struct player {
	mpv_handle *ctx;
	int vol;
	int muted;
	int paused;
	char cmd[2048];
	char msg[256];
	char *cur_station;
};

struct station {
	char *name;
	char *url;
};

struct station_list {
	struct station **stations;
	size_t index;
	size_t pg_i;
	size_t size;
	size_t a_size;
	size_t *sel;
	enum state state;
	char *path;
};

static size_t strcpy_t(char *, const char *, size_t);
static int station_list_init(struct station_list *, char *);
static int station_list_save(const struct station_list *);
static int station_list_swap(struct station_list *, size_t, size_t);
static int player_init(struct player *);
static int vol_add(struct player *);
static int vol_mute(struct player *);
static int vol_sub(struct player *);
static int stop(struct player *);
static int play(struct player *, struct station);
static int parse_stations(struct station_list *, FILE *);
static int search_f(struct station_list *, const char *);
static int search_r(struct station_list *, const char *);
static int io_read(struct station_list *, struct player *);
static char *strstr_i(const char *, const char*);
static void station_list_render(struct station_list *, struct player *);

size_t
strcpy_t(char *dest, const char *src, size_t size)
{
	size_t i = 0;

	if (size > 0) {
		for (i = 0; i < size - 1 && src[i]; i++) {
			dest[i] = src[i];
		}
		dest[i] = '\0';
	}
	return i;
}

char *
strstr_i(const char *src, const char *tgt)
{
	size_t i;
	int c = tolower((unsigned char)*tgt);

	if (c == '\0') {
		return (char *)src;
	}
	for (; *src; src++) {
		if (tolower((unsigned char)*src) == c) {
			for (i = 1;; i++) {
				if (tgt[i] == '\0') {
					return (char *)src;
				}
				if (tolower((unsigned char)src[i])
				    != tolower((unsigned char)tgt[i])) {
					break;
				}
			}
		}
	}
	return NULL;
}

int
station_list_init(struct station_list *sl, char *path)
{
	sl->pg_i = 0;
	sl->index = 0;
	sl->size = 0;
	sl->state = NORMAL;
	sl->stations = malloc(sizeof(struct station) * 1024);
	sl->a_size = 1024;
	sl->path = path;
	sl->sel = NULL;
	if (!sl->stations) {
		printf("Failed to allocate stations list\n");
		return -1;
	}
	return 0;
}

int
station_list_add(struct station_list *sl, struct station *s)
{
	void *a_tmp;

	if (sl->size + 1 == sl->a_size) {
		a_tmp = realloc(
		    sl->stations,
		    sizeof(struct station)
			* (sl->a_size + 256));
		if (!a_tmp) {
			printf("Failed to resize station list\n");
			return -1;
		}
		sl->stations = a_tmp;
		sl->a_size += 256;
	}
	sl->stations[sl->size++] = s;
	return 0;
}

int
station_list_swap(struct station_list *sl, size_t oi, size_t ni)
{
	struct station *tmp;

	tmp = sl->stations[oi];
	sl->stations[oi] = sl->stations[ni];
	sl->stations[ni] = tmp;
	free(sl->sel);
	sl->sel = NULL;
	return 0;
}

int
station_list_delete(struct station_list *sl, size_t index)
{
	struct station *s = NULL;
	int i;

	if (index >= sl->a_size - 1 || sl->size == 0) {
		return -1;
	}
	s = sl->stations[index];
	for (i = index; i < sl->size - 1; i++) {
		sl->stations[i] = sl->stations[i + 1];
	}
	free(s->name);
	s->name = NULL;
	free(s->url);
	s->url = NULL;
	free(s);
	s = NULL;
	i = sl->size - 1;
	if (sl->stations[i]) {
		sl->stations[i] = NULL;
	}
	if (sl->index == i && sl->index > 0) {
		sl->index -= 1;
	}
	sl->size = i;
	return 0;
}

int
station_list_save(const struct station_list *sl)
{
	FILE *save = NULL;
	size_t i, s;
	char *bpath = NULL;

	s = strlen(sl->path) + 5;
	bpath = malloc(sizeof(char) * s);
	if (!bpath) {
		goto error;
	}
	snprintf(bpath, s, "%s.bak", sl->path);
	save = fopen(bpath, "w+");
	if (!save) {
		printf("Failed to open tmp file: %s\n", bpath);
		goto error;
	}
	for (i = 0; i < sl->size; i++) {
		fprintf(save, "\"%s\" \"%s\"\n", sl->stations[i]->name,
		    sl->stations[i]->url);
	}
	fclose(save);
	save = NULL;
	if (rename(bpath, sl->path) != 0) {
		printf("Failed to replace stations file %s\n", sl->path);
		goto error;
	}
	return 0;
error:
	free(bpath);
	bpath = NULL;
	if (save != NULL) {
		fclose(save);
		save = NULL;
	}
	return -1;
}

int
player_init(struct player *pl)
{
	pl->vol = 100;
	pl->muted = 0;
	pl->paused = 0;
	pl->cur_station = "";
	pl->cmd[0] = '\0';
	pl->msg[0] = '\0';
	pl->ctx = mpv_create();
	if (!pl->ctx) {
		printf("Failed to create mpv context\n");
		return -1;
	}
	if (mpv_initialize(pl->ctx) < 0) {
		printf("Failed to initialize MPV\n");
		return -1;
	}
	return 0;
}

int
vol_add(struct player *pl)
{
	if (pl->vol < 100) {
		if (mpv_command_string(pl->ctx, "add volume 1") < 0) {
			strcpy_t(pl->msg, "mpv api error", sizeof(pl->msg));
			return -1;
		}
		pl->msg[0] = '\0';
		pl->vol += 1;
	}
	return 0;
}

int
vol_mute(struct player *pl)
{
	if (mpv_command_string(pl->ctx, "cycle mute") >= 0) {
		pl->muted = 1 - pl->muted;
		return 0;
	}
	return -1;
}

int
vol_sub(struct player *pl)
{
	if (pl->vol > 0) {
		if (mpv_command_string(pl->ctx, "add volume -1") < 0) {
			strcpy_t(pl->msg, "mpv api error", sizeof(pl->msg));
			return -1;
		}
		pl->msg[0] = '\0';
		pl->vol -= 1;
	}
	return 0;
}

int
stop(struct player *pl)
{
	if (mpv_command_string(pl->ctx, "cycle pause") < 0) {
		strcpy_t(pl->msg, "mpv api error", sizeof(pl->msg));
		return -1;
	}
	pl->paused = 1 - pl->paused;
	return 0;
}

int
play(struct player *pl, struct station s)
{
	const char *cmd[] = {"loadfile", s.url, NULL};

	if (mpv_command(pl->ctx, cmd) < 0) {
		strcpy_t(pl->msg, "Failed to play URL", sizeof(pl->msg));
		return -1;
	}
	pl->msg[0] = '\0';
	pl->cur_station = s.name;
	return 0;
}

int
parse_stations(struct station_list *sl, FILE *stream)
{
	struct station *s = NULL;
	size_t i = 0, len, fbuf_sz, fbuf_i = 0, buf_sz = 2048;
	bool quote_open = false, step = false, esc = false;
	char *fbuf, *buf = malloc(sizeof(char) * 2048), tmp;
	void *a_tmp;

	if (!buf) {
		printf("Failed to allocate parse buffer\n");
		return -1;
	}

	if (fseek(stream, 0L, SEEK_END) != 0) {
		goto error;
	}
	fbuf_sz = ftell(stream);
	if (fbuf_sz == -1) {
		goto error;
	}
	if (fseek(stream, 0L, SEEK_SET) != 0) {
		goto error;
	}
	fbuf = malloc(sizeof(char) * (fbuf_sz + 1));
	len = fread(fbuf, sizeof(char), fbuf_sz, stream);
	if (ferror(stream) != 0) {
		goto error;
	}
	fbuf[len] = '\0';

	for (tmp = fbuf[fbuf_i]; fbuf_i < len; tmp = fbuf[++fbuf_i]) {
		if (!quote_open) {
			if (tmp != ' ' && tmp != '\"' && tmp != '\n') {
				printf("Missing quote\n");
				goto error;
			} else if (tmp == '\"') {
				quote_open = true;
				if (step == false) {
					s = malloc(sizeof(struct station));
					if (!s) {
						printf("Allocation failed\n");
						goto error;
					}
					s->name = NULL;
					s->url = NULL;
				}
			}
		} else {
			switch (tmp) {
			case '\n':
				printf("Unexpected end of line\n");
				goto error;
				break;
			case '\\':
				esc = true;
				break;
			case '\"':
				if (esc) {
					esc = false;
					goto esc;
				}
				quote_open = false;
				if (i == buf_sz) {
					a_tmp = realloc(buf, buf_sz + 1024);
					if (a_tmp != NULL) {
						buf = a_tmp;
						buf_sz += 1024;
					} else {
						goto error;
					}
				}
				buf[i++] = '\0';
				if (step == false) {
					s->name = strdup(buf);
					step = true;
				} else {
					s->url = strdup(buf);
					station_list_add(sl, s);
					step = false;
				}
				i = 0;
				memset(buf, 0, buf_sz);
				break;
			default:
esc:
				if (i < buf_sz) {
					buf[i++] = tmp;
				} else {
					a_tmp = realloc(buf, buf_sz + 1024);
					if (a_tmp != NULL) {
						buf = a_tmp;
						buf_sz += 1024;
					}
					goto error;
				}
				break;
			}
		}
	}
	free(buf);
	buf = NULL;
	return 0;
error:
	printf("Failed to read stations file\n");
	fclose(stream);
	free(buf);
	buf = NULL;
	free(s->name);
	s->name = NULL;
	free(s->url);
	s->url = NULL;
	free(s);
	s = NULL;
	return -1;
}

int
search_f(struct station_list *sl, const char *cmd)
{
	size_t i;
	int h = tb_height();

	if (sl->index + 1 < h - 1) {
		for (i = sl->index + 1; i < sl->size; i++) {
			if (strstr_i(sl->stations[i]->name, cmd) != NULL) {
				sl->index = i;
				if (i > (sl->pg_i + (h - 1))) {
					sl->pg_i = i;
				}
				break;
			}
		}
	}
	return 0;
}

int
search_r(struct station_list *sl, const char *cmd)
{
	size_t i;

	if (sl->index == 0) {
		return 0;
	}
	for (i = sl->index - 1;; i--) {
		if (strstr_i(sl->stations[i]->name, cmd) != NULL) {
			sl->index = i;
			if (i < sl->pg_i) {
				sl->pg_i = i;
			}
			break;
		}
		if (i == 0) {
			break;
		}
	}
	return 0;
}

void
station_list_render(struct station_list *sl, struct player *pl)
{
	size_t i, j, l, r_w;
	int c = 0, h = tb_height(), w = tb_width();
	char *title = NULL;
	char bar[w];
	char muted[4];
	char playing[8];

	tb_clear();

	if (sl->index > sl->pg_i) {
		if (sl->index - sl->pg_i > (h - 2)) {
			if ((h - 2) < sl->index) {
				sl->pg_i = sl->index - (h - 2);
			} else {
				sl->pg_i = 0;
			}
		}
	}
	if (sl->index < sl->pg_i) {
		sl->pg_i -= 1;
	}

	mpv_get_property(pl->ctx, "media-title", MPV_FORMAT_OSD_STRING,
	    &title);
	strcpy_t(muted, pl->muted ? "(m)" : "", sizeof(muted));
	strcpy_t(playing, pl->paused ? "Paused" : "Playing", sizeof(playing));

	/* fix later */
	snprintf(bar, sizeof(bar), "Vol: %d%s | %s: %s | %s | %s %s",
	    pl->vol, muted, playing, pl->cur_station, title, pl->msg, pl->cmd);
	tb_print(0, h - 1, 0, 3, bar);

	l = MIN(sl->size, sl->pg_i + h - 1);

	for (i = sl->pg_i; i < l; i++) {
		if (i == sl->index) {
			tb_print_ex(0, c++, 1, 8, &r_w, sl->stations[i]->name);
		} else {
			tb_print_ex(0, c++, 0, 0, &r_w, sl->stations[i]->name);
		}
		for (j = r_w; j < w; j++) {
			tb_set_cell(j, c - 1, ' ', 1, 0);
		}
	}
	/*
	for (i = c; i < h - 1; i++) {
		for (j = 0; j < w; j++) {
			tb_set_cell(j, c, ' ', 1, 0);
		}
	}
	*/
	tb_present();
	if (title != NULL) {
		mpv_free(title);
		title = NULL;
	}
}

int
io_read(struct station_list *sl, struct player *pl)
{
	struct station *s = NULL;
	struct tb_event ev;
	size_t i;
	int h = tb_height();
	char buf[2048];

	tb_peek_event(&ev, 100);
	if (sl->state == NORMAL) {
		switch (ev.ch) {
		case '0':
			vol_add(pl);
			break;
		case '9':
			vol_sub(pl);
			break;
		case 'c':
			stop(pl);
			break;
		case 'a':
			s = malloc(sizeof(struct station));
			if (!s) {
				printf("Allocation failed\n");
				return -1;
			}
			s->name = strdup("<name>");
			s->url = strdup("<url>");
			station_list_add(sl, s);
			sl->index = sl->size - 1;
			pl->cmd[0] = '\0';
			sl->state = EDIT_N;
		case 'e':
			if (!sl->stations[sl->index]) {
				break;
			}
			strcpy_t(pl->cmd, sl->stations[sl->index]->name,
			    sizeof(pl->cmd));
			sl->state = EDIT_N;
			break;
		case 'g':
			sl->index = 0;
			sl->pg_i = 0;
			break;
		case 'G':
			sl->index = sl->size - 1;
			if ((h - 2) < (sl->size - 1)) {
				sl->pg_i = (sl->size - 1) - (h - 2);
			} else {
				sl->pg_i = 0;
			}
			break;
		case 'j':
			if (sl->index < (sl->size - 1)) {
				sl->index++;
			}
			break;
		case 'k':
			if (sl->index > 0) {
				sl->index--;
			}
			break;
		case 'l':
			if (sl->stations[sl->index]) {
				play(pl, *sl->stations[sl->index]);
			}
			break;
		case 'm':
			vol_mute(pl);
			break;
		case 'n':
			search_f(sl, pl->cmd);
			break;
		case 'N':
			search_r(sl, pl->cmd);
			break;
		case 'p':
			if (sl->sel) {
				station_list_swap(sl, *sl->sel, sl->index);
			}
			break;
		case 'q':
			tb_shutdown();
			station_list_save(sl);
			for (i = 0; i < sl->size; i++) {
				free(sl->stations[i]->name);
				free(sl->stations[i]->url);
				free(sl->stations[i]);
			}
			exit(0);
			break;
		case 'R':
			tb_shutdown();
			tb_init();
			break;
		case 'x':
			if (sl->sel) {
				if (*sl->sel == sl->index) {
					free(sl->sel);
					sl->sel = NULL;
				}
			}
			station_list_delete(sl, sl->index);
			break;
		case 'y':
			if (!sl->sel) {
				sl->sel = malloc(sizeof(size_t));
			}
			if (!sl->sel) {
				printf("Failed to allocate sel field\n");
				return -1;
			}
			*sl->sel = sl->index;
			break;
		case '/':
			memset(pl->cmd, '\0', strlen(pl->cmd));
			sl->state = SEARCH;
			break;
		default:
			break;
		}
	} else {
		switch (ev.key) {
		case BACKSPACE:
			i = strlen(pl->cmd);
			if (i > 0) {
				pl->cmd[i - 1] = '\0';
			}
			break;
		case ESC:
			memset(pl->cmd, 0, strlen(pl->cmd));
			sl->state = NORMAL;
			break;
		case ENTER:
			if (sl->state == SEARCH) {
				search_f(sl, pl->cmd);
				sl->state = NORMAL;
			} else {
				if (sl->state == EDIT_N) {
					free(sl->stations[sl->index]->name);
					sl->stations[sl->index]->name =
					    strdup(pl->cmd);
					strcpy_t(pl->cmd,
					    sl->stations[sl->index]->url,
					    sizeof(pl->cmd));
					sl->state = EDIT_U;
				} else {
					free(sl->stations[sl->index]->url);
					sl->stations[sl->index]->url =
					    strdup(pl->cmd);
					pl->cmd[0] = '\0';
					sl->state = NORMAL;
				}
			}
			break;
		default:
			snprintf(buf, sizeof(buf), "%s%c", pl->cmd, ev.ch);
			strcpy_t(pl->cmd, buf, sizeof(pl->cmd));
			break;
		}
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	FILE *stream;
	struct player pl;
	struct station_list sl;
	char path[4096];

	if (!isatty(fileno(stdout))) {
		return 1;
	}
	switch (argc) {
	case 0: /* fallthrough */
		*--argv = ".", ++argc;
		break;
	case 1:
		sprintf(path, getenv("HOME"));
		sprintf(path + strlen(path), "/.config/cradio/stations");
		break;
	case 2:
		strcpy_t(path, argv[1], sizeof(path));
		break;
	default:
		printf("usage: %s [file]\n", argv[0]);
		return 1;
		break;
	}
	stream = fopen(path, "r");
	if (!stream) {
		printf("Failed to open stations list at %s\n", path);
		return -1;
	}
	station_list_init(&sl, path);
	if (player_init(&pl) < 0) {
		printf("Failed to initialize player\n");
		return -1;
	}
	if (parse_stations(&sl, stream) < 0) {
		printf("Failed to read stations\n");
		return -1;
	}
	fclose(stream);
	tb_init();

	for (;;) {
		station_list_render(&sl, &pl);
		io_read(&sl, &pl);
	}
}
