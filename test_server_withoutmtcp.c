
/* --- test of simple poll event socket server without mtcp
         author : ray shih
         Date: 2015-10-05T11:34:04+08:00
--- */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <netdb.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#if 0
#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "debug.h"
#endif
#include <sys/epoll.h>
#include <sys/socket.h>
#include <errno.h>
#define _GNU_SOURCE
#define __USE_GNU
#ifdef __USE_GNU
/* Access macros for `cpu_set'.  */
#define CPU_SETSIZE __CPU_SETSIZE
#define CPU_SET(cpu, cpusetp)   __CPU_SET (cpu, cpusetp)
#define CPU_CLR(cpu, cpusetp)   __CPU_CLR (cpu, cpusetp)
#define CPU_ISSET(cpu, cpusetp) __CPU_ISSET (cpu, cpusetp)
#define CPU_ZERO(cpusetp)       __CPU_ZERO (cpusetp)
#endif
#include <unistd.h>
#include <errno.h>
#include <numa.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <assert.h>


#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)
#define HTTP_HEADER_LEN 1024
#define MAX_EVENTS (MAX_FLOW_NUM * 3)
#define MAX_CPUS 8
#ifndef TRUE
#define TRUE (1)
#endif
#define MIN(a, b) ((a)<(b)?(a):(b))
#ifndef FALSE
#define FALSE (0)
#endif

static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
char *http_200 = "HTTP/1.0 200 OK\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n"
                 "Content-Type: text/html\r\n"
                 "\r\n"
                 "<html><body><h1>200 OK</h1>\nEverything is fine.\n</body></html>\n";

struct thread_context
{
   int ep;
   struct server_vars *svars;
};
/*----------------------------------------------------------------------------*/
struct server_vars
{
   char request[HTTP_HEADER_LEN];
   int recv_len;
   int request_len;
   long int total_read, total_sent;
   uint8_t done;
   uint8_t rspheader_sent;
   uint8_t keep_alive;

   int fidx;                  // file cache index
   char fname[128];           // file name
   long int fsize;               // file size
};
/*----------------------------------------------------------------------------*/
static int setnonblock(int fd)
{
   int flags;

   flags = fcntl(fd, F_GETFL);
   if (flags < 0) {
      return flags;
   }
   flags |= O_NONBLOCK;
   if (fcntl(fd, F_SETFL, flags) < 0) {
      return -1;
   }
   return 0;
}
/*----------------------------------------------------------------------------*/
static int
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
   struct epoll_event ev;
   char buf[HTTP_HEADER_LEN];
   char response[HTTP_HEADER_LEN];
   int scode;                 // status code
   time_t t_now;
   char t_str[128];
   char keepalive_str[128];
   int rd;
   int i;
   int len;
   int sent;

   /* HTTP request handling */
   rd = read(sockid, buf, HTTP_HEADER_LEN);
   // printf("rd:%d IN:%s\n",rd,buf);
   if (rd <= 0) {
      return rd;
   }
   /* just response http 200*/
   len = strlen(http_200);
   // printf("Socket %d HTTP Response: \n%s", sockid, http_200);
   sent = write(sockid, http_200, len);
   /*printf("Socket %d Sent response header: try: %d, sent: %d\n",
        sockid, len, sent);*/
   assert(sent == len);
   sv->rspheader_sent = TRUE;

   ev.events = EPOLLIN | EPOLLOUT;
   ev.data.fd = sockid;
   epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sockid, &ev);

   return rd;
}
/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
   int i;

   for (i = 0; i < core_limit; i++) {
      if (app_thread[i] == pthread_self()) {
         //TRACE_INFO("Server thread %d got SIGINT\n", i);
      } else {
         pthread_kill(app_thread[i], signum);
      }
   }
}
/*----------------------------------------------------------------------------*/
int
CreateListeningSocket(struct thread_context *ctx)
{
   int listener;
   struct epoll_event ev;
   struct sockaddr_in saddr;
   int ret;

   /* create socket and set it as nonblocking */
   listener = socket( AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
   if (listener < 0) {
      printf("Failed to create listening socket!\n");
      return -1;
   }
   // printf("In create, listen sockid:%d\n");

   /* bind to port 8080 */
   saddr.sin_family = AF_INET;
   saddr.sin_addr.s_addr = INADDR_ANY;
   saddr.sin_port = htons(8080);
   ret = bind(listener,
              (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
   struct sockaddr_in* pV4Addr = (struct sockaddr_in*)&saddr;
   int ipAddr = pV4Addr->sin_addr.s_addr;
   char ipstr[INET_ADDRSTRLEN];
   inet_ntop( AF_INET, &ipAddr, ipstr, INET_ADDRSTRLEN );
   if (ret < 0) {
      printf("Failed to bind to the listening socket!\n");
      return -1;
   }
   //printf("listen port:%u\n",saddr.sin_port);
   /* listen (backlog: 8K) */
   ret = listen(listener, 8192);
   if (ret < 0) {
      printf("listen() failed!\n");
      return -1;
   }

   /* wait for incoming accept events */
   ev.events = EPOLLIN;
   ev.data.fd = listener;
   epoll_ctl(ctx->ep, EPOLL_CTL_ADD, listener, &ev);

   return listener;
}
/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core)
{
   struct thread_context *ctx;

   cpu_set_t cpus;
   size_t n;
   int ret;

   n = GetNumCPUs();

   if (core < 0 || core >= (int) n) {
      errno = -EINVAL;
      exit(-1);
   }

   CPU_ZERO(&cpus);
   CPU_SET((unsigned)core, &cpus);
   ret = sched_setaffinity(Gettid(), sizeof(cpus), &cpus);

   ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
   if (!ctx) {
      printf("Failed to create thread context!\n");
      return NULL;
   }

   /* create epoll descriptor */
   ctx->ep = epoll_create(MAX_EVENTS);
   if (ctx->ep < 0) {
      printf("Failed to create epoll descriptor!\n");
      return NULL;
   }

   /* allocate memory for server variables */
   ctx->svars = (struct server_vars *)
                calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
   if (!ctx->svars) {
      printf("Failed to create server_vars struct!\n");
      return NULL;
   }

   return ctx;
}
/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg) //arg = core num
{
   int core = *(int *)arg;
   struct thread_context *ctx;

   int listener;
   int ep;
   struct epoll_event *events;
   int nevents;
   int i, ret;
   int do_accept;
   int connect = 0;
   /* initialization */
   ctx = InitializeServerThread(core);
   if (!ctx) {
      printf("Failed to initialize server thread.\n");
      return NULL;
   }
   ep = ctx->ep;
   events = (struct epoll_event *)
            calloc(MAX_EVENTS, sizeof(struct epoll_event));
   if (!events) {
      printf("Failed to create event struct!\n");
      exit(-1);
   }
   /* Create the socket , listen and bind
   */
   listener = CreateListeningSocket(ctx);
   printf("listener:%d\n", listener);
   printf("epoll_fd_id:%d\n", ep);
   if (listener < 0) {
      printf("Failed to create listening socket.\n");
      exit(-1);
   }
   while (1) {
      nevents = epoll_wait(ep, events, MAX_EVENTS, -1);
      if (nevents < 0) {
         if (errno != EINTR) {
            perror("epoll_wait");
         }
      }
      do_accept = FALSE;
      //  printf("events:%d\n",nevents);
      for (i = 0; i < nevents; i++) {
         int sockid = events[i].data.fd;
         if (sockid == listener) { //accept connection, and first process other event
            do_accept = TRUE;
         } else if (events[i].events & EPOLLERR) { //epoll error event
            int err;
            socklen_t len = sizeof(err);

            /* error on the connection */
            printf("[CPU %d] Error on socket %d\n",
                   core, events[i].data.fd);
            if (getsockopt(events[i].data.fd,
                           SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
               if (err != ETIMEDOUT) {
                  fprintf(stderr, "Error on socket %d: %s\n",
                          events[i].data.fd, strerror(err));
               }

            } else {
               perror("getsockopt");
            }
            epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
            close(sockid);

         } else if (events[i].events == EPOLLIN) { //epollin  read and write
            int ret = HandleReadEvent(ctx, events[i].data.fd,
                                      &ctx->svars[events[i].data.fd]);
            if (ret == 0) {
               //printf("epollin close connection\n");
               epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
               close(sockid);
            } else if (ret < 0) {
               /* if not EAGAIN, it's an error */
               if (errno != EAGAIN) {
                  epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
                  close(sockid);
               }
            }
         } else if (events[i].events == EPOLLOUT) { //epollout write
            // printf("In epoll out\n");
            int LEN = strlen(http_200);
            int sent = write(sockid, http_200, LEN);
            //printf("epoll out close connection\n");
            epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sockid, NULL);
            close(sockid);
         }
      }//end for
      if (do_accept) {
         while (1) {
            int c = accept(listener, NULL, NULL);
            if (c >= 0) {
               if (c >= MAX_FLOW_NUM) {
                  printf("Invalid socket id %d.\n", c);
                  exit(-1);
               }
               if (setnonblock(c) < 0) {
                  warn("failed to set client socket non-blocking");
               }
               struct epoll_event ev;
               connect++;
               // printf("New connection %d accepted. total: %d\n", c,connect);
               ev.events = EPOLLIN | EPOLLET;
               ev.data.fd = c;
               epoll_ctl(ctx->ep, EPOLL_CTL_ADD, c, &ev);
               //      printf("Socket %d registered.\n", c);
            } else {
               if (errno != EAGAIN && errno != ECONNABORTED
                     && errno != EPROTO && errno != EINTR) {
                  printf("accept() error %s\n",
                         strerror(errno));
               }
               break;
            }
         }
      }
   }//end while
   pthread_exit(NULL);

   return NULL;
}
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[] )
{
   int i;
   num_cores = GetNumCPUs();
   core_limit = 1;
   int cores[MAX_CPUS];
   //printf("core_limits:%d\n", core_limit);
   for (i = 0; i < core_limit; i++) {
      cores[i] = i;
      if (pthread_create(&app_thread[i],
                         NULL, RunServerThread, (void *)&cores[i])) {
         perror("pthread_create");
         printf("Failed to create server thread.\n");
         exit(-1);
      }
   }
   for (i = 0; i < core_limit; i++) {
      pthread_join(app_thread[i], NULL);
   }

   exit(EXIT_SUCCESS);
}
