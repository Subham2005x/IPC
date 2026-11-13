#include "ipc.h"

#include <errno.h>
#include <unistd.h>

static int pipe_setup(IPCChannel *channel)
{
    int fds[2];
    if (pipe(fds) == -1) {
        return -1;
    }
    channel->read_fd = fds[0];
    channel->write_fd = fds[1];
    return 0;
}

static int pipe_start(IPCChannel *channel, bool sender)
{
    return ipc_close(sender ? &channel->read_fd : &channel->write_fd);
}

static int pipe_send(IPCChannel *channel, const IPCMessage *message)
{
    return ipc_write_full(channel->write_fd, message, sizeof *message);
}

static int pipe_receive(IPCChannel *channel, IPCMessage *message)
{
    int status = ipc_read_full(channel->read_fd, message, sizeof *message);
    if (status == 1) {
        return 0;
    }
    if (status == 0) {
        errno = EPROTO;
    }
    return -1;
}

static int pipe_finish(IPCChannel *channel, bool sender)
{
    if (sender) {
        return ipc_close(&channel->write_fd);
    }
    unsigned char extra;
    int status = ipc_read_full(channel->read_fd, &extra, sizeof extra);
    if (status == 1) {
        errno = EPROTO;
    }
    return status == 0 ? 0 : -1;
}

static int pipe_cleanup(IPCChannel *channel, bool owner)
{
    (void)owner;
    int status = ipc_close(&channel->read_fd);
    int saved_error = errno;
    if (ipc_close(&channel->write_fd) == -1) {
        return -1;
    }
    errno = saved_error;
    return status;
}

const IPCTransport pipe_transport = {
    .name = "Anonymous pipe",
    .setup = pipe_setup,
    .start = pipe_start,
    .send = pipe_send,
    .receive = pipe_receive,
    .finish = pipe_finish,
    .cleanup = pipe_cleanup
};
