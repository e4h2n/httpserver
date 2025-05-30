#pragma once

#include <unistd.h>
#include <stdlib.h>

#include "protocol.h"

#define BUFSIZE MAX_HEADER_LEN

void exit_fail(const char *msg) {
    perror(msg);
    exit(1);
}

int min(int a, int b) {
    return a > b ? b : a;
}

int max(int a, int b) {
    return a < b ? b : a;
}

// repeated read (r_read)
// reads from a file descriptor until any of the following conditions are met:
//  - max_len bytes have been read
//  - read() has been called reps times (ignore if reps < 0)
//  - read() returns 0 (meaning it hit EOF)
// returns the total number of bytes read
int r_read(int fd, char *buf, int max_len, int reps) {
    int tot_len = 0;
    for (; reps != 0 && tot_len < max_len; reps--) {
        int len = read(fd, buf + tot_len, max_len - tot_len);
        if (len == 0)
            break;
        if (len < 0)
            exit_fail("read() failed");
        tot_len += len;
    }
    return tot_len;
}

int read_until(int fd, char *buf, int max_len, char *pattern, int pattern_len) {
    int tot_len = 0;
    while (tot_len < max_len) {
        int len = read(fd, buf + tot_len, max_len - tot_len);
        tot_len += len;
        if (len == 0)
            break;
        if (len < 0)
            exit_fail("read() failed");
        int to_check = tot_len - max(tot_len - len - pattern_len - 1, 0);
        if (memmem(buf + tot_len - to_check, to_check, pattern, pattern_len) != NULL)
            break;
    }
    return tot_len;
}

// repeated write (r_write)
// writes to a file descriptor until any of the following conditions are met:
//  - max_len bytes have been written
//  - write() has been called reps times (ignore if reps < 0)
// returns the total number of bytes written
int r_write(int fd, char *buf, int max_len, int reps) {
    int tot_len = 0;
    for (; reps != 0 && tot_len < max_len; reps--) {
        int len = write(fd, buf + tot_len, max_len - tot_len);
        if (len < 0)
            exit_fail("write() failed");
        tot_len += len;
    }
    return tot_len;
}

int n_pass(int dest, int src, char *buf, int n) {
    int total_len = 0;
    for (int len; total_len < n && (len = read(src, buf, min(n - total_len, BUFSIZE)));
         total_len += len) {
        r_write(dest, buf, len, -1);
    }
    return total_len;
}
