#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int port = 1234;
int listenfd;
char banner[] = "dancecard 1\r\n";

#define MAXHANDLE 50  /* maximum permitted handle size, not including \0 */

#define FOLLOW 0
#define LEAD 1
#define BOTH 2

#define max(a,b)((a>b)?a:b)
#define min(a,b)((a<b)?a:b)

struct dancer {
    int fd;
    struct in_addr ipaddr;
    char handle[MAXHANDLE + 1];  /* zero-terminated; handle[0]==0 if not set */
    int role;  /* -1 if not set yet */
    char buf[200];  /* data in progress */
    int bytes_in_buf;  /* how many data bytes already read in buf */
    struct dancer *partner;  /* null if not yet partnered */
    struct dancer *next;
} *dancers = NULL;

int nlead = 0, nfollow = 0, nboth = 0, someone_is_partnered = 0;

extern void parseargs(int argc, char **argv);
extern void makelistener();
extern void newclient(int fd, struct sockaddr_in *r);
extern void clientactivity(struct dancer *p);
extern void removeclient(struct dancer *p);
extern void do_something(struct dancer *p, char *wherenewline);
extern void begindance();
extern char *memnewline(char *p, int size);  /* finds \r _or_ \n */
extern void who(struct dancer *p);


int main(int argc, char **argv)
{
    int did_something;
    struct dancer *p, *n;

    parseargs(argc, argv);
    makelistener();

    while (1) {

        did_something = 0;

        /* check if we have a full input line in memory for some client */
        for (p = dancers; p; p = n) {
            char *q = memnewline(p->buf, p->bytes_in_buf);
            n = p->next;  /* stash the pointer now in case 'p' is deleted */
            if (q) {
                do_something(p, q);
                did_something = 1;
            }
        }

        if (!did_something) {
            fd_set fds;
            int maxfd = listenfd;
            FD_ZERO(&fds);
            FD_SET(listenfd, &fds);
            for(p = dancers; p; p = p->next) {
                FD_SET(p->fd, &fds);
                if(p->fd > maxfd)
                    maxfd = p->fd;
            }

            if(select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) {
                perror("select");
            } else {
                for(p = dancers; p; p = p->next)
                    if(FD_ISSET(p->fd, &fds))
                        break;

                if(p)
                    clientactivity(p);

                if(FD_ISSET(listenfd, &fds)) {
                    int newfd;
                    struct sockaddr_in r;
                    socklen_t socklen = sizeof r;
                    if((newfd = accept(listenfd, (struct sockaddr *)&r, &socklen)) < 0)
                        perror("accept");
                    else
                        newclient(newfd, &r);
                }
            }
        }
    }
}


void parseargs(int argc, char **argv)
{
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
        case 'p':
            port = atoi(optarg);
            break;
        default:
            status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}


void makelistener()
{
    struct sockaddr_in r;
    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    memset(&r, '\0', sizeof r);
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if(bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }

    if(listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
}


void newclient(int fd, struct sockaddr_in *r)
{
    printf("connection from %s\n", inet_ntoa(r->sin_addr));
    if(write(fd, banner, sizeof banner - 1) == -1) {
        perror("write");
        exit(1);
    }

    struct dancer *d = malloc(sizeof(struct dancer));
    if(!d) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    fflush(stdout);
    d->fd = fd;
    d->ipaddr = r->sin_addr;
    d->handle[0] = '\0';
    d->role = -1;
    d->buf[0] = '\0';
    d->bytes_in_buf = 0;
    d->partner = NULL;
    d->next = dancers;
    dancers = d;
}


void clientactivity(struct dancer *p)
{
    char *q;
    int len;

    len = read(p->fd, p->buf, sizeof p->buf);
    p->bytes_in_buf += len;
    if(p->bytes_in_buf < 0) {
        removeclient(p);
    } else if ((q = memnewline(p->buf, p->bytes_in_buf)))
        do_something(p, q);
    else if (p->bytes_in_buf == sizeof p->buf)
        /*
         * We don't have a newline, but our buffer is full, so we'd
         * better process it anyway.
         */
        do_something(p, p->buf + sizeof p->buf - 1);
}


void removeclient(struct dancer *p)
{
    struct dancer **d;
    for(d = &dancers; *d && (*d)->fd != (*p).fd; d = &(*d)->next)
        ;
    if(*d) {
        struct dancer *s;
        char goodbye[100];
        snprintf(goodbye, sizeof goodbye, "%s bids you all good night.\r\n", p->handle);
        for(s = dancers; s; s = s->next) {
            if(write(s->fd, goodbye, strlen(goodbye)) != strlen(goodbye)) {
                perror("write");
                exit(1);
            }

            if(strcmp(s->partner->handle, p->handle) == 0) {
                s->partner = NULL;
                char sorry[150] = "I'm sorry, your partner seems to have left the dance hall!\nYou'll need to find someone else to dance with.\r\n";
                if(write(s->fd, sorry, sizeof sorry - 1) == -1) {
                    perror("write");
                    exit(1);
                }
            }
        }

        struct dancer *t = (*d)->next;
        printf("disconnecting client %s\n", inet_ntoa((*d)->ipaddr));
        fflush(stdout);
        free(*d);
        *d = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it \n", (*p).fd);
        fflush(stderr);
    }
}


/* there is a command in the buffer; do it */
void do_something(struct dancer *p, char *wherenewline)
{
    int len;
    len = wherenewline - p->buf;
    *wherenewline = '\0';

    if (len == 0)
        ;/* ignore blank lines */
    else if (p->handle[0] == '\0') {
        static char error1[] = "Sorry, that word is a command, so it can't be used as a handle.\r\n";
        static char error2[] = "Sorry, someone is already using that handle. Please choose another.\r\n";
        static char valid[] = "\r\n";
        if(strcmp("who", p->buf) == 0 || strcmp("begin", p->buf) == 0 || strcmp("debug", p->buf) == 0) {
            if(write(p->fd, error1, sizeof error1 - 1) == -1) {
                perror("write");
                exit(1);
            }
        } else {
            int found = 0;
            struct dancer *d;
            for(d = dancers; d; d = d->next) 
                if(strcmp(p->buf, d->handle) == 0)
                    found = 1;

            if(found) {
                if(write(p->fd, error2, sizeof error2 - 1) == -1) {
                    perror("write");
                    exit(1);
                }
            } else {
                strncpy(p->handle, p->buf, 50);
                printf("set handle of fd %d to %s\n", p->fd, p->handle);
                if(write(p->fd, valid, sizeof valid - 1) == -1) {
                    perror("write");
                    exit(1);
                }
            }
        }
    } else if(p->role < 0) {
        static char error[] = "Invalid role. Type lead, follow or both.\r\n";
        static char valid[] = "\r\n";
        int proceed = 1;
        if(strcmp("l", p->buf) == 0 || strcmp("lead", p->buf) == 0) { 
            p->role = LEAD;
            nlead += 1;
        } else if(strcmp("f", p->buf) == 0 || strcmp("follow", p->buf) == 0) {
            p->role = FOLLOW;
            nfollow += 1;
        } else if(strcmp("b", p->buf) == 0 || strcmp("both", p->buf) == 0) {
            p->role = BOTH;
            nboth += 1;
        } else {
            proceed = 0;
        }

        if(proceed) {
            printf("set role of fd %d to %d\n", p->fd, p->role);
            if(write(p->fd, valid, sizeof valid - 1) == -1) {
                perror("write");
                exit(1);
            }

            char join[100];
            snprintf(join, sizeof join, "%s has joined the dance!\r\n", p->handle);
            struct dancer *d;
            for(d = dancers; d; d = d->next)
                if(p->fd != d->fd)
                    if(write(d->fd, join, strlen(join)) != strlen(join)) {
                        perror("write");
                        exit(1);
                    }

            static char welcome[] = "Welcome to the dance!\r\n";
            if(write(p->fd, welcome, sizeof welcome - 1) == -1) {
                perror("write");
                exit(1);
            }
            who(p);
        } else {
            if(write(p->fd, error, sizeof error - 1) == -1) {
                perror("write");
                exit(1);
            }
        }
    } else {
        if(strcmp("who", p->buf) == 0) {
            who(p);
        } else if(strcmp("begin", p->buf) == 0) {
            begindance();
        } else {
            struct dancer *d;
            for(d = dancers; d; d = d->next)
                if(strcmp(d->handle, p->buf) == 0)
                    break;
                
            if(d) {
                char reply[100];
                if(p->partner != NULL) {
                    snprintf(reply, sizeof reply, "You already have a partner for this  dance! Type 'begin' to ask for the dance to begin.\r\n");
                    if(write(p->fd, reply, strlen(reply)) != strlen(reply)) {
                        perror("write");
                        exit(1);
                    } 
                } else {
                    if(d->role == p->role) {
                        if(d->role == LEAD)
                            snprintf(reply, sizeof reply, "Sorry, %s can only dance lead.\r\n", d->handle);
                        else if(d->role == FOLLOW)
                            snprintf(reply, sizeof reply, "Sorry, %s can only dance follow.\r\n", d->handle);

                        if(write(p->fd, reply, strlen(reply)) != strlen(reply)) {
                            perror("write");
                            exit(1);
                        }
                    } else {
                        int leftout = max(nlead, nfollow) - min(nlead, nfollow) - nboth;
                        if(leftout != 0) {
                            snprintf(reply, sizeof reply, "Please don't ask %s to dance, as that would leave people out.\r\n", d->handle);
                            if(write(p->fd, reply, strlen(reply)) != strlen(reply)) {
                                perror("write");
                                exit(1);
                            }
                        } else {
                            if(d->role == BOTH && ((p->role == LEAD && nlead < nfollow) || (p->role == FOLLOW && nlead > nfollow))) {
                                snprintf(reply, sizeof reply, "Please don't ask %s to dance, as that would leave people out.\r\n", d->handle);
                                if(write(p->fd, reply, strlen(reply)) != strlen(reply)) {
                                    perror("write");
                                    exit(1);
                                }
                            } else {
                                p->partner = d;
                                d->partner = p;
                                snprintf(reply, sizeof reply, "%s accepts!\r\n", d->handle);
                                if(write(p->fd, reply, strlen(reply)) != strlen(reply)) {
                                    perror("write");
                                    exit(1);
                                }
                                char notify[100];
                                snprintf(notify, sizeof notify, "%s asked you to dance. You accept!\r\n", p->handle);
                                if(write(d->fd, notify, strlen(notify)) != strlen(notify)) {
                                    perror("write");
                                    exit(1);
                                }
                                someone_is_partnered += 2;
                                if(someone_is_partnered == nlead + nfollow + nboth)
                                    begindance();
                            }
                        }
                    }
                }
            } else {
                static char unknown[] = "There is no dancer by that name.\r\n";
                if(write(p->fd, unknown, sizeof unknown - 1) == -1) {
                    perror("write");
                    exit(1);
                }
            }
        }
    }
    
    /* now remove this command from the buffer */
    /* p->buf[len] was either \r or \n.  How about p->buf[len+1]? */
    len++;
    if (len < p->bytes_in_buf && (p->buf[len] == '\r' || p->buf[len] == '\n'))
        len++;
    p->bytes_in_buf -= len;
    memmove(p->buf, p->buf + len, p->bytes_in_buf);
}


void begindance()
{
    static char message1[] = "Dance begins\r\n";
    static char message2[] = "Dance ends.  Your partner thanks you for the dance!\r\nTime to find a new partner.  Type 'who' for a list of available dancers.\r\n";
    struct dancer *p;
    for (p = dancers; p; p = p->next)
        write(p->fd, message1, sizeof message1 - 1);
    sleep(5);
    for (p = dancers; p; p = p->next) {
        write(p->fd, message2, sizeof message2 - 1);
        p->partner = NULL;
    }
    someone_is_partnered = 0;
    // something to reset nlead, etc --
    //   I suggest counting again from scratch, probably as a separate function
}


char *memnewline(char *p, int size)  /* finds \r _or_ \n */
        /* This is like min(memchr(p, '\r'), memchr(p, '\n')) */
        /* It is named after memchr().  There's no memcspn(). */
{
    for (; size > 0; p++, size--)
        if (*p == '\r' || *p == '\n')
            return(p);
    return(NULL);
}


void who(struct dancer *p)
{
    static char msg1[] = "No one else is here yet, but I'm sure they'll be here soon!\r\n";
    if(nlead + nfollow + nboth == 1) {
        if(write(p->fd, msg1, sizeof msg1 - 1) == -1) {
            perror("write");
            exit(1);
        }
    } else {
        static char msg2[] = "Unpartnered dancers are:\r\n";
        if(write(p->fd, msg2, sizeof msg2 - 1) == -1) {
            perror("write");
            exit(1);
        }

        struct dancer *d;
        char msg3[100];
        for(d = dancers; d; d = d->next) {
            if((d->fd != p->fd) && d->partner == NULL) {
                if(d->role != p->role || d->role == BOTH) {
                    snprintf(msg3, sizeof msg3, "%s\r\n", d->handle);
                } else {
                    if(d->role == LEAD)
                        snprintf(msg3, sizeof msg3, "[%s only dances lead]\r\n", d->handle);
                    else if(d->role == FOLLOW)
                        snprintf(msg3, sizeof msg3, "[%s only dances follow]\r\n", d->handle);
                }

                if(write(p->fd, msg3, strlen(msg3)) != strlen(msg3)) {
                    perror("write");
                    exit(1);
                }
            }
        }
    }
}
