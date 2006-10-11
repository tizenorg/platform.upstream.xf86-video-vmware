## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = dist-bzip2

bin_PROGRAMS = vmwarectrl
vmwarectrl_SOURCES = vmwarectrl.c libvmwarectrl.c libvmwarectrl.h
vmwarectrl_CPPFLAGS = @X11_CFLAGS@ -I../src/
vmwarectrl_LDADD = @X11_LIBS@
