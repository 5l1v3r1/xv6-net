#define _str(x) #x
#define _tostr(x) _str(x)
#define _assert_occurs " [" __FILE__ ":" _tostr(__LINE__) "] "

#define assert(x)       \
    do { if (!(x)) panic("assertion failed" _assert_occurs #x); } while (0)
