#include "ipc.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static struct sockaddr_in loopback_address(unsigned short port)
{
    return (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };
}

static int tcp_setup(IPCChannel *channel)
{
    channel->read_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (channel->read_fd == -1) {
        return -1;
    }

    struct sockaddr_in address = loopback_address(0);
    socklen_t length = sizeof address;
    if (bind(channel->read_fd, (const struct sockaddr *)&address, length) == -1 ||
        listen(channel->read_fd, 1) == -1 ||
        getsockname(channel->read_fd, (struct sockaddr *)&address, &length) == -1) {
        return -1;
    }
    channel->port = ntohs(address.sin_port);
    return 0;
}

static int connect_client(int fd, unsigned short port)
{
    struct sockaddr_in address = loopback_address(port);
    if (connect(fd, (const struct sockaddr *)&address, sizeof address) == 0) {
        return 0;
    }
    if (errno != EINTR || ipc_interrupted) {
        return -1;
    }

    /* An interrupted connect can continue in the kernel. Wait for its result. */
    struct pollfd ready = { .fd = fd, .events = POLLOUT };
    int result;
    do {
        result = poll(&ready, 1, -1);
    } while (result == -1 && errno == EINTR && !ipc_interrupted);
    if (result == -1) {
        return -1;
    }
    int error;
    socklen_t length = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
        return -1;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static int tcp_start(IPCChannel *channel, bool sender)
{
    if (sender) {
        if (ipc_close(&channel->read_fd) == -1) {
            return -1;
        }
        channel->write_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (channel->write_fd == -1) {
            return -1;
        }
        return connect_client(channel->write_fd, channel->port);
    }

    int connection;
    do {
        connection = accept(channel->read_fd, NULL, NULL);
    } while (connection == -1 && errno == EINTR && !ipc_interrupted);
    if (connection == -1) {
        return -1;
    }
    int result = ipc_close(&channel->read_fd);
    channel->read_fd = connection;
    return result;
}

static int tcp_send(IPCChannel *channel, const IPCMessage *message)
{
    const unsigned char *bytes = (const unsigned char *)message;
    size_t remaining = sizeof *message;

    while (remaining > 0) {
        ssize_t written = send(channel->write_fd, bytes, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            bytes += written;
            remaining -= (size_t)written;
        } else if (written == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR || ipc_interrupted) {
            return -1;
        }
    }
    return 0;
}

static ssize_t receive_bytes(int fd, void *buffer, size_t length)
{
    ssize_t received;
    do {
        received = recv(fd, buffer, length, 0);
    } while (received == -1 && errno == EINTR && !ipc_interrupted);
    return received;
}

static int tcp_receive(IPCChannel *channel, IPCMessage *message)
{
    unsigned char *bytes = (unsigned char *)message;
    size_t remaining = sizeof *message;

    while (remaining > 0) {
        ssize_t received = receive_bytes(channel->read_fd, bytes, remaining);
        if (received == -1) {
            return -1;
        }
        if (received == 0) {
            errno = EPROTO;
            return -1;
        }
        bytes += received;
        remaining -= (size_t)received;
    }
    return 0;
}

static int tcp_finish(IPCChannel *channel, bool sender)
{
    if (sender) {
        return shutdown(channel->write_fd, SHUT_WR);
    }

    unsigned char extra;
    ssize_t received = receive_bytes(channel->read_fd, &extra, sizeof extra);
    if (received == 0) {
        return 0;
    }
    if (received > 0) {
        errno = EPROTO;
    }
    return -1;
}

static int tcp_cleanup(IPCChannel *channel, bool owner)
{
    (void)owner;
    int result = ipc_close(&channel->read_fd);
    int saved_error = errno;
    if (ipc_close(&channel->write_fd) == -1) {
        return -1;
    }
    if (result == -1) {
        errno = saved_error;
    }
    return result;
}

const IPCTransport tcp_transport = {
    .name = "TCP sockets",
    .setup = tcp_setup,
    .start = tcp_start,
    .send = tcp_send,
    .receive = tcp_receive,
    .finish = tcp_finish,
    .cleanup = tcp_cleanup
};
