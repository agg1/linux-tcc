#ifndef __ARCH_DESC_H
#define __ARCH_DESC_H

#include <asm/ldt.h>
#include <asm/segment.h>

#ifndef __ASSEMBLY__

#include <asm/mmu.h>

extern struct desc_struct cpu_gdt_table[NR_CPUS][GDT_ENTRIES];

struct Xgt_desc_struct {
	unsigned short size;
	unsigned long address __attribute__((packed));
} __attribute__ ((packed));

extern struct Xgt_desc_struct idt_descr, cpu_gdt_descr[NR_CPUS];

#define load_TR_desc() __asm__ __volatile__("ltr %%ax"::"a" (GDT_ENTRY_TSS*8))
#define load_LDT_desc() __asm__ __volatile__("lldt %%ax"::"a" (GDT_ENTRY_LDT*8))

/*
 * This is the ldt that every process will get unless we need
 * something other than this.
 */
extern const struct desc_struct default_ldt[];
extern void set_intr_gate(unsigned int irq, const void * addr);

#define _set_tssldt_desc(n,addr,limit,type) \
__asm__ __volatile__ ("movw %w3,0(%2)\n\t" \
	"movw %%ax,2(%2)\n\t" \
	"rorl $16,%%eax\n\t" \
	"movb %%al,4(%2)\n\t" \
	"movb %4,5(%2)\n\t" \
	"movb $0,6(%2)\n\t" \
	"movb %%ah,7(%2)\n\t" \
	"rorl $16,%%eax" \
	: "=m"(*(n)) : "a" (addr), "r"(n), "ir"(limit), "i"(type))

static inline void set_tss_desc(unsigned int cpu, const void *addr)
{
	_set_tssldt_desc(&cpu_gdt_table[cpu][GDT_ENTRY_TSS], (int)addr, 235, 0x89);
}

static inline void set_ldt_desc(unsigned int cpu, const void *addr, unsigned int size)
{
	_set_tssldt_desc(&cpu_gdt_table[cpu][GDT_ENTRY_LDT], (int)addr, ((size << 3)-1), 0x82);
}

#define LDT_entry_a(info) \
	((((info)->base_addr & 0x0000ffff) << 16) | ((info)->limit & 0x0ffff))

#define LDT_entry_b(info) \
	(((info)->base_addr & 0xff000000) | \
	(((info)->base_addr & 0x00ff0000) >> 16) | \
	((info)->limit & 0xf0000) | \
	(((info)->read_exec_only ^ 1) << 9) | \
	((info)->contents << 10) | \
	(((info)->seg_not_present ^ 1) << 15) | \
	((info)->seg_32bit << 22) | \
	((info)->limit_in_pages << 23) | \
	((info)->useable << 20) | \
	0x7000)
// 0x7100 ? see arch/i386/kernel/ldt.c grsecurity patch

#define LDT_empty(info) (\
	(info)->base_addr	== 0	&& \
	(info)->limit		== 0	&& \
	(info)->contents	== 0	&& \
	(info)->read_exec_only	== 1	&& \
	(info)->seg_32bit	== 0	&& \
	(info)->limit_in_pages	== 0	&& \
	(info)->seg_not_present	== 1	&& \
	(info)->useable		== 0	)

#if TLS_SIZE != 24
# error update this code.
#endif

static inline void load_TLS(struct thread_struct *t, unsigned int cpu)
{
#define C(i) cpu_gdt_table[cpu][GDT_ENTRY_TLS_MIN + i] = t->tls_array[i]
	C(0); C(1); C(2);
#undef C
}

static inline void clear_LDT(void)
{
	set_ldt_desc(smp_processor_id(), &default_ldt[0], 5);
	load_LDT_desc();
}

/*
 * load one particular LDT into the current CPU
 */
static inline void load_LDT (mm_context_t *pc)
{
	const void *segments = pc->ldt;
	int count = pc->size;

	if (!count) {
		segments = &default_ldt[0];
		count = 5;
	}
		
	set_ldt_desc(smp_processor_id(), segments, count);
	load_LDT_desc();
}

#endif /* !__ASSEMBLY__ */

#endif
