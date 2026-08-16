#ifndef TGCOMPAT_BROKER_H
#define TGCOMPAT_BROKER_H

#include <tgcompat/protocol.h>
#include <tgcompat/sem_store.h>

#include <stdint.h>
#include <sys/types.h>

/*
 * Dispatch one already-framed request. Transport and peer authentication stay
 * outside this boundary so malformed-message behavior is unit-testable.
 */
int tgc_broker_dispatch(struct tgc_sem_store *store,
                        const struct tgc_protocol_packet *request,
                        int32_t actor_pid,
struct tgc_protocol_packet *response);

struct tgc_broker;

struct tgc_broker *tgc_broker_create(void);
void tgc_broker_destroy(struct tgc_broker *broker);

/* Serve requests until the authenticated peer closes its socket. */
int tgc_broker_serve_connection(struct tgc_broker *broker, int socket_fd,
                                uid_t expected_uid);

#endif
