#
# Makefile for the linux shfs filesystem routines.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile.

O_TARGET := shfs.o

obj-y    := dcache.o dir.o fcache.o file.o inode.o ioctl.o proc.o shell.o symlink.o

obj-m    := $(O_TARGET)

# If you want debugging output, please uncomment the following line
# EXTRA_CFLAGS += -DENABLE_DEBUG

CFLAGS_proc.o += -finline-functions

include $(TOPDIR)/Rules.make
