ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD      =  nsudp.so

#
# Objects to build.
#
OBJS     = nsudp.o

#
# Objects to clean
#
CLEAN   += clean-bak

MODLIBS	 = 
CFLAGS += -I../../nsd

include  $(NAVISERVER)/include/Makefile.module

clean-bak:
	rm -rf *~
