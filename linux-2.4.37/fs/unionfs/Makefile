#UNIONFS_VERSION  = 1.0.14
#EXTRACFLAGS+=-DUNIONFS_VERSION=\"${UNIONFS_VERSION}\"
# This will disable debugging support
# EXTRACFLAGS+=-DUNIONFS_NDEBUG

O_TARGET := unionfs.o

obj-y    := subr.o dentry.o locks.o file.o inode.o main.o super.o \
	stale_inode.o branchman.o xattr.o rdstate.o copyup.o  \
	dirhelper.o rename.o unlink.o lookup.o persistent_inode.o \
	commonfops.o dirfops.o print.o

obj-m   := $(O_TARGET)

include $(TOPDIR)/Rules.make
