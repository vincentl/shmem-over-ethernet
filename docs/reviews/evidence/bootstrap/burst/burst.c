/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Rendezvous burst model: N "PEs" connect simultaneously to one "rank 0"
 * listener (backlog B) and send a 16-byte hello. The server accepts and reads
 * each hello serially (per-connection work W microseconds, modelling
 * table insertion/logging). Reports time to collect all hellos and the
 * slowest single connection. Clients spread over 127.0.0.0/16 source addresses
 * so the ephemeral-port range is per source, as across real nodes. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int N, B, W, PER_SRC = 256;
static int port;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}

static void *server(void *arg){
    int ls=(int)(long)arg; int got=0; char buf[16];
    while(got<N){
        int s=accept(ls,NULL,NULL); if(s<0){perror("accept");continue;}
        size_t r=0; while(r<16){ssize_t k=read(s,buf+r,16-r); if(k<=0)break; r+=k;}
        if(W) usleep(W);
        got++;
        /* keep fd open: rank 0 holds every connection until the broadcast */
    }
    return NULL;
}

int main(int argc,char**argv){
    N=atoi(argv[1]); B=atoi(argv[2]); W=argc>3?atoi(argv[3]):0;
    struct rlimit rl={2*N+100,2*N+100}; if(setrlimit(RLIMIT_NOFILE,&rl)){perror("setrlimit");return 1;}
    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    bind(ls,(void*)&a,sizeof a); listen(ls,B); socklen_t al=sizeof a; getsockname(ls,(void*)&a,&al); port=ntohs(a.sin_port);
    pthread_t th; pthread_create(&th,NULL,server,(void*)(long)ls);
    int ep=epoll_create1(0);
    int *fd=calloc(N,sizeof*fd); double *t0=calloc(N,sizeof*t0); double worst=0; int done=0, errs=0;
    double start=now();
    for(int i=0;i<N;i++){
        fd[i]=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
        struct sockaddr_in src={.sin_family=AF_INET,.sin_addr.s_addr=htonl(0x7f010000+ (i/PER_SRC)+1)};
        if(bind(fd[i],(void*)&src,sizeof src)){perror("bind");return 1;}
        t0[i]=now();
        int r=connect(fd[i],(void*)&a,sizeof a);
        if(r<0&&errno!=EINPROGRESS){perror("connect");errs++;continue;}
        struct epoll_event ev={.events=EPOLLOUT,.data.u32=i}; epoll_ctl(ep,EPOLL_CTL_ADD,fd[i],&ev);
    }
    struct epoll_event evs[1024];
    while(done+errs<N){
        int n=epoll_wait(ep,evs,1024,200000); if(n<=0){fprintf(stderr,"timeout\n");break;}
        for(int k=0;k<n;k++){int i=evs[k].data.u32; int e=0; socklen_t l=sizeof e; getsockopt(fd[i],SOL_SOCKET,SO_ERROR,&e,&l);
            epoll_ctl(ep,EPOLL_CTL_DEL,fd[i],NULL);
            if(e){errs++;continue;}
            char hello[16]={0}; if(write(fd[i],hello,16)!=16) errs++;
            done++;}
    }
    pthread_join(th,NULL);
    double end=now();
    for(int i=0;i<N;i++){ /* nothing more per-conn */ }
    printf("N=%d backlog=%d work_us=%d  all hellos read in %.3f s  connect errors=%d\n",N,B,W,end-start,errs);
    return 0;
}
