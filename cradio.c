#define TB_IMPL
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpv/client.h>

#include "termbox.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define ENTER 13
#define ESC 27
#define BACKSPACE 127

struct player {
	mpv_handle *ctx;
	char cmd[256];
	char msg[256];
	char *cur_station;
	int vol;
	int muted;
	int paused;
};

struct station {
	char *name;
	char *url;
};

struct station_list {
	struct station stations[1024];
	int pg_i;
	int index;
	int searching;
	int size;
};

static char *strcpy_t(char *, const char *, size_t);
static char *strstr_i(const char *, const char*);
static int init_station_list(struct station_list *);
static int init_player(struct player *);
static int add_vol(struct player *);
static int mute_vol(struct player *);
static int sub_vol(struct player *);
static int stop(struct player *);
static int play(struct player *, struct station);
static int parse_stations(struct station_list *);
static int search_f(struct station_list *, char *);
static int search_r(struct station_list *, char *);
static void render_stations(struct station_list *, struct player *);
static void read_io(struct station_list *, struct player *);

char *
strcpy_t(char *dest, const char *src, size_t size)
{
	size_t i;

	if (size > 0) {
		for (i = 0; i < size - 1 && src[i]; i++) {
			dest[i] = src[i];
		}
		dest[i] = '\0';
	}
	return dest;
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
init_station_list(struct station_list *sl)
{
	sl->pg_i = 0;
	sl->index = 0;
	sl->size = 0;
	sl->searching = 0;
	return 0;
}

int
init_player(struct player *pl)
{
	pl->vol = 100;
	pl->muted = 0;
	pl->cur_station = "";
	pl->ctx = mpv_create();
	strcpy_t(pl->msg, "", sizeof(pl->msg));
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
add_vol(struct player *pl)
{
	if (pl->vol < 100) {
		if (mpv_command_string(pl->ctx, "add volume 1") >= 0) {
			strcpy_t(pl->msg, "", sizeof(pl->msg));
			pl->vol += 1;
		} else {
			strcpy_t(pl->msg, "mpv api error", sizeof(pl->msg));
			return -1;
		}
	}
	return 0;
}

int
mute_vol(struct player *pl)
{
	if (mpv_command_string(pl->ctx, "cycle mute") >= 0) {
		pl->muted = 1 - pl->muted;
		return 0;
	}
	return -1;
}

int
sub_vol(struct player *pl)
{
	if (pl->vol > 0) {
		if (mpv_command_string(pl->ctx, "add volume -1") >= 0) {
			strcpy_t(pl->msg, "", sizeof(pl->msg));
			pl->vol -= 1;
		} else {
			strcpy_t(pl->msg, "mpv api error", sizeof(pl->msg));
			return -1;
		}
	}
	return 0;
}

int
stop(struct player *pl)
{
	if (mpv_command_string(pl->ctx, "cycle pause") >= 0) {
		pl->paused = 1 - pl->paused;
		return 0;
	}
	return -1;
}

int
play(struct player *pl, struct station s)
{
	const char *cmd[] = {"loadfile", s.url, NULL};

	if (mpv_command(pl->ctx, cmd) >= 0) {
		strcpy_t(pl->msg, "", sizeof(pl->msg));
		pl->cur_station = s.name;
	} else {
		strcpy_t(pl->msg, "Failed to play URL", sizeof(pl->msg));
		return -1;
	}
	return 0;
}

int
parse_stations(struct station_list *sl)
{
	struct station *s = NULL;
	FILE *stream;
	char path[4096];
	char buf[1024];
	int i = 0, quote_open = 0, step = 0, tmp;

	sprintf(path, getenv("HOME"));
	sprintf(path + strlen(path), "/.config/cradio/stations");
	stream = fopen(path, "r");

	if (!stream) {
		printf("Failed to open stations list at %s\n", path);
		return -1;
	}
	for (tmp = getc(stream); tmp != EOF; tmp = getc(stream)) {
		if (!quote_open) {
			if (tmp != ' ' && tmp != '\"' && tmp != '\n') {
				printf("Missing quote\n");
				goto error;
			} else if (tmp == '\"') {
				quote_open = 1;
				if (step == 0) {
					s = malloc(sizeof(struct station));
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
			case '\"':
				quote_open = 0;
				if (i < 1024) {
					buf[i++] = '\0';
				}
				if (step == 0) {
					s->name = strdup(buf);
					step = 1;
				} else {
					s->url = strdup(buf);
					sl->stations[sl->size++] = *s;
					step = 0;
				}
				i = 0;
				memset(buf, 0, sizeof(buf));
				break;
			default:
				if (i < 1024) {
					buf[i++] = tmp;
				} else {
					printf("Exceeded buffer\n");
					goto error;
				}
				break;
			}
		}
	}
	return 0;
error:
	printf("Failed to read stations file\n");
	fclose(stream);
	if (s->name) {
		free(s->name);
		s->name = NULL;
	}
	if (s->url) {
		free(s->url);
		s->url = NULL;
	}
	if (s) {
		free(s);
		s = NULL;
	}
	return -1;
}

int
search_f(struct station_list *sl, char *cmd)
{
	int i;

	if (sl->index + 1 < tb_height() - 1) {
		for (i = sl->index + 1; i < sl->size; i++) {
			if (strstr_i(sl->stations[i].name, cmd) != NULL) {
				sl->index = i;
				if (i > (sl->pg_i + (tb_height() - 1))) {
					sl->pg_i = i;
				}
				break;
			}
		}
	}
	return 0;
}

int
search_r(struct station_list *sl, char *cmd)
{
	int i;

	if (sl->index - 1 > 0) {
		for (i = sl->index - 1; i > 0; i--) {
			if (strstr_i(sl->stations[i].name, cmd) != NULL) {
				sl->index = i;
				if (i < sl->pg_i) {
					sl->pg_i = i;
				}
				break;
			}
		}
	}
	return 0;
}

void
render_stations(struct station_list *sl, struct player *pl)
{
	int i, c = 0, l;
	char *title = NULL;
	char muted[4];
	char playing[8];
	char stat[1024];

	tb_clear();

	if (sl->index - sl->pg_i > (tb_height() - 2)) {
		sl->pg_i = MAX(0, sl->index - (tb_height() - 2));
	}
	if (sl->index - sl->pg_i <  0) {
		sl->pg_i -= 1;
	}

	title = mpv_get_property_string(pl->ctx, "media-title");
	strcpy_t(muted, pl->muted ? "(m)" : "", sizeof(muted));
	strcpy_t(playing, pl->paused ? "Paused" : "Playing", sizeof(playing));
	snprintf(stat, sizeof(stat), "Vol: %d%s | %s: %s | %s | %s %s",
	    pl->vol, muted, playing, pl->cur_station, title, pl->msg, pl->cmd);
	l = strlen(stat);

	for (i = 0; i < tb_width(); i++) {
		if (i < l) {
			tb_set_cell(i, tb_height() - 1, stat[i], 0, 1);
		} else {
			tb_set_cell(i, tb_height() - 1, ' ', 0, 1);
		}
	}

	l = MIN(sl->size, sl->pg_i + tb_height() - 1);

	for (i = sl->pg_i; i < l; i++) {
		if (i == sl->index) {
			tb_printf(0, c++, 1, 8, "%s", sl->stations[i].name);
		} else {
			tb_printf(0, c++, 0, 0, "%s", sl->stations[i].name);
		}
	}
	tb_present();
	if (title != NULL) {
		mpv_free(title);
		title = NULL;
	}
}

void
read_io(struct station_list *sl, struct player *pl)
{
	struct tb_event ev;
	char buf[256];

	tb_peek_event(&ev, 100);
	if (sl->searching) {
		switch (ev.key) {
		case BACKSPACE:
			pl->cmd[strlen(pl->cmd) - 1] = '\0';
			break;
		case ESC:
			sl->searching = 0;
			memset(pl->cmd, 0, strlen(pl->cmd));
			break;
		case ENTER:
			sl->searching = 0;
			search_f(sl, pl->cmd);
			break;
		default:
			snprintf(buf, sizeof(buf), "%s%c", pl->cmd, ev.ch);
			strcpy_t(pl->cmd, buf, sizeof(pl->cmd));
			break;
		}
	} else {
		switch (ev.ch) {
		case '0':
			add_vol(pl);
			break;
		case '9':
			sub_vol(pl);
			break;
		case 'c':
			stop(pl);
			break;
		case 'g':
			sl->index = 0;
			sl->pg_i = 0;
			break;
		case 'G':
			sl->index = sl->size - 1;
			sl->pg_i = MAX(0, (sl->size - 1) - (tb_height() - 2));
			break;
		case 'j':
			if (sl->index < 99 && sl->index < (sl->size - 1)) {
				sl->index++;
			}
			break;
		case 'k':
			if (sl->index > 0) {
				sl->index--;
			}
			break;
		case 'l':
			play(pl, sl->stations[sl->index]);
			break;
		case 'm':
			mute_vol(pl);
			break;
		case 'n':
			search_f(sl, pl->cmd);
			break;
		case 'N':
			search_r(sl, pl->cmd);
			break;
		case 'q':
			tb_shutdown();
			exit(0);
			break;
		case '/':
			memset(pl->cmd, 0, strlen(pl->cmd));
			sl->searching = 1;
			break;
		default:
			break;
		}
	}
}

int
main()
{
	struct player pl;
	struct station_list sl;

	if (!isatty(fileno(stdout))) {
		exit(1);
	}

	init_station_list(&sl);
	if (init_player(&pl) < 0) {
		exit(1);
	}
	if (parse_stations(&sl) < 0) {
		printf("Failed to read stations\n");
		return -1;
	}
	tb_init();
	for (;;) {
		render_stations(&sl, &pl);
		read_io(&sl, &pl);
	}
}
