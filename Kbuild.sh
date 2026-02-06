#!/bin/sh -e

# (c) 2023,2024,2025,2026 aggi
# this script fully replaces the linux-2.4 internal Kbuild system to simplify
# AoT compilation of kernel with tinycc compiler
#
# Original tccboot was introduced by Fabrice https://bellard.org/tcc/tccboot.html
# for JiT compilation of linux-2.4 kernel
#
# AoT compilation of linux-2.4 was inspired by https://github.com/seyko2/tccboot
# which was not maintained for a decade and suffered from regressions

set -eu
CWD="$(pwd)"
rm -f LOG

prepare_config() {
	echo "### prepare_config"
	cd $CWD/$KDIR/include ; rm -f asm ; ln -sf asm-i386 asm ; cd $CWD

	cp ${KCONF} $CWD/$KDIR/.config

	cd $CWD/$KDIR
	##make ARCH=i386 CC="i386-tcc" LD="i386-tcc" AS="i386-tcc" HOSTCC="i386-tcc" menuconfig
	# autoconf.h and version.h
	make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" oldconfig >/dev/null
	make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" include/linux/version.h
	# slow, use pre-generated compile.h instead
	##make ARCH=i386 CC="$CC" LD="$LD" AS="$AS" HOSTCC="$HOSTCC" dep include/linux/compile.h
}

prepare_loader() {
	echo "### prepare_loader"
	cd $CWD

	[ -d "${KTMP}" ] && rm -rf ${KTMP} ; mkdir ${KTMP}
	cd ${KTMP}

	cp $CWD/$KDIR/arch/i386/boot/bootsect.S .
	$CC -E -P -nostdinc -nostdlib -D__BIG_KERNEL__ -I${CWD}/${KDIR}/include bootsect.S -o bootsect.s
	$REALAS bootsect.s -o bootsect.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o bootsect.tcc bootsect.o
	dd if=bootsect.tcc of=bootsect bs=1 count=512 ; chmod 755 bootsect

	cp $CWD/$KDIR/arch/i386/boot/setup.S .
	cp $CWD/$KDIR/arch/i386/boot/video.S .
	$CC -E -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ setup.S -o setup.s
	$REALAS setup.s -o setup.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o setup.tcc setup.o
	## that is exactly 5 sectors on disk ?
	#dd if=setup.tcc of=setup bs=1 count=2560 ; chmod 755 setup
	dd if=setup.tcc of=setup bs=1; chmod 755 setup

	# 16bit real-mode routines required for smp init, probably buggy rwlock.h patch
	cp $CWD/$KDIR/arch/i386/kernel/trampoline.S .
	$CC -E -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ trampoline.S -o trampoline.s
	$REALAS trampoline.s -o trampoline.o

	#
	TCC_LIBRARY_PATH="/lib:/usr/lib:/usr/lib/tcc" TCC_CPATH="/usr/include:/usr/lib/tcc/include" $HOSTCC \
	-I${CWD}/$KDIR/include -I/usr/lib/tcc/include ${CWD}/$KDIR/arch/i386/boot/tools/build.c -static -o build ; chmod 755 build

	cd $CWD
}

compile_kernel() {
	echo "### compile_kernel" | tee -a LOG
	cd $CWD

	rm -f ${KTMP}/vmlinux
	[ -d ${KTMP}/temp ] && rm -rf ${KTMP}/temp ; mkdir ${KTMP}/temp

	for i in $FILE_LIST
	do
		echo $i 2>&1 | tee -a LOG
		dir=$(dirname $i)
		[ ! -d ${KTMP}/temp/$dir ] && mkdir -p ${KTMP}/temp/$dir

		case $i in
		*.o)
			# tcc assembler does not support 16 bit real-mode asm hence trampoline.S is pre-compiled
			cp ${i} ${KTMP}/temp/$i
			FILE_LIST_o="$FILE_LIST_o ${KTMP}/temp/$i"
		;;
		*)
			$CC \
			-o ${KTMP}/temp/$i.o \
			-fno-common -nostdinc -nostdlib \
			-I${CWD}/${KDIR}/include \
			-D__KERNEL__ \
			-c ${CWD}/${i} 2>&1 | tee -a LOG

			FILE_LIST_o="$FILE_LIST_o ${KTMP}/temp/$i.o"
		;;
		esac
	done
}

link_kernel() {
	echo "### link_kernel" | tee -a LOG
	cd $CWD

	$LD \
	-o ${KTMP}/vmlinux \
	-fno-common -nostdinc -nostdlib -static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary \
	$FILE_LIST_o \
	$CCLIB 2>&1 | tee -a LOG

	cd ${KTMP}
	./build -b ./bootsect ./setup vmlinux >${CWD}/linux
}

compilelink_kernel() {
	echo "### compilelink_kernel" | tee -a LOG
	cd $CWD

	$CC \
	-o ${KTMP}/vmlinux \
	-fno-common -nostdinc -nostdlib -static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary  \
	-I$KDIR/include \
	-D__KERNEL__ \
	$FILE_LIST \
	$CCLIB 2>&1 | tee -a LOG

	cd ${KTMP}
	./build -b ./bootsect ./setup vmlinux >${CWD}/linux
}

link_kernel_gcc() {
	echo "### link_kernel_gcc" | tee -a LOG
	cd $CWD

	$LD \
	-o ${KTMP}/vmlinux \
	-nostdlib -nodefaultlibs -nostartfiles \
	-static -Wl,-Ttext,0xc0100000 -Wl,--oformat,binary \
	-e startup_32 -Tusr/src/linux/arch/i386/vmlinux.lds \
	$FILE_LIST_o \
	$CCLIB 2>&1 | tee -a LOG

	cd ${KTMP}
	./build -b ./bootsect ./setup vmlinux >${CWD}/linux
}


create_iso() {
	echo "### create_iso"
	cd $CWD

	rm -f ${KTMP}/tccboot.iso ${KTMP}/tccboot-hybrid.iso ; 	rm -rf ${KTMP}/isoroot ; mkdir ${KTMP}/isoroot ; mkdir ${KTMP}/isoroot/boot ; mkdir ${KTMP}/isoroot/isolinux

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
" > ${KTMP}/isoroot/isolinux/isolinux.cfg

#label tccboot
#	kernel /boot/tccboot
#	append initrd=/boot/example.romfs root=/dev/ram ramdisk_size=20000 devfs=nomount


	cp /usr/share/syslinux/isolinux.bin ${KTMP}/isoroot/isolinux/
	cp ${INITRD} ${KTMP}/isoroot/boot/initrd

	if [ ! -e "${KTMP}/vmlinux" ] ; then
		echo "vmlinux missing." ; exit 1
	fi

	cd ${KTMP}
	#./build -b ./bootsect ./setup vmlinux CURRENT >../isoroot/boot/linux
	./build -b ./bootsect ./setup vmlinux >isoroot/boot/linux

	# fiddle together tccargs and usr/src/linux for final FILE_LIST on target
	#genromfs -v -x '.git' -d /media/CACHE/TCC/linux-bellard -f ../isoroot/boot/example.romfs >/dev/null 2>&1

	cd ${KTMP}
	### mkisofs -J -R -l -V LIVECD -modification-date 19700101000000.00 \
	#mkisofs -l -V LIVECD -modification-date 19700101000000.00 -o tccboot.iso \
	#-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -eltorito-platform x86 -iso-level 4 isoroot
	mkisofs -l -V LIVECD -o tccboot.iso \
	-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -iso-level 4 isoroot

	cp tccboot.iso tccboot-hybrid.iso
	isohybrid -type 112 -id 0x88888888 tccboot-hybrid.iso

	echo "sg lanout -c \"ncftpput 172.16.2.3 / ${KTMP}/tccboot.iso\""
	echo "sg lanout -c \"ncftpput 172.16.2.3 / ${KTMP}/tccboot-hybrid.iso\""

	cd ${CWD}
}

FILE_LIST_o=""

# directory in sync with tccboot.iso from bellard to match /boot/tccargs paths
# just create a symlink to appropriate kernel sources and leave it as is here
mkdir -p usr/src/ ; cd usr/src/ ; rm -f linux ; ln -sf ../../linux-2.4.37 linux ; cd ${CWD}
cp -p linux-2.4.37/tcc/compile.h usr/src/linux/include/linux/compile.h
cp -p linux-2.4.37/tcc/consolemap_deftbl.c usr/src/linux/drivers/char/consolemap_deftbl.c
KDIR="usr/src/linux"

# build output
KTMP="/var/tmp/ktmp"
#mount -o remount,exec ${KTMP} >/dev/null 2>&1 || true

### keep tcc for prepare_loader
## ? -D__STRICT_ANSI__ see in types.h
HOSTCC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 "
#
### -O1 raised suspicion if this was related to random kernel panics in ahci.c and scrandom.c related testing
#OPTIMIZE=" -O1 -D__OPTIMIZE__ "
## note linux-2.4.37/arch/i386/lib/dummy_syms.c needs additional symbols with -O0 for htonl/ntohl/ntohs/htons
OPTIMIZE=" -O0 -U__OPTIMIZE__ "
#
#CC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 -fgnu89-inline -DUTS_MACHINE='i586' ${OPTIMIZE}"
CC="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 -fgnu89-inline ${OPTIMIZE}"
LD="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 -fgnu89-inline ${OPTIMIZE}"
AS="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 ${OPTIMIZE}"
#REALAS="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 ${OPTIMIZE}"
# 16bit x86 real-mode assembler support
REALAS="i586-tcc-linux-musl-as"


### OK; tiny config does not have ext2fs, use romfs then
#KCONF=kconfig-i486-2.4.37.11.TINY
#. ./kfiles-2.4.TINY

### OK; small config from original bellard tccboot.iso
#KCONF=kconfig-i486-2.4.37.11.SMALL
#. ./kfiles-2.4.SMALL

### partial, with broken math-emu softfloat, needs additional patchset, tested and booting
##KCONF=kconfig-i486-2.4.37.11.ALL
##. ./kfiles-2.4.ALL

### reasonably complete configuration
## including ioapic,usb,smp,squashfs,highmen,ahci,grsecurity,vesa-framebuffer optional PAE etc...
KCONF=kconfig-i486-2.4.37.11.DEBUG
. ./kfiles-2.4.DEBUG


# ext2, romfs, squashfs v1/v2 rootfs
INITRD="/media/DATA/TCC/initrd.romfs"
INITRD_SIZE="$(du -B1024 ${INITRD} | cut -d'/' -f1 || echo 0)"


#
prepare_config
prepare_loader


### tcc
## ensure tcc does not include nor link anything unknown into; tcc patches/tcc/tcc-9999-library_path.patch
export TCC_LIBRARY_PATH="/dev/null"
export TCC_CPATH="/dev/null"
## some missing functions from libtcc can be added to arch/i386/lib/dummy_syms.c when CCLIB=""
## Freeing initrd memory PANIC -> libtcc1 linked against kernel needed for unknown reason
##
CCLIB="/usr/lib/tcc/i386-libtcc1.a" compilelink_kernel
## first compiling objects and linking in a separate stage
#compile_kernel ; CCLIB="/usr/lib/tcc/i386-libtcc1.a" link_kernel


### gcc-4.7.4
#CCLIB=""
#CC="i586-pc-linux-musl-gcc -march=i486 -fno-strict-aliasing -fomit-frame-pointer -mpreferred-stack-boundary=2 -ffreestanding -O"
#LD="i586-pc-linux-musl-gcc -ffreestanding -O"
#AS="i586-pc-linux-musl-as"
### gcc needs vmlinux.lds linker script to apply appropriate section layout
#compile_kernel ; link_kernel_gcc


###
create_iso

