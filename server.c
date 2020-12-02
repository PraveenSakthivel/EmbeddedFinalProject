#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include "util.h"
#include "common.h"
#define IO_BUFSIZE 8192
#define	MAXLINE	 8192

typedef struct {
    int io_fd;                /* descriptor for this internal buf */
    int io_cnt;               /* unread bytes in internal buf */
    char *io_bufptr;          /* next unread byte in internal buf */
    char io_buf[IO_BUFSIZE]; /* internal buffer */
} io_t;

pthread_mutex_t messageLock;
char message[200]= "";

static ssize_t io_read(io_t *p, char *usrbuf, size_t n)
{
    int cnt;

    while (p->io_cnt <= 0) {  /* refill if buf is empty */
	p->io_cnt = read(p->io_fd, p->io_buf, 
			   sizeof(p->io_buf));
	if (p->io_cnt < 0) {
	    if (errno != EINTR) /* interrupted by sig handler return */
		return -1;
	}
	else if (p->io_cnt == 0)  /* EOF */
	    return 0;
	else 
	    p->io_bufptr = p->io_buf; /* reset buffer ptr */
    }

    cnt = n;          
    if (p->io_cnt < n)   
	cnt = p->io_cnt;
    memcpy(usrbuf, p->io_bufptr, cnt);
    p->io_bufptr += cnt;
    p->io_cnt -= cnt;
    return cnt;
}

ssize_t io_readline(io_t *p, void *usrbuf, size_t maxlen) 
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) { 
	if ((rc = io_read(p, &c, 1)) == 1) {
	    *bufp++ = c;
	    if (c == '\n')
		break;
	} else if (rc == 0) {
	    if (n == 1)
		return 0; /* EOF, no data read */
	    else
		break;    /* EOF, some data was read */
	} else
	    return -1;	  /* error */
    }
    *bufp = 0;
    return n;
}

void io_readinit(io_t *p, int fd) 
{
    p->io_fd = fd;  
    p->io_cnt = 0;  
    p->io_bufptr = p->io_buf;
}

void *clientServiceThread(void *args){
    int connfd = *(int *)args;
    free(args);

    int flags = fcntl(connfd, F_GETFL);
    fcntl(connfd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in peerAddr;
    socklen_t peerAddrLen = sizeof(peerAddr);
    getpeername(connfd, &peerAddr, &peerAddrLen);
    char *peerIP = inet_ntoa(peerAddr.sin_addr);
    printf("Connection established with %s\n", peerIP);
    
    char *request;                  /* HTTP request from client */
    char *rest_of_request;          /* Beginning of second HTTP request header line */
    int request_len;                /* Total size of HTTP request */
    int i, n;                       /* General index and counting variables */
    int realloc_factor;             /* Used to increase size of request buffer if necessary */  

    char hostname[MAXLINE];         /* Hostname extracted from request URI */
    char pathname[MAXLINE];         /* Content pathname extracted from request URI */
    int serverport;                 /* Port number extracted from request URI (default 80) */
    char log_entry[MAXLINE];        /* Formatted log entry */
    int error = 0;
    io_t io;                      /* buffer*/
    char buf[MAXLINE];              /* General I/O buffer */
    request = (char *)malloc(MAXLINE);
    request[0] = '\0';
    realloc_factor = 2;
    request_len = 0;
    io_readinit(&io, connfd);
    while (1) {
        if ((n = io_readline(&io, buf, MAXLINE)) <= 0) {
            error = 1;	//Used to fix a bug
            printf("process_request: client issued a bad request (1).\n");
            close(connfd);
            free(request);
                break;
        }

        /* If not enough room in request buffer, make more room */
        if (request_len + n + 1 > MAXLINE)
            realloc(request, MAXLINE*realloc_factor++);

        strcat(request, buf);
        request_len += n;

        /* An HTTP requests is always terminated by a blank line */
        if (strcmp(buf, "\r\n") == 0)
            break;
    }

    /*Check if GET Request 
    Request will return the current message */
    if (strncmp(request, "GET ", strlen("GET ")) == 0) {
        write(connfd,"HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 12\n\n",strlen("HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 12\n\n"));
        write(connfd,message, strlen(message));
        fflush(connfd);
        close(connfd);
        free(request);
    }
    /* Check if POST Request
    Request will update the current message */
    else if (strncmp(request, "POST ", strlen("POST ")) == 0) {
        printf("CONN");
        close(connfd);
        free(request);
    }

    close(connfd);
    return (void *)0;
}


int main(int argc, char** argv){

    if (argc != 2){
        errprint("usage: ./motdServer port\n");
        return 1;
    }

    int port = atoi(argv[1]);
    if (port < 1024){
        errprint("invalid port, choose one over 1024\n");
        return 1;
    }

    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0){
        errprint("socket creation failed\n");
        return 1;
    }

    int flags = fcntl(listenfd, F_GETFL);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0){
        errprint("failed to bind to port %d\n", port);
        return 1;
    }

    if (listen(listenfd, 10) < 0){
        errprint("failed to listen\n");
        return 1;
    }

    printf("listening on port %d...\n", port);

    while (true) {
        int connfd = accept(listenfd, (struct sockaddr*)NULL, NULL);
        if (connfd == -1 && errno == EWOULDBLOCK){
            usleep(250000);
        } else if (connfd == -1){
            errprint("accept failed\n");
        } else {
            int *fdPtr = (int *)malloc(sizeof(int));
            *fdPtr = connfd;
            pthread_t newThread;
            pthread_create(&newThread, NULL, clientServiceThread, (void*)fdPtr);
            pthread_detach(newThread);
        }
    }

    return 0;
}

