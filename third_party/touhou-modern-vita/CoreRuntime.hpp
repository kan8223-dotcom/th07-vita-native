#pragma once
#ifndef TH07_HOSTREF
#include <psp2/kernel/threadmgr.h>
#else
inline int sceKernelGetCpuId(){return -1;}
#endif
namespace touhou::vitacore {
using Logger = void (*)(const char*, ...);
#ifdef TH07_HOSTREF
inline void initialize(Logger){}
inline bool bind(const char*,unsigned){return true;}
inline void observe(const char*){}
inline void snapshot(const char*){}
inline unsigned mathControls(){return 0;}
inline void inheritMathControls(unsigned){}
#else
// Affinity masks are VitaSDK USER_n masks, not Linux's (1 << n).
void initialize(Logger);
bool bind(const char* role, unsigned core);
void observe(const char* role);
void snapshot(const char* phase);
unsigned mathControls();
void inheritMathControls(unsigned);
#endif
}
