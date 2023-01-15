PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

LIBS = -lmpv
CC = cc
LD = $(CC)
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
CFLAGS = -std=c99 -pedantic -Wall -Wno-deprecated-declarations -Os
LDFLAGS = ${LIBS}
