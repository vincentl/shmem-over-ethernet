/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/prctl.h>
/* Two siblings (children of same parent, like srun tasks): A creates memfd, B tries to obtain it. */
int main(void){
  int p[2]; pipe(p);
  pid_t a = fork();
  if(a==0){ int fd=memfd_create("osoe-node0",0); ftruncate(fd,4096); char*m=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); strcpy(m,"hello-from-A");
    char buf[64]; snprintf(buf,sizeof buf,"%d %d",getpid(),fd); write(p[1],buf,strlen(buf)+1); sleep(3); return 0; }
  char buf[64]; read(p[0],buf,sizeof buf); int apid,afd; sscanf(buf,"%d %d",&apid,&afd);
  pid_t b=fork();
  if(b==0){
    char path[64]; snprintf(path,sizeof path,"/proc/%d/fd/%d",apid,afd);
    int fd=open(path,O_RDWR); printf("open(%s): %s\n",path, fd<0?strerror(errno):"OK");
    if(fd>=0){char*m=mmap(0,4096,PROT_READ,MAP_SHARED,fd,0); printf("  read via /proc: %s\n",m);}
    int pfd=syscall(SYS_pidfd_open,apid,0); int fd2=syscall(SYS_pidfd_getfd,pfd,afd,0);
    printf("pidfd_getfd: %s\n", fd2<0?strerror(errno):"OK");
    return 0; }
  waitpid(b,0,0); waitpid(a,0,0); return 0;
}
