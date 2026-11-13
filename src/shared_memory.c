#include "ipc.h"

#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>

typedef struct {
    sem_t empty;
    sem_t full;
    bool empty_initialized;
    bool full_initialized;
    IPCMessage message;
} SharedSlot;

static int shared_setup(IPCChannel *channel)
{
    channel->id = shmget(IPC_PRIVATE, sizeof(SharedSlot), IPC_CREAT | 0600);
    if (channel->id == -1) {
        return -1;
    }

    SharedSlot *slot = shmat(channel->id, NULL, 0);
    if (slot == (void *)-1) {
        return -1;
    }
    channel->shared = slot;

    /* fork() inherits the mapping. Linux deletes it after the last detach. */
    if (shmctl(channel->id, IPC_RMID, NULL) == -1) {
        return -1;
    }
    channel->id = -1;

    if (sem_init(&slot->empty, 1, 1) == -1) {
        return -1;
    }
    slot->empty_initialized = true;
    if (sem_init(&slot->full, 1, 0) == -1) {
        return -1;
    }
    slot->full_initialized = true;
    return 0;
}

static int wait_for_slot(sem_t *semaphore)
{
    int result;
    do {
        result = sem_wait(semaphore);
    } while (result == -1 && errno == EINTR && !ipc_interrupted);
    return result;
}

static int shared_send(IPCChannel *channel, const IPCMessage *message)
{
    SharedSlot *slot = channel->shared;
    if (wait_for_slot(&slot->empty) == -1) {
        return -1;
    }
    slot->message = *message;
    return sem_post(&slot->full);
}

static int shared_receive(IPCChannel *channel, IPCMessage *message)
{
    SharedSlot *slot = channel->shared;
    if (wait_for_slot(&slot->full) == -1) {
        return -1;
    }
    *message = slot->message;
    return sem_post(&slot->empty);
}

static int shared_cleanup(IPCChannel *channel, bool owner)
{
    SharedSlot *slot = channel->shared;
    int error = 0;

    if (slot != NULL) {
        if (owner && slot->full_initialized && sem_destroy(&slot->full) == -1) {
            error = errno;
            perror("sem_destroy full");
        }
        if (owner && slot->empty_initialized && sem_destroy(&slot->empty) == -1) {
            error = errno;
            perror("sem_destroy empty");
        }
        if (shmdt(slot) == -1) {
            error = errno;
            perror("shmdt");
        }
    }

    if (owner && channel->id != -1 && shmctl(channel->id, IPC_RMID, NULL) == -1) {
        error = errno;
        perror("shmctl IPC_RMID");
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

const IPCTransport shared_transport = {
    .name = "Shared Memory + Semaphores",
    .setup = shared_setup,
    .send = shared_send,
    .receive = shared_receive,
    .cleanup = shared_cleanup
};
