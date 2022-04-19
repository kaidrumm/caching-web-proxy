#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <pthread.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <sys/poll.h>
#include <poll.h>

#define MAXBUF   8192  /* max I/O buffer size */
#define MAXLINE  256  /* max text line length */
#define LISTENQ  1024  /* second argument to listen() */
#define HASHMOD  128

struct file_index {
    char fname[MAXLINE];
    clock_t timestamp;
    pthread_mutex_t file_mutex;
    struct file_index *next;
};

pthread_mutex_t timeouts_mutex;
int timeout_limit_g;
struct file_index *timeouts_g[HASHMOD]; // hash talbe

void *thread(void *vargp);
char **strsplit(char *str, char delim);
int open_listenfd(int port);
void http_err(int connfd, int type, char *httpversion, FILE *log);
int validate_hostname(char *hostname, char *port, int clientfd, FILE *log);
bool parse(int connfd, char *msg, FILE *log);
void forward_messages(int sk1, int sk2, char *msg, char *host, char *uri, FILE *log);
void read_whilealive(int clientfd, FILE *log);
bool on_blacklist(char *hostname_ip, FILE *log);
int my_hashvalue(char *uri, FILE *log);
struct file_index *findex_retrieve_create_lock(char *uri, bool *new, FILE *log);

/*
Listen on port and accept incoming connections;
Spin a new thread for each new connection 
*/
int    main(int argc, char **argv){
    int port;
    int timeout;
    int listenfd;
    int *connfdp;
    int clientlen = sizeof(struct sockaddr_in);
    struct sockaddr_in clientaddr;
    pthread_t tid; 

    if (argc != 3) {
	    fprintf(stderr, "usage: %s <port> <timeout>\n", argv[0]);
	    exit(0);
    }
    port = atoi(argv[1]);
    timeout = atoi(argv[2]);
    timeout_limit_g = timeout;
    bzero(timeouts_g, sizeof(struct file_index *)*HASHMOD);
    pthread_mutex_init(&timeouts_mutex, NULL);

    listenfd = open_listenfd(port);
    if (listenfd < 0){
        perror("listenfd");
        return 0;
    }
    while (1) {
        connfdp = malloc(sizeof(int));
        *connfdp = accept(listenfd, (struct sockaddr*)&clientaddr, (socklen_t *)&clientlen);
        pthread_create(&tid, NULL, thread, connfdp);
    }
    return 0;
}

bool    wait_lock(pthread_mutex_t *lock, char *name, FILE *log){
    fprintf(log, "Blocking to lock %s\n", name);
    fflush(log);
    if(pthread_mutex_lock(lock) !=0){
        fprintf(log, "Error locking: %s\n", strerror(errno));
        return false;
    }
    fprintf(log, "Locked %s\n", name);
    fflush(log);
    return true;
}

bool    unlock(pthread_mutex_t *lock, char *name, FILE *log){
    fprintf(log, "Unlocking %s\n", name);
    fflush(log);
    if(pthread_mutex_unlock(lock) !=0){
        fprintf(log, "Error unlocking: %s\n", strerror(errno));
        return false;
    }
    fprintf(log, "Unlocked %s\n", name);
    fflush(log);
    return true;
}

/*
* Given a URI, retrieve or create its index entry.
* Sets timestamp to now.
*/
struct file_index *findex_retrieve_create_lock(char *uri, bool *new, FILE *log){
    fprintf(log, "Retrieving index for %s\n", uri);
    fflush(log);
    struct file_index *fi;
    struct file_index *prev = NULL;
    int hashvalue = my_hashvalue(uri, log);
    *new = false;

    wait_lock(&timeouts_mutex, "index", log);
    fi = timeouts_g[hashvalue];
    while(fi != NULL){
        if(strcmp(fi->fname, uri) == 0){
            fprintf(log, "Index entry found for URI %s\n", uri);
            fflush(log);
            unlock(&timeouts_mutex, "index", log);
            wait_lock(&(fi->file_mutex), uri, log);
            return fi;
        } else {
            fprintf(log, "Using index linked list\n");
            fflush(log);
            prev = fi;
            fi = fi->next;
        }
    }
    fi = (struct file_index *)malloc(sizeof(struct file_index));
    strcpy(fi->fname, uri);
    fi->timestamp = clock();
    fi->next = NULL;
    if(prev)
        prev->next = fi;
    if(pthread_mutex_init(&(fi->file_mutex), NULL)!= 0)
        fprintf(log, "Error initializing mutex: %s\n", strerror(errno));
    wait_lock(&(fi->file_mutex), uri, log); // Should always succeed
    timeouts_g[hashvalue] = fi;
    unlock(&timeouts_mutex, "index", log);
    *new = true;
    fprintf(log, "Added new URI entry to index: %s\n", uri);
    return fi;
}

bool recent(struct file_index *fi, FILE *log){
    fprintf(log, "Checking if recent\n");
    fflush(log);
    int elapsed;

    clock_t lastupdated = fi->timestamp;
    clock_t now = clock();

    elapsed = (now - lastupdated) / CLOCKS_PER_SEC;
    fprintf(log, "File updated at %lu; now %lu; %i seconds old\n", lastupdated, now, elapsed);
    fflush(log);
    if (elapsed < timeout_limit_g)
        return true;
    return false;
}

int my_hashvalue(char *uri, FILE *log){
    int i = 0;
    int value = 0;

    while(uri[i]){
        value += uri[i];
        if (value > HASHMOD)
            value = value % HASHMOD;
        i++;
    }
    fprintf(log, "Hash value for %s is %i\n", uri, value);
    fflush(log);
    return value;
}

// // Waits for socket to be available before sending packet
// int read_when_available(int recvfd, char *buf, FILE *log){
//     int n_sent;
//     fd_set recvfds;
//     struct timeval tv;
//     int n;
//     int rv;
    
//     FD_ZERO(&recvfds);
//     FD_SET(recvfd, &recvfds);
//     n = recvfd+1;

//     tv.tv_sec = 3;
//     tv.tv_usec = 0;

//     rv = select(n, &recvfds,NULL, NULL, &tv);

//     if(rv == 0){
//         fprintf(log, "Select timed out\n");
//         fflush(log);
//         return 0;
//     } else if (rv < 0){
//         fprintf(log, "Select error: %s\n", strerror(errno));
//         fflush(log);
//         return -1;
//     } else {
//         if(!FD_ISSET(recvfds, &recvfds)){
//             fprintf(log, "Unexpected socket behavior\n");
//             fflush(log);
//             return -1;
//         } else {
//             fprintf(log, "Socket ready to send\n");
//             n_sent = read(recvfds, buf, MAX_BUF, 0);
//             if(n_sent < 0){
//                 fprintf(log, "Error sending to ready socket: %s\n", strerror(errno));
//                 fflush(log);
//                 return -1;
//             } else {
//                 fprintf(log, "Sent %i bytes to ready socket\n", n_sent);
//                 fflush(log);
//                 return n_sent;
//             }
//         }
//     }
// }

// Waits for socket to be available before sending packet
int send_when_available(int sendfd, char *data, int datasize, FILE *log){
    int n_sent;
    fd_set sendfds;
    struct timeval tv;
    int n;
    int rv;
    
    FD_ZERO(&sendfds);
    FD_SET(sendfd, &sendfds);
    n = sendfd+1;

    tv.tv_sec = 3;
    tv.tv_usec = 0;

    rv = select(n, NULL, &sendfds, NULL, NULL);

    if(rv == 0){
        fprintf(log, "Select timed out\n");
        fflush(log);
        return 0;
    } else if (rv < 0){
        fprintf(log, "Select error: %s\n", strerror(errno));
        fflush(log);
        return -1;
    } else {
        if(!FD_ISSET(sendfd, &sendfds)){
            fprintf(log, "Unexpected socket behavior\n");
            fflush(log);
            return -1;
        } else {
            fprintf(log, "Socket ready to send\n");
            //n_sent = send(sendfd, data, datasize, MSG_NOSIGNAL);
            n_sent = send(sendfd, data, datasize, MSG_NOSIGNAL);
            if(n_sent < 0){
                fprintf(log, "Error sending to ready socket: %s\n", strerror(errno));
                fflush(log);
                return -1;
            } else {
                fprintf(log, "Sent %i bytes to ready socket\n", n_sent);
                fflush(log);
                return n_sent;
            }
        }
    }
}

bool fetch_from_cache(char *host, char *uri, int clientfd, FILE *log){
    fprintf(log, "Fetch from cache\n");
    char fname[MAXLINE];
    char filebuf[MAXBUF];
    FILE *fp;
    int n_read;
    int n_sent;

    sprintf(&fname[0], "cache/%s/%s", host, uri);
    fp = fopen(fname, "r");
    if(!fp){
        fprintf(log, "No file %s in cache\n", uri);
        //close(fp);
        return false;
    } else {
        fprintf(log, "Opened file for reading\n");
    }
    fflush(log);
    bzero(&filebuf, MAXBUF);
    n_read = fread(&filebuf[0], 1, MAXBUF, fp); // get bytes from fp into filebuf
    fprintf(log, "Read %i bytes\n", n_read);
    fflush(log);
    while(n_read > 0){
        n_sent = send(clientfd, &filebuf, n_read, 0); // blocking
        if(n_sent < 0){
            fprintf(log, "Error sending: %s\n", strerror(errno));
            fclose(fp);
            return true;
        } else
            fprintf(log, "Sent %i bytes\n", n_sent);
        fflush(log);
        bzero(&filebuf, MAXBUF);
        n_read = fread(&filebuf[0], 1, MAXBUF, fp);
        fprintf(log, "Read %i bytes\n", n_read);
        fflush(log);
    } if (n_read < 0){
        fprintf(log, "Error reading from file%s\n", strerror(errno));
    }
    fclose(fp);
    return true;
}

FILE *open_for_saving(char *host, char *uri, FILE *log){
    fprintf(log, "Open for saving\n");
    FILE *fp;
    char fname[MAXLINE];
    char dirname[MAXLINE];

    // Make folder if needed
    sprintf(&dirname[0], "cache/%s", host);
    mkdir(dirname, 0777);

    // construct filename
    sprintf(&fname[0], "cache/%s/%s", host, uri);

    fp = fopen(fname, "w");
    if(!fp){
        fprintf(log, "Cannot open file for writing: %s\n", strerror(errno));
        return NULL;
    }
    return fp;
}

void write_chunk(FILE *fp, char *chunk, int len, FILE *log){
    //fprintf(log, "Write chunk\n");
    int n_written;
    n_written = fwrite(chunk, 1, len, fp);
    if(n_written < len)
        fprintf(log, "Error writing to file: %s\n", strerror(errno));
    else
        fprintf(log, "Wrote %i bytes to file\n", n_written);
    fflush(log);
    //fclose(fp);
}

/*
Open new socket, forward GET request, and send response back
*/
void forward_messages(int clientfd, int serverfd, char *msg, char *host, char *uri, FILE *log){
    fprintf(log, "FORWARD MESSAGES!\n");
    char buf[MAXBUF];
    bzero(buf, MAXBUF);
    int n;
    FILE *storage;
    struct file_index *fi = NULL;
    bool forward = true;
    bool new;
    struct timeval timeout;      
    timeout.tv_sec = 4;
    timeout.tv_usec = 0;
    
    // if (setsockopt (serverfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0)
    //     fprintf(log, "setsockopt failed: %s\n", strerror(errno));

    fflush(log);
    while(!fi){
        fprintf(log, "Spinning until I can lock file %s\n", uri);
        fflush(log);
        fi = findex_retrieve_create_lock(uri, &new, log);
    }
    fflush(log);

    if(!new && recent(fi, log)){
        fprintf(log, "Approved to retrieve recent from cache\n");
        if(fetch_from_cache(host, uri, clientfd, log)){
            unlock(&(fi->file_mutex), uri, log);
            fprintf(log, "Copied response %s from cache\n", uri);
            return;
        }
    } else {
        fprintf(log, "not recent\n");
    }
    fflush(log);

    n = write(serverfd, msg, strlen(msg));
    if (n < 0){
        fprintf(log, "Error writing request: %s\n", strerror(errno));
        fflush(log);
        return;
    } else if (n==0){
        fprintf(log, "Request could not be forwarded\n");
    } else {
        fprintf(log, "Sent original message, %i bytes\n", n);
    }
    fflush(log);

    storage = open_for_saving(host, uri, log);
    while(1){
        fflush(log);
        n = read(serverfd, buf, MAXBUF);
        if (n < 0){
            fprintf(log, "error reading response: %s\n", strerror(errno));
            fflush(log);
            fclose(storage);
            unlock(&(fi->file_mutex), uri, log);
            return;
        } else if (n == 0){
            fprintf(log, "Full response received from server\n");
            fflush(log);
            fclose(storage);
            fi->timestamp = clock(); // Record timestamp to global
            fprintf(log, "Recorded finish time %lu to global\n", fi->timestamp);
            fflush(log);
            unlock(&(fi->file_mutex), uri, log);
            return;
        } else {
            fprintf(log, "Received %i bytes response\n", n);
            //fprintf(log, "Response:\n%s\n", buf);
            fflush(log);
        }
        write_chunk(storage, buf, n, log);
        if(!forward)
            continue; // skip forwarding to client if they refused it, but keep downloading
        n = send_when_available(clientfd, buf, n, log);
        //n = write(clientfd, buf, n);
        if (n <= 0){
            fprintf(log, "Connection to client unavailable: %s\n", strerror(errno));
            forward = false;
            // fclose(storage);
            // unlock(&(fi->file_mutex), uri, log);
            fflush(log);
            // return;
        } else {
            fprintf(log, "Forwarded %i bytes from %i to %i\n", n, serverfd, clientfd);
            fflush(log);
        }
    }
}

bool on_blacklist(char *hostname_ip, FILE *log){
    //fprintf(log, "ON BLACKLIST?\n");
    FILE *fp;
    char linebuf[MAXLINE];
    char *line;

    if(!hostname_ip)
        return false;

    fp = fopen("blacklist.txt", "r");
    if (!fp)
        perror("Error opening blacklist");
    line = fgets(&linebuf[0], MAXLINE, fp);
    while(line){
        //fprintf(log, "Checking %s against line %s\n", hostname_ip, line);
        //fflush(log);
        if(strstr(hostname_ip, line) != NULL){
            fprintf(log, "Found %s on blacklist\n", hostname_ip);
            fclose(fp);
            return true; // on blacklist
        }
        line = fgets(&linebuf[0], MAXLINE, fp);
    }
    //fflush(log);
    fclose(fp);
    return false;
}

char *local_dns(char *hostname, FILE *log){
    //fprintf(log, "Local DNS\n");
    FILE *fp;
    char linebuf[MAXLINE];
    char *line;
    char *ip;

    fp = fopen("dns.txt", "r");
    if(!fp)
        perror("DNS File");

    line = fgets(&linebuf[0], MAXLINE, fp);
    while(line){
        //fprintf(log, "Checking %s against line %s\n", hostname, line);
        fflush(log);
        if(strstr(line, hostname) != NULL){
            ip = strrchr(line, ',');
            ip = &ip[1];
            ip[strcspn(ip, "\n")] = 0; // remove trailing newline
            //fprintf(log, "DNS: IP for %s is %s\n", hostname, ip);
            fclose(fp);
            return ip;
        }
        line = fgets(&linebuf[0], MAXLINE, fp);
    }
    //fflush(log);
    fclose(fp);
    return NULL;
}

void add_dns_entry(char *hostname, char *ip){
    FILE *fp;

    fp = fopen("dns.txt", "a");
    if (!fp)
        perror("DNS file");
    //fprintf(fp, "%s,%s\n", hostname, ip);
    fclose(fp);
}

/*
Referenced from Beej's Guide https://www.beej.us/guide/bgnet/pdf/bgnet_a4_c_1.pdf
p. 83-84
*/
int validate_hostname(char *hostname, char *port, int clientfd, FILE *log){
    //fprintf(log, "VALIDATE_HOSTNAME %s %s\n", hostname, port);
    int serverfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    void *addr;
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in *ipv4;
    struct sockaddr_in6 *ipv6;
    char *ip;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM; // tcp

    // Get IP from local file
    ip = local_dns(hostname, log);
    if(ip != NULL){
        struct sockaddr serv_addr;
        struct sockaddr_in *sa;
        sa = (struct sockaddr_in *)&serv_addr;
        sa->sin_family = AF_INET;
        sa->sin_port = htons(atoi(port));
        rv = inet_pton(AF_INET, ip, &(sa->sin_addr));
        if (rv <= 0)
            perror("inet-pton");
        // Make a socket
        if ((serverfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
            perror("socket");
        } else {
            //printf("Made a socket\n");
        }
        // Connect
        if (connect(serverfd, &serv_addr, sizeof(struct sockaddr)) == -1){
            perror("connect");
            close(serverfd);
        } //else
            //printf("Connected successfully\n");
        return serverfd; // Connected successfully
    }

    // Get IP from GetAddrInfo
    if ((rv = getaddrinfo(hostname, port, &hints, &servinfo)) != 0) {
        fprintf(log, "getaddrinfo: %s\n", gai_strerror(rv));
        freeaddrinfo(servinfo);
        return(-1);
    }
    for(p = servinfo; p != NULL; p = p->ai_next){
        //fflush(log);
        // Print the IP address
        if(p->ai_family == AF_INET){
            ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
        } else {
            ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
        }
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        //fprintf(log, "Hostname %s translated to IP %s\n", hostname, ipstr);
        add_dns_entry(hostname, ipstr);
        if(on_blacklist(ipstr, log)){
            freeaddrinfo(servinfo);
            return 0;
        }
        // Make a socket
        if ((serverfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1){
            perror("socket");
            freeaddrinfo(servinfo);
            continue;
        }
        // Connect
        if (connect(serverfd, p->ai_addr, p->ai_addrlen) == -1){
            perror("connect");
            freeaddrinfo(servinfo);
            close(serverfd);
            continue;
        }
        //fprintf(log, "Connected successfully\n");
        freeaddrinfo(servinfo);
        return serverfd;// Connected successfully
    }
    if (p == NULL){
        fprintf(log, "failed to connect\n");
        freeaddrinfo(servinfo);
        return(-1);
    }
    freeaddrinfo(servinfo); // when should this be freed?
    return(-1);

}

// Back to basics lol
char **strsplit(char *str, char delim){
    int n = 0;
    int i = 0;
    while(str[i]){
        if(str[i] == delim)
            n++;
        i++;
    }
    char **values = (char **)malloc(sizeof(char *) * n);
    //printf("Allocated %i strings\n", n);
    i = 0;
    int j = 0;
    int k = 0;
    while(str[k]){
        j = 0;
        if(str[k] == delim){
            k++;
            continue;
        }
        //printf("Examining letter %c for string %i position %i\n", str[k], i, j);
        values[i] = (char *)malloc(sizeof(char) * MAXLINE);
        while(str[k] != delim){
            if(str[k] == '\r'){
                k++;
                continue;
            }
            values[i][j] = str[k];
            j++;
            k++;
        }
        values[i][j] = '\0';
        k++;
        //printf("Found string: %s\n", values[i]);
        i++;
    }
    return values;
}

/*
* Read the first message from the socket.
* If keepalive is set, continue listening for 10s.
* Sending incoming messages to parse
* Parse will return a bool for keepalive or not.
*/
void read_whilealive(int clientfd, FILE *log){
    char recvbuf[MAXBUF];
    ssize_t n;
    clock_t lastmsg;
    clock_t checktime;
    int lastelapsed = 0;
    int elapsedtime;
    bool firstreceived = false;
    bool keepalive;
    lastmsg = clock();

    while(1) {
        fflush(log);
        bzero(recvbuf, MAXBUF);
        n = read(clientfd, recvbuf, MAXBUF);
        if(n == 0){
            fprintf(log, "Read=0, Closing connection\n");
            return;
        }
        else if (n < 0){
            if(errno != EAGAIN && errno != EWOULDBLOCK){
                fprintf(log, "Error getting input: %s\n", strerror(errno));
                return;
            }
            //printf("Client %i sent first message\n", clientfd);
            // Keep looping if waiting for first message or waiting for keepalive timeout
            checktime = clock();
            elapsedtime = (checktime-lastmsg)/CLOCKS_PER_SEC;
            if (elapsedtime > lastelapsed){
                fprintf(log, "Socket %i Elapsed time: %i\n", clientfd, elapsedtime);
                lastelapsed = elapsedtime;
            }
            //if (!firstreceived || (keepalive && (elapsedtime < 10))){
            if (elapsedtime < 10){
                //printf("Socket %i continuing. Firstreceived: %i, keepalive: %i, elapsedtime: %i\n", clientfd, firstreceived, keepalive, elapsedtime);
                continue;
            } else {
                fprintf(log, "Socket %i Closing connection\n", clientfd);
                return;
            }
        } else {
            firstreceived = true;
            //fcntl(clientfd, F_SETFL, O_NONBLOCK);
            lastmsg = clock();
            //printf("Socket %i received the following request:\n%s\n", clientfd, recvbuf);
            keepalive = parse(clientfd, &recvbuf[0], log);
            if (!keepalive){
                fprintf(log, "Socket %i Closing connection\n", clientfd);
                return;
            } else {
                // Set to nonblocking for this function
                fcntl(clientfd, F_SETFL, O_NONBLOCK);
            }
        }
    }
}

/*
Receive request from client
Parse it into host, port, path, and body
Validate request and send error if needed
If valid, call to forward() method
*/
bool parse(int clientfd, char *msg, FILE *log){
    fprintf(log, "PARSE\n");
    char msgcpy[MAXBUF];
    char *remainder;
    char *method;
    char *uri;
    char *hostp;
    char port[6];
    char host[MAXLINE];
    char pth[MAXLINE];
    char *hostport;
    char *version;
    int serverfd;
    bool keepalive = false;

    // Save a copy of the original message
    strncpy(msgcpy, msg, MAXBUF);

    // Parse main request
    method = strtok_r(&msg[0], " ", &remainder);
    uri = strtok_r(NULL, " ", &remainder);
    version = strtok_r(NULL, " \n\r", &remainder);

    fprintf(log, "\nSocket %i recognized request: \nmethod [%s] \nuri [%s] \nversion [%s]\n", 
        clientfd, method, uri, version);
    printf("\nSocket %i takes request: \nmethod [%s] uri [%s] \n", 
        clientfd, method, uri);

    if(strncmp(method, "GET", 3) != 0){
        printf("Not a get request - ignoring\n");
        http_err(clientfd, 400, "HTTP/1.0", log);
        return keepalive;
    }

    /*
    * Parse options
    */
    char **options = strsplit(remainder, '\n');
    size_t i = 0;
    while (i < sizeof(options)){
        char *option = options[i];
        //printf("Socket %i Checking option: \n\t%s\n", clientfd, option);
        if(!option)
            break;
        else if(strncmp(option, "Connection: keep-alive", 22) == 0){
            //fprintf(log, "\tRecognized keepalive\n");
            keepalive = true;
        } else if (strncmp(option, "Host:", 5) == 0){
            bzero(host, MAXLINE);
            hostport = &option[6]; // host:port or host
            hostp = strsep(&hostport, ":");
            strncpy(host, hostp, strlen(hostp));
            if(!hostport)
                strncpy(&port[0], "80", 3);
            else {
                strncpy(&port[0], hostport, strlen(hostport));
            }
            fprintf(log, "\tHost: %s, Port: %s\n", host, port);
        }
        free(option);
        i++;
    }
    free(options);

    if(host[0] == 0){
        fprintf(log, "Trying to derive missing host\n");
        char *tmp = strstr(uri, "//");
        tmp = &host[2];
        tmp = strsep(&tmp, "/");
        strncpy(host, tmp, strlen(tmp));
        fprintf(log, "Derived host from uri: %s\n", host);
        strncpy(&port[0], "80", 3);
    }

    if(on_blacklist(host, log)){
        http_err(clientfd, 403, version, log);
        return keepalive;
    }

    serverfd = validate_hostname(host, port, clientfd, log);
    if (serverfd < 0)
        http_err(clientfd, 400, version, log);
    else if (serverfd == 0){
        http_err(clientfd, 403, version, log);
    } else {
        //fprintf(log, "Hostname validated\n");
        char *path = NULL;
        path = strrchr(uri, '/');
        if (!path || strlen(path) <= 1)
            path = "index.html";
        else
            path = &path[1];
        bzero(pth, MAXLINE);
        strncpy(&pth[0], path, strlen(path));
        //printf("Path: %s\n", pth);
        //fprintf(log, "Path: %s\n", pth);
        // set to blocking again
        int flags = fcntl(clientfd, F_GETFL, 0);
        flags &= !O_NONBLOCK;
        if (flags < 0){
            fprintf(log, "Flags err: %s", strerror(errno));
        }
        if(fcntl(clientfd, F_SETFL, flags)<0)
            fprintf(log, "Fcntl err: %s", strerror(errno));
        //fflush(log);
        forward_messages(clientfd, serverfd, &msgcpy[0], host, &pth[0], log);
        close(serverfd);
    }
    return keepalive;

}

/*
 Generic error
 */
void http_err(int connfd, int type, char *httpversion, FILE *log){
    fprintf(log, "Sending error %i to client\n", type);
    char *msg;
    char buf[MAXLINE];
    int n;

    if (type == 400)
        msg = "Bad Request";
    else if (type == 403)
        msg = "Forbidden";
    else
        msg = "Internal Server Error";

    n = sprintf(buf, "%s %i %s\r\n", httpversion, type, msg);

    n = send(connfd, buf, n, 0);
    if(n < 0)
        fprintf(log, "Error sending error: %s\n", strerror(errno));
    else
        fprintf(log, "Sent %i bytes err to client: %s\n", n, buf);
}

/*
Allocate thread buffer and launch thread specific actions
Cleanup memory afterwards
*/
void *thread(void *vargp){
    int connfd = *((int *)vargp);
    printf("Created client connection %i\n", connfd);
    pthread_detach(pthread_self());
    //fcntl(connfd, F_SETFL, O_NONBLOCK);
    char logname[MAXLINE];
    sprintf(logname, "logs/connfd_%i.txt", connfd);
    FILE *threadlog = fopen(logname, "a");
    fprintf(threadlog, "\nOpened new process for connection %i\n", connfd);
    fcntl(connfd, F_SETFL, O_NONBLOCK);
    read_whilealive(connfd, threadlog);
    fprintf(threadlog, "Closing client connection %i\n", connfd);
    printf("Closing client connection %i\n", connfd);
    free(vargp);
    close(connfd);
    fclose(threadlog);
    return NULL;
}

/* 
 * open_listenfd - open and return a listening socket on port
 * Returns -1 in case of failure 
 */
int open_listenfd(int port) 
{
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;
  
    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
                   (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons((unsigned short)port); 
    if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
}