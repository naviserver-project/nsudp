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
MODOBJS     = nsudp.o

include  $(NAVISERVER)/include/Makefile.module

