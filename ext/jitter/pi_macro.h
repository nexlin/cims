#ifndef __POINTI_MACRO_H_V01_
#define __POINTI_MACRO_H_V01_

#define PT_ABS(x)       ((x) >  0 ? (x) : -(x))
#define PT_MAX(x, y)    ((x) > (y)? (x) : (y))
#define PT_MIN(x, y)    ((x) < (y)? (x) : (y))

#define ALIGN8(_v)     _v+(_v%8?8-(_v%8):0)
#define ALIGNX(_v,_x)  _v+(_v%_x?_x-(_v%_x):0)

#define PRINTF_IPV(__ipv_) \
((__ipv_&0xFF000000)>>24),((__ipv_&0x00FF0000)>>16),((__ipv_&0x0000FF00)>>8),((__ipv_&0x000000FF))

#define PRINTF_IPV_NO(__ipv_) \
((__ipv_&0x0000FF)),((__ipv_&0x0000FF00)>>8),((__ipv_&0x00FF0000)>>16),((__ipv_&0xFF000000)>>24)

#define IPV_ISCLASSD(__ipv__) ((__ipv__&0xF0000000)==0xE0000000)

#endif


