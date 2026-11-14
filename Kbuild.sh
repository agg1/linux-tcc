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

set -eux
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

## as86 support to emit 16bit real-mode boot code is incomplete for trampoline.AS86
## hence it could be used for TINY and SMALL linux-tcc configuration without SMP support then
## full kfiles.DEBUG configuration needs help from yasm and/or gnu-as assembler still
#prepare_loader_as86() {
#	echo "### prepare_loader"
#	cd $CWD
#
#	[ -d "${KTMP}" ] && rm -rf ${KTMP} ; mkdir ${KTMP}
#	cd ${KTMP}
#
#	## AS86
#	cp $CWD/$KDIR/arch/i386/boot/bootsect.AS86 bootsect.AS86
#	cp $CWD/$KDIR/arch/i386/boot/setup.AS86 setup.AS86
#	cp $CWD/$KDIR/arch/i386/boot/video.AS86 video.S
#	cp $CWD/$KDIR/arch/i386/kernel/trampoline.AS86 trampoline.AS86
#
#	mcpp -@old -P -D__BIG_KERNEL__ -I${CWD}/${KDIR}/include bootsect.AS86 -o bootsect.as86
#	$REALAS bootsect.as86 -o bootsect.o
#	$REALLD -d -o bootsect.tcc bootsect.o
#	dd if=bootsect.tcc of=bootsect bs=1 count=512 ; chmod 755 bootsect
#
#	mcpp -@old -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ setup.AS86 -o setup.as86
#	$REALAS setup.as86 -o setup.o
#	$REALLD -d -o setup setup.o
#
#	### with as86 SMP/trampoline.S cannot is not supported yet
#	#mcpp -@std -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ trampoline.AS86 -o trampoline.as86
#	#$REALAS trampoline.as86 -o trampoline.86.o
#	#$REALLD -d -o trampoline.o trampoline.86.o
#
#	## yasm can be used instead here
#	#cp $CWD/$KDIR/arch/i386/kernel/trampoline.S trampoline.S
#	#$CC -E -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ trampoline.S -o trampoline.s
#	#yasm -p gas -f elf32 trampoline.s -o trampoline.o
#
#	#
#	TCC_LIBRARY_PATH="/lib:/usr/lib:/usr/lib/tcc" TCC_CPATH="/usr/include:/usr/lib/tcc/include" $HOSTCC \
#	-I${CWD}/$KDIR/include -I/usr/lib/tcc/include ${CWD}/$KDIR/arch/i386/boot/tools/build86.c -static -o build ; chmod 755 build
#
#	cd $CWD
#}

prepare_isolinux_bin() {
	### either 'yasm -p nasm' or 'nasm' can process isolinux.asm

	### these are all asm sources needed for isolinux
	## cp -pR syslinux-2.13/isolinux.asm .
	## cp -pR syslinux-2.13/*.inc .
	## cp -pR syslinux-2.13/*.gen .
	## cp -pR syslinux-2.13/COPYING .
	## cp -pR syslinux-2.13/README .
	####
	### cp -pR syslinux-2.13/isohybrid.pl .
	#### attaches signature to ISO required for isohybrid boot
	### cp -pR syslinux-2.13/checksumiso.pl .
	### isolinux.c/isolinux.h were grabbed from latest syslinux.git

	cd ${CWD}/isolinux/
	rm -f isolinux.bin
	rm -f *.o
	rm -f isohybrid

	${NASM} -DDATE_STR="0x00000000" -DHEXDATE="0x00000000" -l isolinux.lst -o isolinux.bin isolinux.asm
	# isolinux.bin is sufficient to produce a bootable cd.iso with mkisofs

	### rather use isohybrid compiled from C sources than perl script
	## cp -pR syslinux.git/utils/isohybrid.c .
	## cp -pR syslinux.git/utils/isohybrid.h .
	### patched to ignore missing isohybrid signature in syslinux-2.13/isolinux.asm version
	## cp -pR syslinux.git/mbr/isohdpfx.S .
	### keeping pre-generated isohdpfx.c because it is generated with syslinux.git/utils/isohdpfxarray.pl
	## cp -pR syslinux.git/utils/isohdpfx.c .
	${HOSTCC} -I/usr/include -c isohdpfx.c -o isohdpfx.o
	${HOSTCC} -I/usr/include -c isohybrid.c -o isohybrid.o
	### optional isohybrid support introduces yet another dependency with -luuid
	### which has to be bootstrapped early with tiny-bootstrap if isohybrid boot from usb-flash is desired
	TCC_LIBRARY_PATH="/lib:/usr/lib:/usr/lib/tcc" TCC_CPATH="/usr/include:/usr/lib/tcc/include" \
	${HOSTCC} -static isohdpfx.o isohybrid.o -o isohybrid -luuid
	chmod 755 isohybrid

	### syslinux-2.13 does NOT support isohybrid boot
	### perl ./checksumiso.pl ./isolinux.bin
}

prepare_loader_gas() {
	# either 'yasm -p gas' or GNU binutils-as can do it
	#
	# older linux-2.2 shipped bootsect.AS86 pieces which could be assembled/linked by as86/ld86
	# in case of bootloaders ancient LILO too chose as86 syntax instead of GNU binutils-as
	# porting linux-tcc/2.4 bootsect.S pieces to as86 syntax is feasible but not necessary
	# since yasm assembler is ported to tiny-bootstrap and suffices for linux-tcc/2.4 gas syntax bootsect.S

	echo "### prepare_loader"
	cd $CWD

	[ -d "${KTMP}" ] && rm -rf ${KTMP} ; mkdir ${KTMP}
	cd ${KTMP}

	## GNU gas or YASM confirmed
	cp $CWD/$KDIR/arch/i386/boot/bootsect.S bootsect.S
	cp $CWD/$KDIR/arch/i386/boot/setup.S setup.S
	cp $CWD/$KDIR/arch/i386/boot/video.S video.S
	cp $CWD/$KDIR/arch/i386/kernel/trampoline.S trampoline.S

	$CC -E -P -nostdinc -nostdlib -D__BIG_KERNEL__ -I${CWD}/${KDIR}/include bootsect.S -o bootsect.s
	$REALAS bootsect.s -o bootsect.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o bootsect.tcc bootsect.o
	dd if=bootsect.tcc of=bootsect bs=1 count=512 ; chmod 755 bootsect

	$CC -E -P -I${CWD}/${KDIR}/include -D__ASSEMBLY__ -D__KERNEL__ -D__BIG_KERNEL__ setup.S -o setup.s
	$REALAS setup.s -o setup.o
	$LD -nostdlib -static -Wl,-Ttext,0 -Wl,--oformat,binary -o setup setup.o

	# 16bit real-mode routines required for smp init, reconfirm inline assembly in rwlock.h 
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

label linux-debug
	kernel /boot/linux
	append debug earlyprintk console=ttyS0,9600 console=tty0 video=vesa:mtrr initrd=/boot/initrd ramdisk_size=${INITRD_SIZE} root=/dev/ram0 rootfstype=romfs no_timer_check vga=ask
" > ${KTMP}/isoroot/isolinux/isolinux.cfg

	# some isolinux.asm/bin must be bootstrapped/compiled with i386-tcc/yasm toolchain
	# yasm is preferred over nasm because yasm supports both GAS and NASM syntax
	# NASM syntax support is needed for isolinux.asm and GAS syntax is needed for linux-tcc/2.4 bootsect.S pieces
	# yasm assembler can handle all of it: gas, nasm, .code16, .code32
	# with a much smaller compile-time dependency graph than binutils-as
	# and isolinux.bin itself got a much smaller compile-time dependency graph than GRUB
	# to support CD ISO boot
	cp ${ISOLINUX_BIN} ${KTMP}/isoroot/isolinux/
	cp ${INITRD} ${KTMP}/isoroot/boot/initrd

	if [ ! -e "${KTMP}/vmlinux" ] ; then
		echo "vmlinux missing." ; exit 1
	fi

	cd ${KTMP}
	./build -b ./bootsect ./setup vmlinux >isoroot/boot/linux

	# fiddle together tccargs and usr/src/linux for final FILE_LIST on target
	#genromfs -v -x '.git' -d /media/CACHE/TCC/linux-bellard -f ../isoroot/boot/example.romfs >/dev/null 2>&1

	### this non-hybrid ISO MUST be burned onto CD for booting
	cd ${KTMP}
	mkisofs -l -V LIVECD -o tccboot.iso \
	-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -iso-level 4 isoroot

### isohybrid boot is NOT yet supported with a bootstrappable isolinux.asm|bin ported to i386-tcc/yasm
### if isohybrid boot from usb-flash is desired then ISOLINUX_BIN must point to some recent syslinux-3.86 version or later
### such cd-hybrid.iso then can be either burned onto CD or directly dumped onto usb-flash
#	cp tccboot.iso tccboot-hybrid.iso
#	# isohybrid can be compiled from isohybrid.c instead of isohybrid.pl written in perl
#	# to remove perl from the compile-time dependency graph
#	${ISOHYBRID_BIN} -type 112 -id 0x88888888 tccboot-hybrid.iso || true


	echo "sg lanout -c \"ncftpput 172.16.2.3 / ${KTMP}/tccboot.iso\""
#	echo "sg lanout -c \"ncftpput 172.16.2.3 / ${KTMP}/tccboot-hybrid.iso\""

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

### AS86 suffices for TINY and SMALL configuration variant only; trampoline.AS86 needs re-feactoring
##REALAS="/usr/bin/as86 -0 -a"
##REALLD="/usr/bin/ld86"
##prepare_loader_as86

## TinyCC is not known for sufficient support of 16bit real-mode assembly to emit linux bootcode
#REALAS="/usr/bin/i386-tcc -D__GNUC__=2 -D__GNUC_MINOR__=95 ${OPTIMIZE}"
## binutils introduce a gigantic dependency graph to compile a few hundred lines of real-mode assembly
#REALAS="i586-tcc-linux-musl-as"
## YASM can fully replace GNU binutils-as with a much smaller dependency graph and less lines of code
REALAS="yasm -p gas -f elf32"
prepare_loader_gas

## ensure tcc does not include nor link anything unknown into; tcc patches/tcc/tcc-9999-library_path.patch
export TCC_LIBRARY_PATH="/dev/null"
export TCC_CPATH="/dev/null"
## some missing functions from libtcc can be added to arch/i386/lib/dummy_syms.c when CCLIB=""
## Freeing initrd memory PANIC -> libtcc1 linked against kernel needed for unknown reason
##
## compilelink_kernel produces mis-compiled/mis-linked binary when tcc compiles/links in a single-pass !!!
## noticed with ahci.c which panics kernel in interrupt handler (dd if=/dev/sdX of=/dev/null bs=1M count=1024)!
#CCLIB="/usr/lib/tcc/i386-libtcc1.a" compilelink_kernel
## first compiling objects and linking in a separate stage with tcc does not cause kernel panics with ahci.c/sata
compile_kernel ; CCLIB="/usr/lib/tcc/i386-libtcc1.a" link_kernel


### gcc-4.7.4
## GNU toolchain was re-confirmed to compile/link linux-tcc regularly and is known to work
#CCLIB=""
#CC="i586-pc-linux-musl-gcc -march=i486 -fno-strict-aliasing -fomit-frame-pointer -mpreferred-stack-boundary=2 -ffreestanding -O"
#LD="i586-pc-linux-musl-gcc -ffreestanding -O"
#AS="i586-pc-linux-musl-as"
### gcc needs vmlinux.lds linker script to apply appropriate section layout
#compile_kernel ; link_kernel_gcc


## of cause some bootloader is needed to yield a bootable ISO with create_iso
## GRUB got a gigantic compile-time dependency with GNU-toolchain, autotools, perl, scheme, gcc, binutils etc...
## porting isolinux.bin to i386-tcc/yasm toolchain instead reduces dependencies by a hundred million lines of code
##
## by default using re-compiled isolinux.bin from syslinux-2.13/isolinux.asm sources shipped with linux-tcc
## isohybrid support can remain optional when booting from CD instead of usb-flash
ISOLINUX_BIN="${CWD}/isolinux/isolinux.bin"
## not yet supported with bootstrappable syslinux-2.13/isolinux.bin
#ISOHYBRID_BIN="${CWD}/isolinux/isohybrid"
## yasm supports both NASM and GAS syntax hence yasm is ported to tiny-bootstrap instead of nasm
NASM="yasm -p nasm -f bin"
[ -e /usr/bin/mkisofs ] && prepare_isolinux_bin


###
## mkisofs is provided by app-cdr/cdrtools-3.02_alpha09-r5 which must be ported to tiny-bootstrap
[ -e /usr/bin/mkisofs ] && create_iso

