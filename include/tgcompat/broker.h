#ifndef TGCOMPAT_BROKER_H
#define TGCOMPAT_BROKER_H

#include <tgcompat/protocol.h>
#include <tgcompat/sem_store.h>

/*
 * Dispatch one already-framed request. Transport and peer authentication stay
 * outside this boundary so malformed-message behavior is unit-testable.
 */
int tgc_broker_dispatch(struct tgc_sem_store *store,
                        const struct tgc_protocol_packet *request,
                        struct tgc_protocol_packet *response);

#endif
