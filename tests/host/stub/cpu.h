/* 主機測試用的替身:取代 include/cpu.h。每條測試執行緒扮演一顆 CPU */
#ifndef HOST_STUB_CPU_H
#define HOST_STUB_CPU_H
#define MAX_CPUS 8
struct cpu { int id; };
extern __thread struct cpu host_cpu;
extern int cpu_count;
static inline struct cpu *this_cpu(void) { return &host_cpu; }
#endif
