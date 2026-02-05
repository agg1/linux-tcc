// gcc -ffreestanding
int abs(int a)
{
    if (a < 0)
        return -a;
    else
        return a;
}

#ifdef __TINYC__

#ifndef __OPTIMIZE__

/* following symbols are missing with i386-tcc -U__OPTIMIZE__ -O0 */
//tcc: error: undefined symbol 'htonl'
//tcc: error: undefined symbol 'ntohl'
//tcc: error: undefined symbol 'ntohs'
//tcc: error: undefined symbol 'htons'

// grabbed from musl-libc-1.1.24

#include <linux/types.h>

static inline u16 __bswap_16(u16 __x)
{
        return __x<<8 | __x>>8;
}

static inline u32 __bswap_32(u32 __x)
{
	return __x>>24 | __x>>8&0xff00 | __x<<8&0xff0000 | __x<<24;
}

u32 htonl(u32 n)
{
	union { int i; char c; } u = { 1 };
	return u.c ? __bswap_32(n) : n;
}

u32 ntohl(u32 n)
{
	union { int i; char c; } u = { 1 };
	return u.c ? __bswap_32(n) : n;
}

u16 ntohs(u16 n)
{
	union { int i; char c; } u = { 1 };
	return u.c ? __bswap_16(n) : n;
}

u16 htons(u16 n)
{
	union { int i; char c; } u = { 1 };
	return u.c ? __bswap_16(n) : n;
}

#endif

// symbols provided by libtcc1
//tcc: error: undefined symbol '__ashrdi3'
//tcc: error: undefined symbol '__ashldi3'
//tcc: error: undefined symbol '__lshrdi3'
//tcc: error: undefined symbol '__divdi3'

#define DUMMY(x) const char x = 0xcc;

/* these symbols are needed because TCC does not suppress unused code */
DUMMY(__this_fixmap_does_not_exist)
DUMMY(__put_user_bad)
DUMMY(__get_user_bad)
// ???
DUMMY(__get_user_X)

#endif
