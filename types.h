typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;
typedef uint pde_t;

// import form jos: inc/types.h
// Explicitly-sized versions of integer types
#ifndef _SYS_TYPES_H
typedef __signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
#endif

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof(a[0]))
