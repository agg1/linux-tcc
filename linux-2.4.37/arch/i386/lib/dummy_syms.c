#ifdef __TINYC__

int abs(int a)
{
    if (a < 0)
        return -a;
    else
        return a;
}

// symbols provided by libtcc1
//long long __ashrdi3(long long a, int b) {
//    return a >> b;
//}
//long long __ashldi3(long long a, int b) {
//    return a << b;
//}
//unsigned long long __lshrdi3(unsigned long long a, int b) {
//    return a >> b;
//}


#define DUMMY(x) const char x = 0xcc;

/* these symbols are needed because TCC is not smart enough to suppress unused code */
DUMMY(__this_fixmap_does_not_exist)

DUMMY(__put_user_bad)
DUMMY(__get_user_bad)
DUMMY(__get_user_X)
//DUMMY(__bad_udelay)
//DUMMY(__bad_ndelay)

// depending on kernel config further dummy symbols may be necessary
DUMMY(tcp_v4_lookup)
DUMMY(irttp_cleanup)
DUMMY(__br_lock_usage_bug)

#endif
