#ifndef TGCOMPAT_TRANSPORT_H
#define TGCOMPAT_TRANSPORT_H

#include <tgcompat/protocol.h>

#include <sys/types.h>

enum {
    TGC_TRANSPORT_EOF = 1,
};

struct tgc_peer_credentials {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

/*
 * Create a mode-0600 AF_UNIX listener below an exact mode-0700 directory
 * owned by expected_uid. The final directory is created when absent; its
 * parent must already exist. Existing socket paths are never removed.
 */
int tgc_transport_listen(const char *socket_path, uid_t expected_uid);

/* Validate and connect to an owner-only broker socket. */
int tgc_transport_connect(const char *socket_path, uid_t expected_uid);

/* Authenticate an already-connected Linux Unix socket with SO_PEERCRED. */
int tgc_transport_authenticate(int socket_fd, uid_t expected_uid,
                               pid_t *peer_pid);
int tgc_transport_get_credentials(int socket_fd, uid_t expected_uid,
                                  struct tgc_peer_credentials *credentials);

/* Receive or send one complete bounded protocol packet. */
int tgc_transport_receive(int socket_fd, struct tgc_protocol_packet *packet);
int tgc_transport_send(int socket_fd,
                       const struct tgc_protocol_packet *packet);

#endif
