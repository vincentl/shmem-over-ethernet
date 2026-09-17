/* Does an io_uring send on a TCP socket with a small SO_SNDBUF complete short?
 * And does MSG_WAITALL make io_uring retry internally to full length? */
#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static void run(int flags, int nonblock){
    int ls=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    bind(ls,(void*)&a,sizeof a); listen(ls,1); socklen_t al=sizeof a; getsockname(ls,(void*)&a,&al);
    int cs=socket(AF_INET,SOCK_STREAM|(nonblock?SOCK_NONBLOCK:0),0); int sz=4096;
    setsockopt(cs,SOL_SOCKET,SO_SNDBUF,&sz,sizeof sz);
    connect(cs,(void*)&a,sizeof a); int ss=accept(ls,NULL,NULL);
    setsockopt(ss,SOL_SOCKET,SO_RCVBUF,&sz,sizeof sz);
    struct io_uring ring; io_uring_queue_init(8,&ring,0);
    size_t len=1<<20; char *buf=malloc(len); memset(buf,'x',len);
    int short_cqes=0, total=0;
    for(int i=0;i<20;i++){
        struct io_uring_sqe*sqe=io_uring_get_sqe(&ring); io_uring_prep_send(sqe,cs,buf,len,flags); io_uring_submit(&ring);
        /* drain receiver concurrently so the send can make progress */
        struct io_uring_cqe*cqe; char rb[65536]; long got=0;
        while(io_uring_peek_cqe(&ring,&cqe)!=0){ ssize_t k=recv(ss,rb,sizeof rb,MSG_DONTWAIT); if(k>0)got+=k; }
        if(cqe->res!=(int)len) short_cqes++;
        if(i<3) printf("  send %d: res=%d\n",i,cqe->res);
        total+=cqe->res>0?cqe->res:0; io_uring_cqe_seen(&ring,cqe);
        /* drain the remainder so the next iteration starts empty */
        long want=(long)total; (void)want;
        usleep(20000); for(;;){ ssize_t k=recv(ss,rb,sizeof rb,MSG_DONTWAIT); if(k<=0)break; }
    }
    printf("flags=%s nonblock=%d: %d/20 sends completed short\n", flags?"MSG_WAITALL":"0", nonblock, short_cqes);
    io_uring_queue_exit(&ring); close(cs); close(ss); close(ls); free(buf);
}
int main(int c,char**v){ setvbuf(stdout,NULL,_IONBF,0); run(atoi(v[1])?MSG_WAITALL:0, atoi(v[2])); return 0; }
