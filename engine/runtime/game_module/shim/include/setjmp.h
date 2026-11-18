/* slim prototype: spu_context.h declares SPU_SETJMP/SPU_LONGJMP macros; the lifted code never calls them
   (the engine does). The type has the UCRT's size (16 x 16 bytes on x64). */
#pragma once
typedef struct { __declspec(align(16)) unsigned long long _p[2]; } _SETJMP_FLOAT128;
typedef _SETJMP_FLOAT128 jmp_buf[16];
#ifdef __cplusplus
extern "C" {
#endif
int _setjmp(jmp_buf);
__declspec(noreturn) void longjmp(jmp_buf, int);
#ifdef __cplusplus
}
#endif
#define setjmp(b) _setjmp(b)
