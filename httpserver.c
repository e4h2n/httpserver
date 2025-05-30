#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <regex.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h> // for address format conversion

#include "protocol.h"
#include "myio.h"
#include "queue.h"
#include "rwlock.h"

#define szstr(str) str, sizeof(str) // includes NULL BYTE

#define HTTP_RESPONSE(status, len) HTTP_VERSION " " status "\r\nContent-Length: " len "\r\n\r\n"

#define HTTP_OK          HTTP_RESPONSE("200 OK", "3") "OK\n"
#define HTTP_OK_MSG      HTTP_RESPONSE("200 OK", "%ld") // use with sprintf
#define HTTP_CREATED     HTTP_RESPONSE("201 Created", "8") "Created\n"
#define HTTP_BAD_REQUEST HTTP_RESPONSE("400 Bad Request", "12") "Bad Request\n"
#define HTTP_FORBIDDEN   HTTP_RESPONSE("403 Forbidden", "10") "Forbidden\n"
#define HTTP_NOT_FOUND   HTTP_RESPONSE("404 Not Found", "10") "Not Found\n"
#define HTTP_INTERNAL_SERVER_ERROR                                                                 \
    HTTP_RESPONSE("500 Internal Server Error", "22") "Internal Server Error\n"
#define HTTP_NOT_IMPLEMENTED HTTP_RESPONSE("501 Not Implemented", "16") "Not Implemented\n"
#define HTTP_VERSION_NOT_SUPPORTED                                                                 \
    HTTP_RESPONSE("505 Version Not Supported", "22") "Version Not Supported\n"

#define HTTP_REQUEST_REGEX                                                                         \
    "(" TYPE_REGEX ") (" URI_REGEX ") (" HTTP_REGEX ")\r\n" /* request_line */                     \
    "((" HEADER_FIELD_REGEX ": " HEADER_VALUE_REGEX "\r\n)*)" /* header-field-list */              \
    "\r\n" /* empty-line */                                                                        \
    "(.*)" /* message-body */

#define HTTP_REQUEST_NMATCH 7 // number of capture groups in HTTP_REQUEST_REGEX

struct uri_lock {
    char *uri;
    rwlock_t *lock;
    int in_use;
};

queue_t *request_queue;
pthread_t *thread_pool;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // for pool access
struct uri_lock *lock_pool;
int threads;

regex_t header_regex;
regex_t content_len_regex;
regex_t id_regex;

// WORKER ======================================================================
int respond_get(int sock_fd, char *buf, char *uri) {
    struct stat sbuf;
    int err = stat(uri, &sbuf);
    if (err) { // does not exist (or perms denied, but no way to check)
        write(sock_fd, szstr(HTTP_NOT_FOUND) - 1);
        return 404;
    } else if ((sbuf.st_mode & S_IFDIR) == S_IFDIR) {
        write(sock_fd, szstr(HTTP_FORBIDDEN) - 1);
        return 403;
    }

    int fd = open(uri, O_RDONLY);
    if (fd < 0) { // perm issue
        write(sock_fd, szstr(HTTP_FORBIDDEN) - 1);
        return 403;
    }

    // get file length...goofy ahh
    long file_len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    // write header
    int len = sprintf(buf, HTTP_OK_MSG, file_len);
    write(sock_fd, buf, len); // excludes null byte by default

    // write message-body
    n_pass(sock_fd, fd, buf, file_len);

    close(fd);

    return 200;
}

int respond_put(int sock_fd, char *buf, struct uri_lock *ulock, regmatch_t *header_match) {
    // extract content_length value from fields
    int content_len = -1;

    regmatch_t content_len_match[2];
    content_len_match[0].rm_so = header_match[4].rm_so;
    content_len_match[0].rm_eo = header_match[4].rm_eo;
    while (regexec(&content_len_regex, buf, 2, content_len_match, REG_STARTEND) == 0) {
        content_len = strtol(buf + content_len_match[1].rm_so, NULL, 10);
        content_len_match[0].rm_so = content_len_match[0].rm_eo + 1;
        content_len_match[0].rm_eo = header_match[4].rm_eo;
    }

    struct stat sbuf;
    if (content_len < 0) { // never matched :(
        r_write(sock_fd, szstr(HTTP_BAD_REQUEST) - 1, -1);
        return 400;
    } else if (!stat(ulock->uri, &sbuf) && (sbuf.st_mode & S_IFDIR) == S_IFDIR) { // is a directory
        r_write(sock_fd, szstr(HTTP_FORBIDDEN) - 1, -1);
        return 403;
    }

    char tempuri[] = "tempXXXXXX";
    int tmpfd = mkstemp(tempuri);
    if (tmpfd < 0) { // failed to create temp file => internal error
        r_write(sock_fd, szstr(HTTP_INTERNAL_SERVER_ERROR) - 1, -1);
        return 500;
    }

    // pass msg-body to tmp file
    content_len -= r_write(tmpfd, buf + header_match[6].rm_so,
        min(content_len, header_match[0].rm_eo - header_match[6].rm_so), -1);
    n_pass(tmpfd, sock_fd, buf, content_len);
    close(tmpfd);

    writer_lock(ulock->lock);
	// get status of uri; must be atomic so no two threads respond CREATED (201)
    char *http_response = HTTP_OK;
    int response_len = sizeof(HTTP_OK) - 1;
    int response_code = 200;
    if (stat(ulock->uri, &sbuf)) { // does not exist (or perms denied)
        http_response = HTTP_CREATED;
        response_len = sizeof(HTTP_CREATED) - 1;
        response_code = 201;
    }
	// rename temp file to uri
    int res = rename(tempuri, ulock->uri);
    if (res < 0) { // rename failed
        http_response = HTTP_INTERNAL_SERVER_ERROR;
        response_len = sizeof(HTTP_INTERNAL_SERVER_ERROR) - 1;
        response_code = 500;
    }
	// send response
    r_write(sock_fd, http_response, response_len, -1);
    return response_code;
}

void process_connection(int sock_fd, char *buf) {
    // read the header (and maybe some body)
    int len = read_until(sock_fd, buf, BUFSIZE, "\r\n\r\n", 4);
    if (len < 0) {
        exit_fail("read fail");
    }

    regmatch_t header_match[HTTP_REQUEST_NMATCH];
    regmatch_t id_match[2];

    // header matching
    header_match[0].rm_so = 0;
    header_match[0].rm_eo = len;

    int err = regexec(&header_regex, buf, HTTP_REQUEST_NMATCH, header_match, REG_STARTEND);
    header_match[0].rm_eo = len; // regex stupid; want to pass this information to put()
    if (err) {
        r_write(sock_fd, szstr(HTTP_BAD_REQUEST) - 1, -1);
        return;
    }

    // match ID
    id_match[0].rm_so = header_match[4].rm_so;
    id_match[0].rm_eo = header_match[4].rm_eo;
    int id = 0;
    while (regexec(&id_regex, buf, 2, id_match, REG_STARTEND) == 0) {
        id = strtol(buf + id_match[1].rm_so, NULL, 10);
        id_match[0].rm_so = id_match[0].rm_eo + 1;
        id_match[0].rm_eo = header_match[4].rm_eo;
    }

    // isolate and null-terminate uri
    buf[header_match[2].rm_eo] = '\0';
    char *uri = strdup(buf + header_match[2].rm_so + 1);

    pthread_mutex_lock(&lock);
    // pick the right URI lock
    struct uri_lock *my_uri_lock;
    for (int i = 0; i < threads; i++) {
        struct uri_lock *curr = lock_pool + i;
        if (strcmp(uri, curr->uri) == 0) {
            my_uri_lock = curr;
            break;
        } else if (curr->in_use == 0) {
            my_uri_lock = curr;
        }
    }

    free(my_uri_lock->uri);
    my_uri_lock->uri = strdup(uri);
    my_uri_lock->in_use++;

    pthread_mutex_unlock(&lock);

    if (memcmp(buf + header_match[3].rm_so, HTTP_VERSION, 8) != 0) {
        write(sock_fd, szstr(HTTP_VERSION_NOT_SUPPORTED) - 1);
    } else if (memcmp(buf, "GET ", 4) == 0) {
        reader_lock(my_uri_lock->lock);
        int response_code = respond_get(sock_fd, buf, uri);
        fprintf(stderr, "GET,/%s,%d,%d\n", uri, response_code, id); // LOG
        reader_unlock(my_uri_lock->lock);
    } else if (memcmp(buf, "PUT ", 4) == 0) {
        int response_code = respond_put(sock_fd, buf, my_uri_lock, header_match);
        fprintf(stderr, "PUT,/%s,%d,%d\n", uri, response_code, id); // LOG
        writer_unlock(my_uri_lock->lock);
    } else {
        write(sock_fd, szstr(HTTP_NOT_IMPLEMENTED) - 1);
    }

    free(uri);

    pthread_mutex_lock(&lock);
    my_uri_lock->in_use--;
    pthread_mutex_unlock(&lock);
}

void *work(void *_) {
    (void) _;
    char buf[BUFSIZE]; // on STACK, could cause issues? idk
    void *sock_void = NULL;
    while (1) {
        queue_pop(request_queue, &sock_void); // BLOCK
        int sock = (intptr_t) sock_void;
        process_connection(sock, buf);
        close(sock);
    }
    return NULL;
}

// DISPATCHER ==================================================================
struct sockaddr_in ls_addr;

int init_ls(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        exit_fail("Failed to create socket (init_ls)");
    }

    // init socket address struct
    // for TCP protocol (see https://man7.org/linux/man-pages/man7/ip.7.html)
    // must convert port, ip address from (h)ost to (n)etwork byte order!
    ls_addr.sin_family = AF_INET;
    ls_addr.sin_port = htons(port);
    ls_addr.sin_addr.s_addr = htonl(INADDR_ANY); // no IP binding

    int result = bind(sock, (struct sockaddr *) &ls_addr, sizeof(ls_addr));
    if (result == -1) {
        exit_fail("Invalid Port");
    }

    result = listen(sock, 32); // arbitrary choice of queue capacity
    if (result == -1) {
        exit_fail("uh oh spagetti-o, listen failed");
    }

    return sock;
}

extern char *optarg;
int main(int argc, char *argv[]) {
    int err = regcomp(&header_regex, HTTP_REQUEST_REGEX, REG_EXTENDED);
    err |= regcomp(&content_len_regex, "Content-Length: ([0-9]+)\r\n", REG_EXTENDED);
    err |= regcomp(&id_regex, "Request-Id: ([0-9]+)\r\n", REG_EXTENDED);
    if (err) {
        exit_fail("regex failed to compile");
    }

    int port;
    int res = getopt(argc, argv, "t:");
    char *last_port_char;
    char *last_threads_char = "\0";
    if (res == 't' && argc == 4) { // httpserver -t <threads> <port>
        threads = strtol(optarg, &last_threads_char, 10);
        port = strtol(argv[3], &last_port_char, 10);
    } else if (res == -1 && argc == 2) { // httpserver <port>
        threads = 4;
        port = strtol(argv[1], &last_port_char, 10);
    } else { // invalid usage
        fprintf(stdout, "USAGE: %s [-t threads] <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (*last_port_char || *last_threads_char || threads < 1 || port < 0) {
        fprintf(stdout, "non-natural argument\n");
        exit(EXIT_FAILURE);
    }

    // create a listener socket bound to port
    int listener = init_ls(port);
    fprintf(stdout, "SERVER: listening on port %d\n", port);

    // create request queue
    request_queue = queue_new(63);

    // create worker thread pool that wants to pop from queue
    thread_pool = calloc(sizeof(pthread_t), threads);
    lock_pool = calloc(sizeof(struct uri_lock), threads);
    res = 0;
    for (int i = 0; i < threads; i++) {
        lock_pool[i].uri = strdup("");
        lock_pool[i].lock = rwlock_new(WRITERS, 0);
        lock_pool[i].in_use = 0;
        res |= pthread_create(thread_pool + i, NULL, work, NULL);
    }
    if (res) {
        exit_fail("uh oh spagetti-o, pthread_create failed");
    }

    // accept connections forever, push requests into the queue
    while (1) {
        socklen_t len = sizeof(ls_addr);
        int sock = accept(listener, (struct sockaddr *) &ls_addr, &len);
        if (sock == -1) {
            exit_fail("uh oh spagetti-o, accept failed");
        }
        queue_push(request_queue, (void *) (intptr_t) sock); // L O L
    }
    return 0;
}
