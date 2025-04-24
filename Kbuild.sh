#!/bin/sh -e

# (c) 2023,2024 aggi
# this script fully replaces the linux-2.4 internal Kbuild system to simplify
# AoT compilation of kernel with tinycc compiler
#
# Original tccboot was introduced by Fabrice https://bellard.org/tcc/tccboot.html
# for JiT compilation of linux-2.4 kernel
#
# AoT compilation of linux-2.4 was inspired by https://github.com/seyko2/tccboot

set -eu
CWD="$(pwd)"
rm -f LOG

prepare_config() {
	echo "### prepare_config"
	cd $CWD/$KDIR/include ; rm -f asm ; ln -sf asm-i386 asm ; cd $CWD

	cp ${KCONF} $CWD/$KDIR/.config

	cd $CWD/$KDIR
	##make ARCH=i386 CC="i386-tcc" LD="i386-tcc" AS="i386-tcc" HOSTCC="tcc" menuconfig
	# autoconf.h and version.h
	make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" oldconfig
	make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" include/linux/version.h
	# slow, use pre-generated compile.h instead
	##make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" dep include/linux/compile.h
}

prepare_loader() {
	echo "### prepare_loader"
	cd $CWD
	rm -rf btmp ; mkdir btmp ; cd btmp

	cp $CWD/$KDIR/arch/i386/boot/bootsect.S .
	$CC -E -P -nostdinc -nostdlib -D__BIG_KERNEL__ -I../$KDIR/include bootsect.S -o bootsect.s
	$REALAS bootsect.s -o bootsect.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o bootsect.tcc bootsect.o
	dd if=bootsect.tcc of=bootsect bs=1 count=512 ; chmod 755 bootsect

	cp $CWD/$KDIR/arch/i386/boot/setup.S .
	cp $CWD/$KDIR/arch/i386/boot/video.S .
	$CC -E -P -I../$KDIR/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ setup.S -o setup.s
	$REALAS setup.s -o setup.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o setup.tcc setup.o
	## that is exactly 5 sectors on disk ?
	#dd if=setup.tcc of=setup bs=1 count=2560 ; chmod 755 setup
	dd if=setup.tcc of=setup bs=1; chmod 755 setup

	# 16bit real-mode routines required for smp init, probably buggy rwlock.h patch
	cp $CWD/$KDIR/arch/i386/kernel/trampoline.S .
	$CC -E -P -I../$KDIR/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ trampoline.S -o trampoline.s
	$REALAS trampoline.s -o trampoline.o

	#
	$HOSTCC -I../$KDIR/include -I/usr/lib/tcc/include ../$KDIR/arch/i386/boot/tools/build.c -static -o build ; chmod 755 build

	cd $CWD
}

compile_kernel() {
	echo "### compile_kernel" | tee -a LOG
	rm -f btmp/vmlinux
	[ -d temp ] && rm -rf temp ; mkdir temp

	for i in $FILE_LIST
	do
		echo $i 2>&1 | tee -a LOG
		dir=$(dirname $i)
		[ ! -d temp/$dir ] && mkdir -p temp/$dir

		case $i in
		*.o)
			cp $i temp/$i
			FILE_LIST_o="$FILE_LIST_o temp/$i"
		;;
		*)
			$CC -c \
			-o temp/$i.o \
			-fno-common -nostdinc -nostdlib \
			-I$KDIR/include \
			-D__KERNEL__ -D__OPTIMIZE__ \
			$i 2>&1 | tee -a LOG

			FILE_LIST_o="$FILE_LIST_o temp/$i.o"
		;;
		esac
	done
}

link_kernel() {
	echo "### link_kernel" | tee -a LOG

	$LD \
	-o btmp/vmlinux \
	-fno-common -nostdinc -nostdlib -static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary \
	$FILE_LIST_o \
	$CCLIB 2>&1 | tee -a LOG
}

compilelink_kernel() {
	echo "### compilelink_kernel" | tee -a LOG
	rm -f vmlinux

	$CC \
	-o btmp/vmlinux \
	-fno-common -nostdinc -nostdlib -static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary  \
	-I$KDIR/include \
	-D__KERNEL__ -D__OPTIMIZE__ \
	$FILE_LIST \
	$CCLIB 2>&1 | tee -a LOG
}

link_kernel_gcc() {
	echo "### link_kernel_gcc" | tee -a LOG

	$LD \
	-o btmp/vmlinux \
	-nostdlib -nodefaultlibs -nostartfiles \
	-static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary \
	-e startup_32 -Tusr/src/linux/arch/i386/vmlinux.lds \
	$FILE_LIST_o \
	$CCLIB 2>&1 | tee -a LOG
}


create_iso() {
	echo "### create_iso"
	rm -f tccboot.iso tccboot-hybrid.iso ; 	rm -rf isoroot ; mkdir isoroot ; mkdir isoroot/boot ; mkdir isoroot/isolinux

	echo "default linux-debug
timeout 10
ontimeout linux-debug
prompt 1

label linux-smp
	kernel /boot/linux
	append earlyprintk video=vesa:mtrr initrd=/boot/initrd ramdisk_size=${INITRD_SIZE} root=/dev/ram0 noacpi no_timer_check

label linux-nosmp
	kernel /boot/linux
	append earlyprintk video=vesa:mtrr initrd=/boot/initrd ramdisk_size=${INITRD_SIZE} root=/dev/ram0 noacpi nosmp

label linux-debug
	kernel /boot/linux
	append debug earlyprintk console=ttyS0,9600 console=tty0 video=vesa:mtrr initrd=/boot/initrd ramdisk_size=${INITRD_SIZE} root=/dev/ram0 noacpi no_timer_check
" > isoroot/isolinux/isolinux.cfg

#label tccboot
#	kernel /boot/tccboot
#	append initrd=/boot/example.romfs root=/dev/ram ramdisk_size=20000 devfs=nomount


	cp /usr/share/syslinux/isolinux.bin isoroot/isolinux/
	cp ${INITRD} isoroot/boot/initrd

	cd $CWD/btmp
	#./build -b ./bootsect ./setup vmlinux CURRENT >../isoroot/boot/linux
	./build -b ./bootsect ./setup vmlinux >../isoroot/boot/linux

	# fiddle together tccargs and usr/src/linux for final FILE_LIST on target
	#genromfs -v -x '.git' -d /media/CACHE/TCC/linux-bellard -f ../isoroot/boot/example.romfs >/dev/null 2>&1

	cd $CWD
	### mkisofs -J -R -l -V LIVECD -modification-date 19700101000000.00 \
	#mkisofs -l -V LIVECD -modification-date 19700101000000.00 -o tccboot.iso \
	#-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -eltorito-platform x86 -iso-level 4 isoroot
	mkisofs -l -V LIVECD -o tccboot.iso \
	-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -iso-level 4 isoroot

	#cp tccboot.iso tccboot-hybrid.iso
	#isohybrid -type 112 -id 0x88888888 tccboot-hybrid.iso
}

FILE_LIST_o=""

# directory in sync with tccboot.iso from bellard to match /boot/tccargs paths
# just create a symlink to appropriate kernel sources and leave it as is
KDIR="usr/src/linux"


### keep tcc for prepare_loader
## ? -fasynchronous-unwind-tables
## ? -D__STRICT_ANSI__ see in types.h
HOSTCC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
#CC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 -fgnu89-inline "
CC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
LD="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
AS="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
#REALAS="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
# 16bit x86 real-mode assembler support
REALAS="i486-pc-linux-musl-as"



### OK; tiny config does not have ext2fs, use romfs then
#KCONF=kconfig-i486-2.4.37.11.TINY
#. ./kfiles-2.4.TINY

### OK; small config from original bellard tccboot.iso
#KCONF=kconfig-i486-2.4.37.11.SMALL
#. ./kfiles-2.4.SMALL

### partial, with broken math-emu softfloat, needs additional patchset, tested and booting
##KCONF=kconfig-i486-2.4.37.11.ALL
##. ./kfiles-2.4.ALL

### reasonably complete configuration including ioapic,usb,smp,squashfs with sufficient test coverage
KCONF=kconfig-i486-2.4.37.11.DEBUG
. ./kfiles-2.4.DEBUG


# ext2, romfs, squashfs v1/v2 rootfs
INITRD="/media/CACHE/TCC/initrd.ext2"
INITRD_SIZE=$(du -B1024 ${INITRD} | cut -d'/' -f1)


#
prepare_config
prepare_loader


### tcc
# ensure tcc does not include nor link anything unknown into; tcc needs patches
export TCC_LIBRARY_PATH="/dev/null"
export TCC_CPATH="/dev/null"
## some missing functions from libtcc can be added to dummy_syms.c
## Freeing initrd memory PANIC -> libtcc1 linked against kernel needed for unknown reason
CCLIB="/usr/lib/tcc/i386-libtcc1.a" compilelink_kernel


### gcc-4.4.7
#CCLIB=""
#CC="i486-pc-linux-musl-gcc -march=i486 -fno-strict-aliasing -fomit-frame-pointer -mpreferred-stack-boundary=2 -ffreestanding -O"
#LD="i486-pc-linux-musl-gcc -ffreestanding -O"
#AS="i486-pc-linux-musl-as"
### gcc needs vmlinux.lds linker script to apply appropriate section layout
#compile_kernel ; link_kernel_gcc


###
create_iso

echo 'sg lanout -c "ncftpput 172.16.2.3 / tccboot.iso"'

