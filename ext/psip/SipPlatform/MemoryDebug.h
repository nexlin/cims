#ifndef _MEMORY_DEBUG_H_
#define _MEMORY_DEBUG_H_

// 메모리 누수 검사를 위해서 아래의 주석을 해제한 후, 빌드하고 실행 & 종료하면 메모리 누수가 발생한 소스 코드가 출력된다.
//#define MEMORY_DEBUG

#ifdef WIN32
#ifdef _DEBUG
#ifdef MEMORY_DEBUG

#ifdef malloc
#undef malloc
#endif

#define malloc(s) (_malloc_dbg(s, _NORMAL_BLOCK, __FILE__, __LINE__ ))

#ifdef calloc
#undef calloc
#endif

#define calloc(c, s) (_calloc_dbg(c, s, _NORMAL_BLOCK, __FILE__, __LINE__ ))

#ifdef realloc
#undef realloc
#endif

#define realloc(p, s) (_realloc_dbg(p, s, _NORMAL_BLOCK, __FILE__, __LINE__ ))

#ifdef _expand
#undef _expand
#endif

#define _expand(p, s) (_expand_dbg(p, s, _NORMAL_BLOCK, __FILE__, __LINE__  ))

#ifdef free
#undef free
#endif

#define free(p) (_free_dbg(p, _NORMAL_BLOCK))

#ifdef _msize
#undef _msize
#endif

#define _msize(p) (_msize_dbg(p, _NORMAL_BLOCK))

#define new new( _NORMAL_BLOCK, __FILE__, __LINE__ )

#endif
#endif
#endif

#endif
