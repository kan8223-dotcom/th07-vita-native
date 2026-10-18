#include "CoreRuntime.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <psp2/kernel/processmgr.h>
namespace {
touhou::vitacore::Logger logger=nullptr;
constexpr unsigned capacity=96;
struct Entry { SceUID id=0; int requested=0; } entries[capacity];
unsigned created=0;
void remember(SceUID id,int mask){
    if(id<0)return;
    const unsigned slot=__atomic_fetch_add(&created,1u,__ATOMIC_RELAXED);
    if(slot<capacity){entries[slot].requested=mask;__atomic_store_n(&entries[slot].id,id,__ATOMIC_RELEASE);}
}
void info(SceUID id,const char* phase,int requested){
    if(!logger)return;
    SceKernelThreadInfo s{};s.size=sizeof(s);
    const int rc=sceKernelGetThreadInfo(id,&s);
    if(rc<0){logger("[Vita cores] phase=%s tid=%08x info_rc=%08x (possibly ended)\n",phase,unsigned(id),unsigned(rc));return;}
    logger("[Vita cores] phase=%s tid=%08x name=%s requested=%08x initial=%08x affinity=%08x current_core=%d last_core=%d priority=%d\n",
        phase,unsigned(id),s.name,unsigned(requested),unsigned(s.initCpuAffinityMask),unsigned(s.currentCpuAffinityMask),s.currentCpuId,s.lastExecutedCpuId,s.currentPriority);
}
}
extern "C" SceUID __real_sceKernelCreateThread(const char*,SceKernelThreadEntry,int,SceSize,SceUInt,int,const SceKernelThreadOptParam*);
extern "C" SceUID __wrap_sceKernelCreateThread(const char* name,SceKernelThreadEntry entry,int priority,SceSize stack,SceUInt attr,int mask,const SceKernelThreadOptParam* option){
    // SDL's output worker performs no simulation. Its platform-generated name
    // identifies it independently of the engine's BGM mixer/reader workers.
    if(name&&std::strncmp(name,"SDLAudio",8)==0)mask=SCE_KERNEL_CPU_MASK_USER_2;
    const SceUID id=__real_sceKernelCreateThread(name,entry,priority,stack,attr,mask,option);
    remember(id,mask);return id;
}
namespace touhou::vitacore {
void initialize(Logger value){
    logger=value;remember(sceKernelGetThreadId(),sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()));
}
bool bind(const char* role,unsigned core){
    const SceUID id=sceKernelGetThreadId();
    const int mask=core<3?(SCE_KERNEL_CPU_MASK_USER_0<<core):0;
    const int before=sceKernelGetThreadCpuAffinityMask(id);
    const int rc=mask?sceKernelChangeThreadCpuAffinityMask(id,mask):-1;
    if(logger)logger("[Vita cores] bind role=%s tid=%08x before=%08x wanted=%08x rc=%08x now=%08x executing_core=%d\n",
        role,unsigned(id),unsigned(before),unsigned(mask),unsigned(rc),unsigned(sceKernelGetThreadCpuAffinityMask(id)),sceKernelGetCpuId());
    return rc>=0&&sceKernelGetThreadCpuAffinityMask(id)==mask;
}
void snapshot(const char* phase){
    const unsigned count=std::min(__atomic_load_n(&created,__ATOMIC_ACQUIRE),capacity);
    if(logger)logger("[Vita cores] snapshot phase=%s tracked=%u coverage=main+linked_thread_creations (kernel-internal threads not enumerated)\n",phase,count);
    for(unsigned i=0;i<count;++i){const SceUID id=__atomic_load_n(&entries[i].id,__ATOMIC_ACQUIRE);if(id>0)info(id,phase,entries[i].requested);}
}
void observe(const char*){
    // SceGxmDisplayQueue has only a 4 KiB stack. Register here without logging:
    // the modern logger's formatting buffer alone is 4 KiB. The render worker
    // queries/prints this thread at its next aggregate snapshot instead.
    const SceUID id=sceKernelGetThreadId();remember(id,sceKernelGetThreadCpuAffinityMask(id));
}
unsigned mathControls(){unsigned value;asm volatile("vmrs %0, fpscr":"=r"(value));return value&0x07c00000u;}
void inheritMathControls(unsigned parent){
    unsigned before,after;asm volatile("vmrs %0, fpscr":"=r"(before));
    const unsigned wanted=(before&~0x07c00000u)|(parent&0x07c00000u);
    asm volatile("vmsr fpscr, %0"::"r"(wanted):"memory");asm volatile("vmrs %0, fpscr":"=r"(after));
    if(logger)logger("[Vita cores] render_math parent_controls=%08x before_fpscr=%08x now_fpscr=%08x\n",parent,before,after);
}
}
