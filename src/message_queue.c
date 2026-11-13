#include "ipc.h"

#include <errno.h>
#include <sys/ipc.h>
#include <sys/msg.h>

typedef struct {
    long type;
    IPCMessage payload;
} QueueMessage;

_Static_assert(offsetof(QueueMessage, payload) == sizeof(long),
               "System V message text must immediately follow its type");

static int queue_setup(IPCChannel *channel)
{
    channel->id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    return channel->id == -1 ? -1 : 0;
}

static int queue_send(IPCChannel *channel, const IPCMessage *message)
{
    QueueMessage record = { .type = IPC_QUEUE_MESSAGE_TYPE, .payload = *message };
    int result;

    /* The length counts the message text only, excluding the type field. */
    do {
        result = msgsnd(channel->id, &record, sizeof record.payload, 0);
    } while (result == -1 && errno == EINTR && !ipc_interrupted);

    return result;
}

static int queue_receive(IPCChannel *channel, IPCMessage *message)
{
    QueueMessage record;
    ssize_t length;

    do {
        length = msgrcv(channel->id, &record, sizeof record.payload,
                        IPC_QUEUE_MESSAGE_TYPE, 0);
    } while (length == -1 && errno == EINTR && !ipc_interrupted);

    if (length == -1) {
        return -1;
    }
    if ((size_t)length != sizeof record.payload) {
        errno = EPROTO;
        return -1;
    }
    *message = record.payload;
    return 0;
}

static int queue_cleanup(IPCChannel *channel, bool owner)
{
    if (owner && channel->id != -1) {
        return msgctl(channel->id, IPC_RMID, NULL);
    }
    return 0;
}

const IPCTransport queue_transport = {
    .name = "System V Message Queue",
    .setup = queue_setup,
    .send = queue_send,
    .receive = queue_receive,
    .cleanup = queue_cleanup
};
