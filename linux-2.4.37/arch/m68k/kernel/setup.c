/*
 *  linux/arch/m68k/kernel/setup.c
 *
 *  Copyright (C) 1995  Hamish Macdonald
 */

/*
 * This file handles the architecture-dependent parts of system setup
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/console.h>
#include <linux/genhd.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/seq_file.h>
#include <linux/module.h>

#include <asm/bootinfo.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/machdep.h>
#ifdef CONFIG_AMIGA
#include <asm/amigahw.h>
#endif
#ifdef CONFIG_ATARI
#include <asm/atarihw.h>
#include <asm/atari_stram.h>
#endif
#ifdef CONFIG_SUN3X
#include <asm/dvma.h>
extern void sun_serial_setup(void);
#endif

#ifdef CONFIG_BLK_DEV_INITRD
#include <linux/blk.h>
#endif

unsigned long m68k_machtype;
unsigned long m68k_cputype;
unsigned long m68k_fputype;
unsigned long m68k_mmutype;
#ifdef CONFIG_VME
unsigned long vme_brdtype;
#endif

int m68k_is040or060 = 0;

extern int end;
extern unsigned long availmem;

int m68k_num_memory = 0;
int m68k_realnum_memory = 0;
unsigned long m68k_memoffset;
struct mem_info m68k_memory[NUM_MEMINFO];

static struct mem_info m68k_ramdisk = { 0, 0 };

static char m68k_command_line[CL_SIZE];
char saved_command_line[CL_SIZE];

char m68k_debug_device[6] = "";

void (*mach_sched_init) (void (*handler)(int, void *, struct pt_regs *)) __initdata = NULL;
/* machine dependent keyboard functions */
#ifdef CONFIG_VT
int (*mach_keyb_init) (void) __initdata = NULL;
int (*mach_kbdrate) (struct kbd_repeat *) = NULL;
void (*mach_kbd_leds) (unsigned int) = NULL;
int (*mach_kbd_translate)(unsigned char scancode, unsigned char *keycode, char raw_mode) = NULL;
#endif
/* machine dependent irq functions */
void (*mach_init_IRQ) (void) __initdata = NULL;
void (*(*mach_default_handler)[]) (int, void *, struct pt_regs *) = NULL;
void (*mach_get_model) (char *model) = NULL;
int (*mach_get_hardware_list) (char *buffer) = NULL;
int (*mach_get_irq_list) (char *) = NULL;
void (*mach_process_int) (int, struct pt_regs *) = NULL;
/* machine dependent timer functions */
unsigned long (*mach_gettimeoffset) (void);
void (*mach_gettod) (int*, int*, int*, int*, int*, int*);
int (*mach_hwclk) (int, struct rtc_time*) = NULL;
int (*mach_set_clock_mmss) (unsigned long) = NULL;
unsigned int (*mach_get_ss)(void) = NULL;
int (*mach_get_rtc_pll)(struct rtc_pll_info *) = NULL;
int (*mach_set_rtc_pll)(struct rtc_pll_info *) = NULL;
void (*mach_reset)( void );
void (*mach_halt)( void ) = NULL;
void (*mach_power_off)( void ) = NULL;
long mach_max_dma_address = 0x00ffffff; /* default set to the lower 16MB */
#if defined(CONFIG_AMIGA_FLOPPY) || defined(CONFIG_ATARI_FLOPPY) 
void (*mach_floppy_setup) (char *, int *) __initdata = NULL;
#endif
#ifdef CONFIG_HEARTBEAT
void (*mach_heartbeat) (int) = NULL;
EXPORT_SYMBOL(mach_heartbeat);
#endif
#ifdef CONFIG_M68K_L2_CACHE
void (*mach_l2_flush) (int) = NULL;
#endif

#ifdef CONFIG_MAGIC_SYSRQ
unsigned int SYSRQ_KEY;
int mach_sysrq_key = -1;
int mach_sysrq_shift_state = 0;
int mach_sysrq_shift_mask = 0;
char *mach_sysrq_xlate = NULL;
#endif

#if defined(CONFIG_ISA) && defined(MULTI_ISA)
int isa_type;
int isa_sex;
#endif

extern int amiga_parse_bootinfo(const struct bi_record *);
extern int atari_parse_bootinfo(const struct bi_record *);
extern int mac_parse_bootinfo(const struct bi_record *);
extern int q40_parse_bootinfo(const struct bi_record *);
extern int bvme6000_parse_bootinfo(const struct bi_record *);
extern int mvme16x_parse_bootinfo(const struct bi_record *);
extern int mvme147_parse_bootinfo(const struct bi_record *);

extern void config_amiga(void);
extern void config_atari(void);
extern void config_mac(void);
extern void config_sun3(void);
extern void config_apollo(void);
extern void config_mvme147(void);
extern void config_mvme16x(void);
extern void config_bvme6000(void);
extern void config_hp300(void);
extern void config_q40(void);
extern void config_sun3x(void);

extern void mac_debugging_short (int, short);
extern void mac_debugging_long  (int, long);

#define MASK_256K 0xfffc0000

extern void paging_init(void);

static void __init m68k_parse_bootinfo(const struct bi_record *record)
{
    while (record->tag != BI_LAST) {
	int unknown = 0;
	const unsigned long *data = record->data;
	switch (record->tag) {
	    case BI_MACHTYPE:
	    case BI_CPUTYPE:
	    case BI_FPUTYPE:
	    case BI_MMUTYPE:
		/* Already set up by head.S */
		break;

 	    case BI_MEMCHUNK:
		if (m68k_num_memory < NUM_MEMINFO) {
		    m68k_memory[m68k_num_memory].addr = data[0];
		    m68k_memory[m68k_num_memory].size = data[1];
		    m68k_num_memory++;
		} else
		    printk("m68k_parse_bootinfo: too many memory chunks\n");
		break;

	    case BI_RAMDISK:
		m68k_ramdisk.addr = data[0];
		m68k_ramdisk.size = data[1];
		break;

	    case BI_COMMAND_LINE:
		strncpy(m68k_command_line, (const char *)data, CL_SIZE);
		m68k_command_line[CL_SIZE-1] = '\0';
		break;

	    default:
		if (MACH_IS_AMIGA)
		    unknown = amiga_parse_bootinfo(record);
		else if (MACH_IS_ATARI)
		    unknown = atari_parse_bootinfo(record);
		else if (MACH_IS_MAC)
		    unknown = mac_parse_bootinfo(record);
		else if (MACH_IS_Q40)
		    unknown = q40_parse_bootinfo(record);
		else if (MACH_IS_BVME6000)
		    unknown = bvme6000_parse_bootinfo(record);
		else if (MACH_IS_MVME16x)
		    unknown = mvme16x_parse_bootinfo(record);
		else if (MACH_IS_MVME147)
		    unknown = mvme147_parse_bootinfo(record);
		else
		    unknown = 1;
	}
	if (unknown)
	    printk("m68k_parse_bootinfo: unknown tag 0x%04x ignored\n",
		   record->tag);
	record = (struct bi_record *)((unsigned long)record+record->size);
    }

    m68k_realnum_memory = m68k_num_memory;
#ifdef CONFIG_SINGLE_MEMORY_CHUNK
    if (m68k_num_memory > 1) {
	printk("Ignoring last %i chunks of physical memory\n",
	       (m68k_num_memory - 1));
	m68k_num_memory = 1;
    }
    m68k_memoffset = m68k_memory[0].addr-PAGE_OFFSET;
#endif
}

void __init setup_arch(char **cmdline_p)
{
	extern int _etext, _edata, _end;
#ifndef CONFIG_SUN3
	unsigned long endmem, startmem;
#endif
	int i;
	char *p, *q;

	if (!MACH_IS_HP300) {
		/* The bootinfo is located right after the kernel bss */
		m68k_parse_bootinfo((const struct bi_record *)&_end);
	} else {
		/* FIXME HP300 doesn't use bootinfo yet */
		extern unsigned long hp300_phys_ram_base;
		unsigned long hp300_mem_size = 0xffffffff-hp300_phys_ram_base;
		m68k_cputype = CPU_68030;
		m68k_fputype = FPU_68882;
		m68k_memory[0].addr = hp300_phys_ram_base;
		/* 0.5M fudge factor */
		m68k_memory[0].size = hp300_mem_size-512*1024;
		m68k_num_memory++;
	}

	if (CPU_IS_040)
		m68k_is040or060 = 4;
	else if (CPU_IS_060)
		m68k_is040or060 = 6;

	if (CPU_IS_060) {
		u32 pcr;

		asm (".chip 68060; movec %%pcr,%0; .chip 68k"
		     : "=d" (pcr));
		if (((pcr >> 8) & 0xff) <= 5) {
			printk("Enabling workaround for errata I14\n");
			asm (".chip 68060; movec %0,%%pcr; .chip 68k"
			     : : "d" (pcr | 0x20));
		}
	}

	/* FIXME: m68k_fputype is passed in by Penguin booter, which can
	 * be confused by software FPU emulation. BEWARE.
	 * We should really do our own FPU check at startup.
	 * [what do we do with buggy 68LC040s? if we have problems
	 *  with them, we should add a test to check_bugs() below] */
#ifndef CONFIG_M68KFPU_EMU_ONLY
	/* clear the fpu if we have one */
	if (m68k_fputype & (FPU_68881|FPU_68882|FPU_68040|FPU_68060)) {
		volatile int zero = 0;
		asm __volatile__ ("frestore %0" : : "m" (zero));
	}
#endif	

	init_mm.start_code = PAGE_OFFSET;
	init_mm.end_code = (unsigned long) &_etext;
	init_mm.end_data = (unsigned long) &_edata;
	init_mm.brk = (unsigned long) &_end;

	*cmdline_p = m68k_command_line;
	memcpy(saved_command_line, *cmdline_p, CL_SIZE);

	/* Parse the command line for arch-specific options.
	 * For the m68k, this is currently only "debug=xxx" to enable printing
	 * certain kernel messages to some machine-specific device.
	 */
	for( p = *cmdline_p; p && *p; ) {
	    i = 0;
	    if (!strncmp( p, "debug=", 6 )) {
		strncpy( m68k_debug_device, p+6, sizeof(m68k_debug_device)-1 );
		m68k_debug_device[sizeof(m68k_debug_device)-1] = 0;
		if ((q = strchr( m68k_debug_device, ' ' ))) *q = 0;
		i = 1;
	    }
#ifdef CONFIG_ATARI
	    /* These options must be parsed very early */
	    if (!strncmp( p, "switches=", 9 )) {
		extern void atari_switches_setup( const char *, int );
		atari_switches_setup( p+9, (q = strchr( p+9, ' ' )) ?
				           (q - (p+9)) : strlen(p+9) );
		i = 1;
	    }
#ifdef CONFIG_STRAM_SWAP
	    if (!strncmp( p, "stram_swap=", 11 )) {
		extern void stram_swap_setup( char * );
		stram_swap_setup( p+11 );
		i = 1;
	    }
#endif	/* CONFIG_STRAM_SWAP */
#endif

	    if (i) {
		/* option processed, delete it */
		if ((q = strchr( p, ' ' )))
		    strcpy( p, q+1 );
		else
		    *p = 0;
	    } else {
		if ((p = strchr( p, ' ' ))) ++p;
	    }
	}

	switch (m68k_machtype) {
#ifdef CONFIG_AMIGA
	    case MACH_AMIGA:
		config_amiga();
		break;
#endif
#ifdef CONFIG_ATARI
	    case MACH_ATARI:
		config_atari();
		break;
#endif
#ifdef CONFIG_MAC
	    case MACH_MAC:
		config_mac();
		break;
#endif
#ifdef CONFIG_SUN3
	    case MACH_SUN3:
	    	config_sun3();
	    	break;
#endif
#ifdef CONFIG_APOLLO
	    case MACH_APOLLO:
	    	config_apollo();
	    	break;
#endif
#ifdef CONFIG_MVME147
	    case MACH_MVME147:
	    	config_mvme147();
	    	break;
#endif
#ifdef CONFIG_MVME16x
	    case MACH_MVME16x:
	    	config_mvme16x();
	    	break;
#endif
#ifdef CONFIG_BVME6000
	    case MACH_BVME6000:
	    	config_bvme6000();
	    	break;
#endif
#ifdef CONFIG_HP300
	    case MACH_HP300:
		config_hp300();
		break;
#endif
#ifdef CONFIG_Q40
	    case MACH_Q40:
	        config_q40();
		break;
#endif
#ifdef CONFIG_SUN3X
	    case MACH_SUN3X:
		config_sun3x();
		break;
#endif
	    default:
		panic ("No configuration setup");
	}

#ifndef CONFIG_SUN3
	startmem= m68k_memory[0].addr;
	endmem = startmem + m68k_memory[0].size;
	high_memory = PAGE_OFFSET;
	for (i = 0; i < m68k_num_memory; i++) {
		m68k_memory[i].size &= MASK_256K;
		if (m68k_memory[i].addr < startmem)
			startmem = m68k_memory[i].addr;
		if (m68k_memory[i].addr+m68k_memory[i].size > endmem)
			endmem = m68k_memory[i].addr+m68k_memory[i].size;
		high_memory += m68k_memory[i].size;
	}

	availmem += init_bootmem_node(NODE_DATA(0), availmem >> PAGE_SHIFT,
				      startmem >> PAGE_SHIFT, endmem >> PAGE_SHIFT);

	for (i = 0; i < m68k_num_memory; i++)
		free_bootmem(m68k_memory[i].addr, m68k_memory[i].size);

	reserve_bootmem(m68k_memory[0].addr, availmem - m68k_memory[0].addr);

#ifdef CONFIG_BLK_DEV_INITRD
	if (m68k_ramdisk.size) {
		reserve_bootmem(m68k_ramdisk.addr, m68k_ramdisk.size);
		initrd_start = (unsigned long)phys_to_virt(m68k_ramdisk.addr);
		initrd_end = initrd_start + m68k_ramdisk.size;
		printk ("initrd: %08lx - %08lx\n", initrd_start, initrd_end);
	}
#endif

#ifdef CONFIG_ATARI
	if (MACH_IS_ATARI)
		atari_stram_reserve_pages((void *)availmem);
#endif
#ifdef CONFIG_SUN3X
	if (MACH_IS_SUN3X) {
		dvma_init();
#ifdef CONFIG_SUN3X_ZS
		sun_serial_setup();
#endif
	}
#endif

#endif /* !CONFIG_SUN3 */

	paging_init();

/* set ISA defs early as possible */
#if defined(CONFIG_ISA) && defined(MULTI_ISA)
#if defined(CONFIG_Q40) 
	if (MACH_IS_Q40) {
	    isa_type = Q40_ISA;
	    isa_sex = 0;
	} 
#elif defined(CONFIG_GG2)
	if (MACH_IS_AMIGA && AMIGAHW_PRESENT(GG2_ISA)){
	    isa_type = GG2_ISA;
	    isa_sex = 0;
	}
#elif defined(CONFIG_AMIGA_PCMCIA)
	if (MACH_IS_AMIGA && AMIGAHW_PRESENT(PCMCIA)){
	    isa_type = AG_ISA;
	    isa_sex = 1;
	}
#endif
#endif
}

static int show_cpuinfo(struct seq_file *m, void *v)
{
    const char *cpu, *mmu, *fpu;
    unsigned long clockfreq, clockfactor;

#define LOOP_CYCLES_68020	(8)
#define LOOP_CYCLES_68030	(8)
#define LOOP_CYCLES_68040	(3)
#define LOOP_CYCLES_68060	(1)

    if (CPU_IS_020) {
	cpu = "68020";
	clockfactor = LOOP_CYCLES_68020;
    } else if (CPU_IS_030) {
	cpu = "68030";
	clockfactor = LOOP_CYCLES_68030;
    } else if (CPU_IS_040) {
	cpu = "68040";
	clockfactor = LOOP_CYCLES_68040;
    } else if (CPU_IS_060) {
	cpu = "68060";
	clockfactor = LOOP_CYCLES_68060;
    } else {
	cpu = "680x0";
	clockfactor = 0;
    }

#ifdef CONFIG_M68KFPU_EMU_ONLY
    fpu="none(soft float)";
#else
    if (m68k_fputype & FPU_68881)
	fpu = "68881";
    else if (m68k_fputype & FPU_68882)
	fpu = "68882";
    else if (m68k_fputype & FPU_68040)
	fpu = "68040";
    else if (m68k_fputype & FPU_68060)
	fpu = "68060";
    else if (m68k_fputype & FPU_SUNFPA)
	fpu = "Sun FPA";
    else
	fpu = "none";
#endif

    if (m68k_mmutype & MMU_68851)
	mmu = "68851";
    else if (m68k_mmutype & MMU_68030)
	mmu = "68030";
    else if (m68k_mmutype & MMU_68040)
	mmu = "68040";
    else if (m68k_mmutype & MMU_68060)
	mmu = "68060";
    else if (m68k_mmutype & MMU_SUN3)
	mmu = "Sun-3";
    else if (m68k_mmutype & MMU_APOLLO)
	mmu = "Apollo";
    else
	mmu = "unknown";

    clockfreq = loops_per_jiffy*HZ*clockfactor;

    seq_printf(m, "CPU:\t\t%s\n"
		   "MMU:\t\t%s\n"
		   "FPU:\t\t%s\n"
		   "Clocking:\t%lu.%1luMHz\n"
		   "BogoMips:\t%lu.%02lu\n"
		   "Calibration:\t%lu loops\n",
		   cpu, mmu, fpu,
		   clockfreq/1000000,(clockfreq/100000)%10,
		   loops_per_jiffy/(500000/HZ),(loops_per_jiffy/(5000/HZ))%100,
		   loops_per_jiffy);
    return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}
static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}
static void c_stop(struct seq_file *m, void *v)
{
}
const struct seq_operations cpuinfo_op = {
	start:	c_start,
	next:	c_next,
	stop:	c_stop,
	show:	show_cpuinfo,
};

int get_hardware_list(char *buffer)
{
    int len = 0;
    char model[80];
    unsigned long mem;
    int i;

    if (mach_get_model)
	mach_get_model(model);
    else
	strcpy(model, "Unknown m68k");

    len += sprintf(buffer+len, "Model:\t\t%s\n", model);
    //len += get_cpuinfo(buffer+len);
    for (mem = 0, i = 0; i < m68k_num_memory; i++)
	mem += m68k_memory[i].size;
    len += sprintf(buffer+len, "System Memory:\t%ldK\n", mem>>10);

    if (mach_get_hardware_list)
	len += mach_get_hardware_list(buffer+len);

    return(len);
}


#if defined(CONFIG_AMIGA_FLOPPY) || defined(CONFIG_ATARI_FLOPPY)
void __init floppy_setup(char *str, int *ints)
{
	if (mach_floppy_setup)
		mach_floppy_setup (str, ints);
}

#endif

/* for "kbd-reset" cmdline param */
void __init kbd_reset_setup(char *str, int *ints)
{
}

void arch_gettod(int *year, int *mon, int *day, int *hour,
		 int *min, int *sec)
{
	if (mach_gettod)
		mach_gettod(year, mon, day, hour, min, sec);
	else
		*year = *mon = *day = *hour = *min = *sec = 0;
}

void check_bugs(void)
{
#ifndef CONFIG_M68KFPU_EMU
	if (m68k_fputype == 0) {
		printk( KERN_EMERG "*** YOU DO NOT HAVE A FLOATING POINT UNIT, "
				"WHICH IS REQUIRED BY LINUX/M68K ***\n" );
		printk( KERN_EMERG "Upgrade your hardware or join the FPU "
				"emulation project\n" );
		printk( KERN_EMERG "(see http://no-fpu.linux-m68k.org)\n" );
		panic( "no FPU" );
	}
#endif /* !CONFIG_M68KFPU_EMU */
}
