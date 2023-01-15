.POSIX:

include config.mk

SRC = cradio.c
OBJ = $(SRC:.c=.o)

all: cradio

cradio: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f cradio ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f cradio ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/cradio
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	cp cradio.1 ${DESTDIR}${MANPREFIX}/man1/cradio.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/cradio.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/cradio\
			${DESTDIR}${MANPREFIX}/man1/cradio.1

.PHONY: all clean install uninstall
