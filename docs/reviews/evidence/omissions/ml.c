/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>
int main(int argc,char**argv){
  struct rlimit r; getrlimit(RLIMIT_MEMLOCK,&r); printf("memlock soft=%llu\n",(unsigned long long)r.rlim_cur);
  int nrings=atoi(argv[1]), entries=atoi(argv[2]);
  for(int i=0;i<nrings;i++){
    struct io_uring ring; struct io_uring_params p; memset(&p,0,sizeof p);
    p.flags=IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_CQSIZE; p.cq_entries=entries*2;
    int rc=io_uring_queue_init_params(512,&ring,&p); if(rc){printf("ring %d init: %s\n",i,strerror(-rc));return 1;}
    int err; struct io_uring_buf_ring*br=io_uring_setup_buf_ring(&ring,entries,1,0,&err);
    if(!br){printf("ring %d buf_ring(%d): %s\n",i,entries,strerror(-err));return 1;}
    /* leak ring intentionally to accumulate */
  }
  printf("ok %d rings x %d pbuf entries\n",nrings,entries); return 0;
}
