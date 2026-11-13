#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_FILENAME "/messages.fifo"

static int open_fifo(const char *path, int flags)
{
    struct stat path_info;
    if (lstat(path, &path_info) == -1) {
        return -1;
    }
    if (!S_ISFIFO(path_info.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    int fd;
    do {
        if (ipc_interrupted) {
            errno = EINTR;
            return -1;
        }
        fd = open(path, flags | O_NOFOLLOW);
    } while (fd == -1 && errno == EINTR && !ipc_interrupted);
    if (fd == -1) {
        return -1;
    }

    /* Verify the open descriptor even if the pathname was replaced. */
    struct stat fd_info;
    int result = fstat(fd, &fd_info);
    if (result == -1 || !S_ISFIFO(fd_info.st_mode) ||
        fd_info.st_dev != path_info.st_dev || fd_info.st_ino != path_info.st_ino) {
        int error = result == -1 ? errno : EINVAL;
        ipc_close(&fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int setup_fifo(IPCChannel *channel)
{
    strcpy(channel->path, "/tmp/ipc-fifo-XXXXXX");
    if (mkdtemp(channel->path) == NULL) {
        channel->path[0] = '\0';
        return -1;
    }

    char path[sizeof channel->path + sizeof FIFO_FILENAME];
    snprintf(path, sizeof path, "%s%s", channel->path, FIFO_FILENAME);
    return mkfifo(path, S_IRUSR | S_IWUSR);
}

static int start_fifo(IPCChannel *channel, bool sender)
{
    char path[sizeof channel->path + sizeof FIFO_FILENAME];
    snprintf(path, sizeof path, "%s%s", channel->path, FIFO_FILENAME);
    int *fd = sender ? &channel->write_fd : &channel->read_fd;
    *fd = open_fifo(path, sender ? O_WRONLY : O_RDONLY);
    return *fd == -1 ? -1 : 0;
}

static int send_fifo(IPCChannel *channel, const IPCMessage *message)
{
    return ipc_write_full(channel->write_fd, message, sizeof *message);
}

static int receive_fifo(IPCChannel *channel, IPCMessage *message)
{
    int result = ipc_read_full(channel->read_fd, message, sizeof *message);
    if (result == 0) {
        errno = EPROTO;
    }
    return result == 1 ? 0 : -1;
}

static int finish_fifo(IPCChannel *channel, bool sender)
{
    if (sender) {
        return ipc_close(&channel->write_fd);
    }

    unsigned char extra;
    int result = ipc_read_full(channel->read_fd, &extra, sizeof extra);
    if (result == 1) {
        errno = EPROTO;
    }
    return result == 0 ? 0 : -1;
}

static int cleanup_fifo(IPCChannel *channel, bool owner)
{
    int error = 0;
    if (ipc_close(&channel->read_fd) == -1) {
        error = errno;
    }
    if (ipc_close(&channel->write_fd) == -1 && error == 0) {
        error = errno;
    }
    if (owner && channel->path[0] != '\0') {
        char path[sizeof channel->path + sizeof FIFO_FILENAME];
        snprintf(path, sizeof path, "%s%s", channel->path, FIFO_FILENAME);
        if (unlink(path) == -1 && errno != ENOENT && error == 0) {
            error = errno;
        }
        if (rmdir(channel->path) == -1 && error == 0) {
            error = errno;
        }
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

const IPCTransport fifo_transport = {
    .name = "Named Pipes (FIFOs)",
    .setup = setup_fifo,
    .start = start_fifo,
    .send = send_fifo,
    .receive = receive_fifo,
    .finish = finish_fifo,
    .cleanup = cleanup_fifo
};

/* A separately launched receiver has no expected message inherited by fork. */
typedef struct {
    IPCMessage message;
    uint32_t text_checksum;
} FIFOFrame;

_Static_assert(sizeof(FIFOFrame) <= _POSIX_PIPE_BUF,
               "Standalone FIFO frames must fit in one atomic pipe write");

static uint32_t text_checksum(const IPCMessage *message)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < sizeof message->message; ++i) {
        hash ^= (unsigned char)message->message[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

int fifo_send(const char *path, const char *text)
{
    FIFOFrame frame = { .message = { .message_id = 1, .sender_pid = getpid(),
                                     .sender_name = "FIFO sender" } };
    size_t text_length = strlen(text);
    if (text_length >= sizeof frame.message.message) {
        fprintf(stderr, "FIFO message must fit in %zu bytes.\n",
                sizeof frame.message.message - 1);
        return EXIT_FAILURE;
    }
    memcpy(frame.message.message, text, text_length + 1);
    frame.text_checksum = text_checksum(&frame.message);

    int fd = open_fifo(path, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open FIFO for writing (start the receiver first)");
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    if (ipc_write_full(fd, &frame, sizeof frame) == -1) {
        perror("write FIFO frame");
        status = EXIT_FAILURE;
    }
    if (ipc_close(&fd) == -1) {
        perror("close FIFO writer");
        status = EXIT_FAILURE;
    }
    if (status == EXIT_SUCCESS &&
        (printf("Sender PID %jd wrote message #%d: %s\n",
                (intmax_t)frame.message.sender_pid, frame.message.message_id,
                frame.message.message) < 0 || fflush(stdout) == EOF)) {
        perror("print FIFO sender message");
        status = EXIT_FAILURE;
    }
    return status;
}

static int receive_standalone_frame(int fd)
{
    /* A reader opened in nonblocking mode can announce readiness before poll. */
    struct pollfd incoming = { .fd = fd, .events = POLLIN };
    if (ipc_interrupted) {
        errno = EINTR;
        return -1;
    }
    int result = poll(&incoming, 1, 30000);
    if (result <= 0) {
        if (result == 0) {
            errno = ETIMEDOUT;
        }
        return -1;
    }

    FIFOFrame frame;
    result = ipc_read_full(fd, &frame, sizeof frame);
    if (result == -1) {
        return -1;
    }
    if (result == 0 || frame.message.message_id != 1 ||
        frame.message.sender_pid <= 0 ||
        memchr(frame.message.message, '\0', sizeof frame.message.message) == NULL ||
        frame.text_checksum != text_checksum(&frame.message)) {
        errno = EPROTO;
        return -1;
    }

    if (printf("Receiver PID %jd verified message #%d from PID %jd: %s\n",
               (intmax_t)getpid(), frame.message.message_id,
               (intmax_t)frame.message.sender_pid, frame.message.message) < 0 ||
        puts("Delivery successful.") == EOF || fflush(stdout) == EOF) {
        return -1;
    }
    return 0;
}

int fifo_receive(const char *path)
{
    if (mkfifo(path, S_IRUSR | S_IWUSR) == -1) {
        perror("mkfifo");
        return EXIT_FAILURE;
    }

    int fd = open_fifo(path, O_RDONLY | O_NONBLOCK);
    int status = EXIT_FAILURE;
    if (fd == -1) {
        perror("open FIFO for reading");
    } else if (printf("Receiver PID %jd waiting up to 30 seconds on %s\n",
                      (intmax_t)getpid(), path) < 0 || fflush(stdout) == EOF) {
        perror("print FIFO readiness");
    } else if (receive_standalone_frame(fd) == -1) {
        perror("receive FIFO frame");
    } else {
        status = EXIT_SUCCESS;
    }

    if (ipc_close(&fd) == -1) {
        perror("close FIFO reader");
        status = EXIT_FAILURE;
    }
    if (unlink(path) == -1) {
        perror("unlink FIFO");
        status = EXIT_FAILURE;
    }
    return status;
}
