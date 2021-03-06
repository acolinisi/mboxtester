#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/epoll.h>

#include "mailbox-map.h"

// If neither of the following is defined, then poll explicitly
#define SELECT
// #define POLL
// #define EPOLL

#define MASTER_ID_TRCH_CPU  0x2d

#define MASTER_ID_RTPS_CPU0 0x2e
#define MASTER_ID_RTPS_CPU1 0x2f

#define MASTER_ID_HPPS_CPU0 0x80
#define MASTER_ID_HPPS_CPU1 0x8d
#define MASTER_ID_HPPS_CPU2 0x8e
#define MASTER_ID_HPPS_CPU3 0x8f
#define MASTER_ID_HPPS_CPU4 0x90
#define MASTER_ID_HPPS_CPU5 0x9d
#define MASTER_ID_HPPS_CPU6 0x9e
#define MASTER_ID_HPPS_CPU7 0x9f

#define HPSC_MBOX_DATA_REGS 16

// From TRCH/RTPS command.h
#define CMD_NOP                         0
#define CMD_PING                        1
#define CMD_PONG                        2
#define CMD_MBOX_LINK_CONNECT           1000
#define CMD_MBOX_LINK_DISCONNECT        1001
#define CMD_MBOX_LINK_PING              1002

#define ENDPOINT_HPPS 0
#define ENDPOINT_RTPS 1

#define DEV_PATH_DIR "/dev/mbox/0/"
#define DEV_FILE_PREFIX "mbox"

#define PATH_SIZE 128

static char devpath_out_buf[PATH_SIZE];
static char devpath_in_buf[PATH_SIZE];
static char devpath_own_out_buf[PATH_SIZE];
static char devpath_own_in_buf[PATH_SIZE];

#define MSG_SIZE HPSC_MBOX_DATA_REGS
static uint32_t msg[HPSC_MBOX_DATA_REGS] = {0};

static int fd_out = -1, fd_in = -1, fd_own_out = -1, fd_own_in = -1;

static void print_msg(const char *ctx, uint32_t *reply, size_t len)
{
    size_t i;
    printf("%s: ", ctx);
    for (i = 0; i < len; ++i) {
        printf("%02x ", reply[i]);
    }
    printf("\n");
}

static const char *expand_path(const char *path, char *buf, size_t size)
{
        ssize_t sz = size;
        if (path[0] != '/') {
                buf[0] = '\0';
                if (sz <= 0)
                        return NULL;
                strncat(buf, DEV_PATH_DIR, sz - 1);
                sz -= strlen(DEV_PATH_DIR);

                if ('0' <= path[0] && path[0] <= '9') {
                        if (sz <= 0)
                                return NULL;
                       strncat(buf, DEV_FILE_PREFIX, sz - 1);
                       size -= strlen(DEV_FILE_PREFIX);
                }
                if (sz <= 0)
                        return NULL;
                strncat(buf, path, sz - 1);
                return buf;
        } else {
                return path;
        }
}

static const char *cmd_to_str(uint32_t cmd)
{
    switch (cmd) {
        case CMD_NOP:                  return "NOP";
        case CMD_PING:                 return "PING";
        case CMD_PONG:                 return "PONG";
        case CMD_MBOX_LINK_CONNECT:    return "MBOX_LINK_CONNECT";
        case CMD_MBOX_LINK_DISCONNECT: return "MBOX_LINK_DISCONNECT";
        case CMD_MBOX_LINK_PING:       return "MBOX_LINK_PING";
        default:                       return "?";
    }
}

static int mbox_open(const char *path, int dir_flag)
{
    int fd = open(path, dir_flag | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "error: open '%s' failed: %s\n", path, strerror(errno));
        exit(1);
    }
    return fd;
}

static void mbox_close(int fd)
{
    if (fd < 0)
        return;
    int rc = close(fd);
    if (rc < 0)
        fprintf(stderr, "error: close failed: %s\n", strerror(errno));
}

static int mbox_read(int fd)
{
    int rc;
#if defined(SELECT)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    printf("select\n");
    rc = select(fd + 1, &fds, NULL, NULL, /* timeout */ NULL);
    if (rc <= 0) {
        fprintf(stderr, "error: select failed: %s\n", strerror(errno));
        return -1;
    }

    rc = read(fd, msg, sizeof(msg)); // non-blocking
    if (rc < 0) {
        fprintf(stderr, "error: read failed: %s\n", strerror(errno));
        return -1;
    }
#elif defined(POLL)
    struct pollfd fds[1] = { { .fd = fd, .events = POLLIN } };
    printf("poll\n");
    rc = poll(fds, 1, -1);
    if (rc <= 0) {
        fprintf(stderr, "error: poll failed: %s\n", strerror(errno));
        return -1;
    }

    rc = read(fd, msg, sizeof(msg)); // non-blocking
    if (rc < 0) {
        fprintf(stderr, "error: read failed: %s\n", strerror(errno));
        return -1;
    }
#elif defined(EPOLL)
    printf("epoll\n");
    int epfd = epoll_create(1);
    if (epfd < 0) {
        fprintf(stderr, "error: epoll_create failed: %s\n", strerror(errno));
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    if (rc < 0) {
        fprintf(stderr, "error: epoll_ctl failed: %s\n", strerror(errno));
        return -1;
    }

    struct epoll_event events[1];
    rc = epoll_wait(epfd, events, 1, -1);
    if (rc != 1) {
        fprintf(stderr, "error: epoll_wait failed (rc %u): %s\n", rc, strerror(errno));
        return -1;
    }

    if (events[0].data.fd != fd || !(events[0].events & EPOLLIN)) {
        fprintf(stderr, "error: epoll_wait unexpected return\n");
        return -1;
    }

    close(epfd);

    rc = read(fd, msg, sizeof(msg)); // non-blocking
    if (rc < 0) {
        fprintf(stderr, "error: read failed: %s\n", strerror(errno));
        return -1;
    }
#else // !SELECT && !POLL
    do {
        rc = read(fd, msg, sizeof(msg)); // non-blocking
        if (rc < 0) {
            if (errno != EAGAIN) {
                fprintf(stderr, "error: read failed: %s\n", strerror(errno));
                return -1;
            }
            sleep(1);
        }
    } while (rc < 0);
#endif // !SELECT
    return 0;
}

static int mbox_write(int fd)
{
    int rc = write(fd, msg, sizeof(msg));
    if (rc != sizeof(msg)) {
        fprintf(stderr, "error: write failed: %s\n", strerror(errno));
        return rc;
    }

    // poll for ack of our outgoing transmission by remote side
    //
    // NOTE: This wait is needed between back-to-back messages, because
    // The wait the kernel on the remote receiver side has a buffer of size 1
    // message only, and an ACK from that receiver indicates that its buffer is
    // empty and so can receive the next message.
    rc = mbox_read(fd);
    if (rc < 0)
        return rc;
    printf("received ACK\n");

    return 0;
}

static int _mbox_request(uint32_t cmd, unsigned nargs, va_list va)
{
    size_t i;

    printf("sending command: %s\n", cmd_to_str(cmd));

    msg[0] = cmd;
    for (i = 1; i <= nargs && i < MSG_SIZE; ++i)
        msg[i] = va_arg(va, uint32_t); 

    return mbox_write(fd_out);
}

static int mbox_request(uint32_t cmd, unsigned nargs, ...)
{
    va_list va;
    va_start(va, nargs);
    int rc = _mbox_request(cmd, nargs, va);
    va_end(va);
    return rc;
}

static int mbox_rpc(uint32_t cmd, unsigned nargs, ...)
{
    va_list va;
    va_start(va, nargs);

    int rc;

    rc = _mbox_request(cmd, nargs, va);
    if (rc)
        goto cleanup;

    rc = mbox_read(fd_in);
    if (rc)
        goto cleanup;

    print_msg("RPC reply: ", msg, MSG_SIZE);

cleanup:
    va_end(va);
    return rc;
}

int main(int argc, char **argv) {
    const char *devpath_out, *devpath_in;
    const char *devpath_own_out, *devpath_own_in;
    int cpu = -1; // by default don't pin
    int link;
    int rc;
    bool test_own = false;

    if (argc == 1) {
        devpath_out = "0";
        devpath_in = "1";
    } else if (argc == 3 || argc == 5) {
        devpath_out = argv[1];
        devpath_in = argv[2];
        if (argc == 5) {
            devpath_own_out = argv[3];
            devpath_own_in = argv[4];
            test_own = true;
        }
    } else {
        fprintf(stderr, "usage: %s [out_mbox_path|filename|index in_mbox_path|filename|index  [mbox_own_out mbox_own_in]]\n", argv[0]);
        return 1;
    }

    devpath_out = expand_path(devpath_out, devpath_out_buf, sizeof(devpath_out_buf));
    devpath_in = expand_path(devpath_in, devpath_in_buf, sizeof(devpath_in_buf));
    if (test_own) {
        devpath_own_out = expand_path(devpath_own_out, devpath_own_out_buf, sizeof(devpath_own_out_buf));
        devpath_own_in = expand_path(devpath_own_in, devpath_own_in_buf, sizeof(devpath_own_in_buf));
    }

    if (!(devpath_out && devpath_in)) {
        fprintf(stderr, "error: failed to construct path\n");
        return 1;
    }

    printf("out mbox: %s\n", devpath_out);
    printf(" in mbox: %s\n", devpath_in);

    if (test_own) {
        printf("out mbox: %s\n", devpath_own_out);
        printf(" in mbox: %s\n", devpath_own_in);
    }

    if (cpu > 0) {
        // pin to core
        cpu_set_t cpumask;
        CPU_ZERO(&cpumask);
        CPU_SET(cpu, &cpumask);
        sched_setaffinity(0 /* i.e. self */, sizeof(cpu_set_t), &cpumask);
    }

    fd_out = mbox_open(devpath_out, O_RDWR); // read gets us the [N]ACK
    fd_in = mbox_open(devpath_in, O_RDONLY);

    if (test_own) {
        fd_own_out = mbox_open(devpath_own_out, O_RDWR); // read gets us the [N]ACK
        fd_own_in = mbox_open(devpath_own_in, O_RDONLY);
    }

    // In this test case, we send a NOP (which generates no reply)
    // follow by a PING. After NOP before PING, we have to wait for ACK.
    // After ACK for NOP comes, we can send PING. After PING, we can
    // optionally wait for ACK, or just wait for the reply.
    if (mbox_request(CMD_NOP, 0)) // no reply
        goto cleanup;

    if (mbox_rpc(CMD_PING, 1, 42))
        goto cleanup;

    printf("Reply to PING: %s\n", cmd_to_str(msg[0]));

    // Test where Linux is the owner and TRCH is destination (opposite setup
    // from the above test).
    //
    // The '_own_' mailboxes are marked with owner=Linux and destination=TRCH
    // in Linux device tree. When we open these two mailboxes above, they are
    // claimed, and after this TRCH can open them as destination (as opposed to
    // owner, as for the first pair of mailboxes).
    //
    // We ask TRCH to open the mailboxes as destination with MBOX_LINK_CONNECT
    // (via the first pair of mailboxes owned by TRCH). We then ask TRCH to
    // send us a request (via the '_own_' mailboxes).  We handle the request as
    // an PING command and reply back (via the '_own_' mailboxes).
    if (test_own) {
        link = mbox_rpc(CMD_MBOX_LINK_CONNECT, 3, ENDPOINT_HPPS,
                        MBOX_HPPS_TRCH__HPPS_OWN_TRCH,
                        MBOX_HPPS_TRCH__TRCH_HPPS_OWN);
        if (link < 0)
            goto cleanup;

        // must not block because TRCH waits for our reply, so not mbox_rpc
        if (mbox_request(CMD_MBOX_LINK_PING, 1, link))
            goto cleanup;


        rc = mbox_read(fd_own_in);
        if (rc)
            goto cleanup;
        print_msg("request: PING: ", msg, MSG_SIZE);

        // send what we received
        rc = mbox_write(fd_own_out);
        if (rc)
            goto cleanup;


        // read reply to CMD_MBOX_LINK_PING request
        rc = mbox_read(fd_in);
        if (rc)
            goto cleanup;

        if (mbox_rpc(CMD_MBOX_LINK_DISCONNECT, 1, link))
            goto cleanup;
    }

     return 0;

cleanup:
    mbox_close(fd_out);
    mbox_close(fd_in);
    if (test_own) {
        mbox_close(fd_own_out);
        mbox_close(fd_own_in);
    }
    return rc;
}
