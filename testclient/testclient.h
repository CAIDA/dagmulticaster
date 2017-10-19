#ifndef TESTCLIENT_H_
#define TESTCLIENT_H_

#include <pthread.h>
#include "message_queue.h"

typedef struct controlparams {
    char *groupaddr;
    char *portstr;
    char *localiface;
    uint32_t maxthreads;
    pthread_t tid;
} controlparams_t;

typedef struct streamsource {
    char *groupaddr;
    char *localaddr;
    uint16_t port;
} streamsource_t;

typedef struct recvthread {
    streamsource_t *sources;
    uint16_t sourcecount;
    pthread_t tid;
    libtrace_message_queue_t mqueue;
} recvthread_t;


enum {
    NDAG_CLIENT_HALT = 0x01,
    NDAG_CLIENT_RESTARTED = 0x02,
    NDAG_CLIENT_NEWGROUP = 0x03
};

typedef struct ndagreadermessage {
    uint8_t type;
    streamsource_t contents;
} mymessage_t;
#endif



// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
