#include "ipc.h"

#include <errno.h>
#include <unistd.h>

volatile sig_atomic_t ipc_interrupted;

static void mark_interrupted(int signal_number)
{
    ipc_interrupted = signal_number;
}

int ipc_install_signals(void)
{
    struct sigaction action = { .sa_handler = mark_interrupted };
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGALRM, &action, NULL) == -1) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

int ipc_close(int *fd)
{
    int current = *fd;
    *fd = -1;
    return current < 0 ? 0 : close(current);
}

int ipc_read_full(int fd, void *buffer, size_t length)
{
    unsigned char *bytes = buffer;
    size_t received = 0;

    while (received < length) {
        if (ipc_interrupted) {
            errno = EINTR;
            return -1;
        }
        ssize_t count = read(fd, bytes + received, length - received);
        if (count > 0) {
            received += (size_t)count;
        } else if (count == 0) {
            if (received == 0) {
                return 0;
            }
            errno = EPROTO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }

    return 1;
}

int ipc_write_full(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    size_t sent = 0;

    while (sent < length) {
        if (ipc_interrupted) {
            errno = EINTR;
            return -1;
        }
        ssize_t count = write(fd, bytes + sent, length - sent);
        if (count > 0) {
            sent += (size_t)count;
        } else if (count == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}
