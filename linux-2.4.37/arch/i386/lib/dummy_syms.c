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
//long long __ashrdi3(long long a, int b)
//long long __ashldi3(long long a, int b)
//unsigned long long __lshrdi3(unsigned long long a, int b)

#define DUMMY(x) const char x = 0xcc;

/* these symbols are needed because TCC does not suppress unused code */
DUMMY(__this_fixmap_does_not_exist)
DUMMY(__put_user_bad)
DUMMY(__get_user_bad)
// ???
DUMMY(__get_user_X)

#endif
