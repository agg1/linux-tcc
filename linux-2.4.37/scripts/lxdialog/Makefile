HOSTCFLAGS += -DLOCALE 
LIBS = -lncurses -lterminfo

ifeq (/usr/include/ncurses/ncurses.h, $(wildcard /usr/include/ncurses/ncurses.h))
        HOSTCFLAGS += -I/usr/include/ncurses -DCURSES_LOC="<ncurses.h>"
else
ifeq (/usr/include/ncurses/curses.h, $(wildcard /usr/include/ncurses/curses.h))
        HOSTCFLAGS += -I/usr/include/ncurses -DCURSES_LOC="<ncurses/curses.h>"
else
ifeq (/usr/include/ncurses.h, $(wildcard /usr/include/ncurses.h))
        HOSTCFLAGS += -DCURSES_LOC="<ncurses.h>"
else
	HOSTCFLAGS += -DCURSES_LOC="<curses.h>"
endif
endif
endif


OBJS = checklist.o menubox.o textbox.o yesno.o inputbox.o \
       util.o lxdialog.o msgbox.o

%.o: %.c
	$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

all: ncurses lxdialog

lxdialog: $(OBJS)
	$(HOSTCC) -static -g -o lxdialog $(OBJS) $(LIBS)

ncurses:
	@echo "main() {}" > lxtemp.c
	@if $(HOSTCC) -lncurses -lterminfo lxtemp.c ; then \
		rm -f lxtemp.c a.out; \
	else \
		rm -f lxtemp.c; \
		echo -e "\007" ;\
		echo ">> Unable to find the Ncurses libraries." ;\
		echo ">>" ;\
		echo ">> You must have Ncurses installed in order" ;\
		echo ">> to use 'make menuconfig'" ;\
		echo ;\
		exit 1 ;\
	fi

clean:
	rm -f core *.o *~ lxdialog
