#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double fp(void){task_vm_info_data_t i;mach_msg_type_number_t n=TASK_VM_INFO_COUNT;task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&i,&n);return i.phys_footprint/1e6;}
int main(int argc,char**argv){
  size_t target=(size_t)atof(argv[1])*1e6; int grow=atoi(argv[2]);
  int N=argc>3?atoi(argv[3]):12; for(int c=0;c<N;c++){
    char*p; size_t cap=65536,len=0;
    if(grow){ p=malloc(cap); while(len<target){ size_t add=65536; if(len+add>cap){cap*=2;p=realloc(p,cap);} memset(p+len,c+1,add); len+=add; } }
    else { p=malloc(target); memset(p,c+1,target); }
    free(p);
    if(c%(N/6)==N/6-1) printf("cycle %2d footprint %.1f MB\n",c+1,fp());
  }
}
