// gcc -ffreestanding
int abs(int a)
{
    if (a < 0)
        return -a;
    else
        return a;
}

#ifdef __TINYC__

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
