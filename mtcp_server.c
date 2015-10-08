
/* --- test of mtcp simple socket server
         author : ray shih
         Date: 2015-10-02T17:29:14+08:00
--- */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <netdb.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "debug.h"

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
   mctx_t mctx;
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
static char *
StatusCodeToString(int scode)
{
   switch (scode) {
   case 200:
      return "OK";
      break;

   case 404:
      return "Not Found";
      break;
   }

   return NULL;
}
/*----------------------------------------------------------------------------*/
static int
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
   struct mtcp_epoll_event ev;
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
   rd = mtcp_read(ctx->mctx, sockid, buf, HTTP_HEADER_LEN);
   //printf("rd:%d IN:%s\n",rd,buf);
   if (rd <= 0) {
      return rd;
   }
   /* just response http 200*/
   len = strlen(http_200);
   //printf("Socket %d HTTP Response: \n%s", sockid, http_200);
   sent = mtcp_write(ctx->mctx, sockid, http_200, len);
   //printf("Socket %d Sent response header: try: %d, sent: %d\n",
   //     sockid, len, sent);
   assert(sent == len);
   sv->rspheader_sent = TRUE;

   ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
   ev.data.sockid = sockid;
   mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

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
   struct mtcp_epoll_event ev;
   struct sockaddr_in saddr;
   int ret;

   /* create socket and set it as nonblocking */
   listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
   if (listener < 0) {
      printf("Failed to create listening socket!\n");
      return -1;
   }
   ret = mtcp_setsock_nonblock(ctx->mctx, listener);
   if (ret < 0) {
      printf("Failed to set socket in nonblocking mode.\n");
      return -1;
   }

   /* bind to port 80 */
   saddr.sin_family = AF_INET;
   saddr.sin_addr.s_addr = INADDR_ANY;
   saddr.sin_port = htons(8080);
   ret = mtcp_bind(ctx->mctx, listener,
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
   /* listen (backlog: 4K) */
   ret = mtcp_listen(ctx->mctx, listener, 4096);
   if (ret < 0) {
      printf("mtcp_listen() failed!\n");
      return -1;
   }

   /* wait for incoming accept events */
   ev.events = MTCP_EPOLLIN | MTCP_EPOLLET;
   ev.data.sockid = listener;
   mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

   return listener;
}
/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core)
{
   struct thread_context *ctx;

   /* affinitize application thread to a CPU core */
#if HT_SUPPORT
   mtcp_core_affinitize(core + (num_cores / 2));
#else
   mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

   ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
   if (!ctx) {
      printf("Failed to create thread context!\n");
      return NULL;
   }

   /* create mtcp context: this will spawn an mtcp thread */
   ctx->mctx = mtcp_create_context(core);
   if (!ctx->mctx) {
      printf("Failed to create mtcp context!\n");
      return NULL;
   }

   /* create epoll descriptor */
   ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
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
   mctx_t mctx;
   int listener;
   int ep;
   struct mtcp_epoll_event *events;
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
   mctx = ctx->mctx;
   ep = ctx->ep;
   events = (struct mtcp_epoll_event *)
            calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
   if (!events) {
      TRACE_ERROR("Failed to create event struct!\n");
      exit(-1);
   }
   /* Create the socket , listen and bind
   */
   listener = CreateListeningSocket(ctx);
   printf("listener:%d\n", listener);
   printf("ep:%d\n", ep);
   if (listener < 0) {
      printf("Failed to create listening socket.\n");
      exit(-1);
   }
   while (1) {
      nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
      if (nevents < 0) {
         if (errno != EINTR) {
            perror("mtcp_epoll_wait");
         }
      }
      do_accept = FALSE;
      for (i = 0; i < nevents; i++) {
         int sockid = events[i].data.sockid;
         if (sockid == listener) { //accept connection, and first process other event
            do_accept = TRUE;
         }else if (events[i].events & MTCP_EPOLLERR) { //epoll error event
            int err;
            socklen_t len = sizeof(err);

            /* error on the connection */
            TRACE_APP("[CPU %d] Error on socket %d\n", 
                  core, events[i].data.sockid);
            if (mtcp_getsockopt(mctx, events[i].data.sockid, 
                  SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
               if (err != ETIMEDOUT) {
                  fprintf(stderr, "Error on socket %d: %s\n", 
                        events[i].data.sockid, strerror(err));
               }

            } else {
               perror("mtcp_getsockopt");
            }
               mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
               mtcp_close(mctx, sockid);

         } else if (events[i].events == MTCP_EPOLLIN) { //epollin  read and write
            int ret = HandleReadEvent(ctx, events[i].data.sockid,
                                      &ctx->svars[events[i].data.sockid]);
            if (ret == 0) {
               //  printf("close connection\n");
               mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
               mtcp_close(mctx, sockid);
            }else if (ret < 0) {
               /* if not EAGAIN, it's an error */
               if (errno != EAGAIN) {
               mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
               mtcp_close(mctx, sockid);
               }
            }
         } else if (events[i].events == MTCP_EPOLLOUT) { //epollout write
            // printf("In epoll out\n");
            int LEN = strlen(http_200);
            mtcp_write(mctx, sockid, http_200, LEN);
            //   printf("out close connection\n");
            mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
            mtcp_close(mctx, sockid);
         }
      }//end for
      if (do_accept) {
         while (1) {
            int c = mtcp_accept(mctx, listener, NULL, NULL);
            if (c >= 0) {
               if (c >= MAX_FLOW_NUM) {
                  printf("Invalid socket id %d.\n", c);
                  exit(-1);
               }
               struct mtcp_epoll_event ev;
               connect++;
               //accept connection and wait EPOLLIN EVENT
               ev.events = MTCP_EPOLLIN | MTCP_EPOLLET;
               ev.data.sockid = c;
               mtcp_setsock_nonblock(ctx->mctx, c);
               mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);
               //      printf("Socket %d registered.\n", c);
            } else {  //c<0
               /*     printf("mtcp_accept() error %s\n",
                          strerror(errno));*/
               break;
            }
         }
      }
   }//end while
   mtcp_destroy_context(mctx);
   pthread_exit(NULL);

   return NULL;
}
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[] )
{
   int i;
   num_cores = GetNumCPUs();
   core_limit = num_cores;
   int cores[MAX_CPUS];

   /* initialize the mtcp context */
   if (mtcp_init("mtcp.conf")) {
      printf("Failed to initialize mtcp\n");
      exit(EXIT_FAILURE);
   }

   /* register SIGINT signal handler */
   mtcp_register_signal(SIGINT, SignalHandler);

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

   mtcp_destroy();
   exit(EXIT_SUCCESS);
}