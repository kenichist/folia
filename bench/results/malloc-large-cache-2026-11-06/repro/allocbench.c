// allocbench <MB> <cycles> <touch 0|1>: malloc, touch every page (or not), free; µs per cycle and footprint.
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double fp(void){task_vm_info_data_t i;mach_msg_type_number_t n=TASK_VM_INFO_COUNT;task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&i,&n);return i.phys_footprint/1e6;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(int argc,char**argv){
  size_t sz=(size_t)(atof(argv[1])*1e6); int N=atoi(argv[2]); int touch=atoi(argv[3]);
  double best=1e18,sum=0;
  for(int c=0;c<N;c++){
    double t0=now(); char*volatile p=((void*(*volatile)(size_t))malloc)(sz);
    if(touch) for(size_t o=0;o<sz;o+=16384) p[o]=(char)c; else p[0]=1;
    ((void(*volatile)(void*))free)(p); double dt=now()-t0; sum+=dt; if(dt<best)best=dt;
  }
  printf("%6.1f MB touch=%d: mean %8.1f us  min %8.1f us  footprint %.1f MB\n",sz/1e6,touch,sum/N,best,fp());
}
