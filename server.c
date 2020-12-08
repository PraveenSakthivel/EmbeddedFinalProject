#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
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
#include <netinet/tcp.h>
#define IO_BUFSIZE 8192
#define	MAXLINE	 8192
#define MSGLEN 1000
#define dprint(...) fprintf(stderr, __VA_ARGS__)

typedef struct {
    int io_fd;                /* descriptor for this internal buf */
    int io_cnt;               /* unread bytes in internal buf */
    char *io_bufptr;          /* next unread byte in internal buf */
    char io_buf[IO_BUFSIZE]; /* internal buffer */
} io_t;

pthread_rwlock_t messageLock = PTHREAD_RWLOCK_INITIALIZER;
char message[MSGLEN] = "";


//Read function that accounts for specific cases
static ssize_t io_read(io_t *p, char *usrbuf, size_t n)
{
    int cnt;

    while (p->io_cnt <= 0) {  /* refill if buf is empty */
	p->io_cnt = read(p->io_fd, p->io_buf, 
			   sizeof(p->io_buf));
	if (p->io_cnt <= 0)  /* EOF */
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

//Wrapper function that reads a line
ssize_t io_readline(io_t *p, void *usrbuf, size_t maxlen) 
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) { 
	if ((rc = io_read(p, &c, 1)) == 1) {
	    *bufp++ = c;
	    if (c == '\n')
		break;
	} else{
	    if (n == 1)
		    return 0; /* EOF, no data read */
	    else
		    break;    /* EOF, some data was read */
	}
    }
    *bufp = 0;
    return n;
}

//init our io object
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
    int request_len;                /* Total size of HTTP request */
    int i, n;                       /* General index and counting variables */

    io_t io;                      /* buffer*/
    char buf[MAXLINE];              /* General I/O buffer */
    request = (char *)malloc(MAXLINE);
    request[0] = '\0';
    request_len = 0;
    io_readinit(&io, connfd);
    while (1) {
        if ((n = io_readline(&io, buf, MAXLINE)) <= 0) {
            printf("process_request: client issued a bad request (1).\n");
            close(connfd);
            free(request);
                break;
        }

        /* If out of space, double the buffer */
        if (request_len + n + 1 > MAXLINE)
            realloc(request, sizeof(request)*2);

        strcat(request, buf);
        request_len += n;

        /* An HTTP requests are always finsih with blank line */
        if (strcmp(buf, "\r\n") == 0)
            break;
    }

    /*Check if GET Request 
    Request will return the current message */
    if (strncmp(request, "GET ", strlen("GET ")) == 0) {
        pthread_rwlock_rdlock(&messageLock);
        int len = strlen(message);
        char header[60 + len];
        sprintf(header,"HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: %d\n\n",len);
        write(connfd,header,strlen(header));
        write(connfd,message, len);
        pthread_rwlock_unlock(&messageLock);
        free(request);
    }
    /* Check if POST Request
    Request will update the current message */
    else if (strncmp(request, "POST ", strlen("POST ")) == 0) {
        char msgbuf[MSGLEN];
        char tmp[MSGLEN];
        int msgLen = 0;
        tmp[0] = '\0';
        while(true){
            n = io_readline(&io, msgbuf, MSGLEN);
            if(n <= 0){
                break;
            }
            //Check if msg is too long, if so truncate
            else if(msgLen + n > MSGLEN){
                n = MSGLEN - msgLen;
                
                strncat(tmp, msgbuf,n);
                break;
            }
            strncat(tmp, msgbuf, n);
            msgLen += n;
        }
        pthread_rwlock_wrlock(&messageLock);
        memcpy(message,tmp,MSGLEN);
        pthread_rwlock_unlock(&messageLock);
        write(connfd,"HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 15\n\nMessage Updated", strlen("HTTP/1.1 200 OK\nContent-Type: text/plain\nContent-Length: 15\n\nMessage Updated"));
        free(request);
    }

    close(connfd);
    return (void *)0;
}


int main(int argc, char** argv){

    if (argc != 2){
        printf("usage: ./motdServer port\n");
        return 1;
    }

    int port = atoi(argv[1]);
    if (port < 1024){
        printf("invalid port, choose one over 1024\n");
        return 1;
    }

    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0){
        printf("socket creation failed\n");
        return 1;
    }

    int flags = fcntl(listenfd, F_GETFL);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0){
        printf("failed to bind to port %d\n", port);
        return 1;
    }

    if (listen(listenfd, 10) < 0){
        printf("failed to listen\n");
        return 1;
    }

    printf("listening on port %d...\n", port);

    while (true) {
        int connfd = accept(listenfd, (struct sockaddr*)NULL, NULL);
        if (connfd == -1 && errno == EWOULDBLOCK){
            usleep(250000);
        } else if (connfd == -1){
            printf("accept failed\n");
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

