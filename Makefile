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

include  $(NAVISERVER)/include/Makefile.module

