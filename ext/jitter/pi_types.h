#ifndef __PI_BASIC_TYPES_H_V01_
#define __PI_BASIC_TYPES_H_V01_

#include <sys/types.h>

// pdk types
typedef unsigned char    pdkbyte ;
typedef unsigned char    pdkub8  ;
typedef char     pdkchar ;
typedef char     pdksb8  ;

typedef short    pdksb16 ;
typedef unsigned short   pdkub16 ;

typedef int   pdksb32 ;
typedef unsigned int   pdkub32 ;

typedef long  pdksb64 ;
typedef unsigned long pdkub64 ;

typedef pdkub32  pdkmagic_t ;
typedef pdkub64  pdkaddr_t ;


// pi types
typedef unsigned char    pibyte ;
typedef unsigned char    piub8  ;
typedef char     pichar ;
typedef char     pisb8  ;

typedef short    pisb16 ;
typedef unsigned short   piub16 ;

typedef int   pisb32 ;
typedef unsigned int   piub32 ;

typedef long  pisb64 ;
typedef unsigned long piub64 ;

typedef pdkub32  pimagic_t ;
typedef pdkub64  piaddr_t ;


#define POOLID_NULL   (-1)
#define ALLOCID_NULL  (-1) 
#define LISTLINK_NULL (-1)

#define DN_NUMBER_MAX (31)

#endif
