#define _GNU_SOURCE

#include <tgcompat/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int validate_runtime_directory(const char *path, uid_t expected_uid)
{
    struct stat status;
    if (lstat(path, &status) != 0) {
        return -errno;
    }
    if (!S_ISDIR(status.st_mode)) {
        return -ENOTDIR;
    }
    if (status.st_uid != expected_uid || (status.st_mode & 0777U) != 0700U) {
        return -EPERM;
    }
    return 0;
}

static int runtime_directory(const char *socket_path, char output[PATH_MAX])
{
    if (socket_path == NULL || socket_path[0] != '/') {
        return -EINVAL;
    }
    size_t length = strlen(socket_path);
    if (length == 0 || length >= PATH_MAX ||
        length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return -ENAMETOOLONG;
    }

    const char *separator = strrchr(socket_path, '/');
    if (separator == NULL || separator == socket_path || separator[1] == '\0') {
        return -EINVAL;
    }
    size_t directory_length = (size_t)(separator - socket_path);
    memcpy(output, socket_path, directory_length);
    output[directory_length] = '\0';
    return 0;
}

int tgc_transport_listen(const char *socket_path, uid_t expected_uid)
{
    char directory[PATH_MAX];
    int result = runtime_directory(socket_path, directory);
    if (result != 0) {
        return result;
    }

    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        return -errno;
    }
    result = validate_runtime_directory(directory, expected_uid);
    if (result != 0) {
        return result;
    }

    struct stat existing;
    if (lstat(socket_path, &existing) == 0) {
        return -EADDRINUSE;
    }
    if (errno != ENOENT) {
        return -errno;
    }

    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return -errno;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(socket_path);
    memcpy(address.sun_path, socket_path, path_length + 1);

    mode_t old_mask = umask(0077);
    int bind_result = bind(socket_fd, (const struct sockaddr *)&address,
                           (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                       path_length + 1));
    int bind_errno = errno;
    (void)umask(old_mask);
    if (bind_result != 0) {
        (void)close(socket_fd);
        return -bind_errno;
    }

    if (chmod(socket_path, 0600) != 0) {
        int saved_errno = errno;
        (void)close(socket_fd);
        (void)unlink(socket_path);
        return -saved_errno;
    }

    struct stat created;
    if (lstat(socket_path, &created) != 0) {
        int saved_errno = errno;
        (void)close(socket_fd);
        (void)unlink(socket_path);
        return -saved_errno;
    }
    if (!S_ISSOCK(created.st_mode) || created.st_uid != expected_uid ||
        (created.st_mode & 0777U) != 0600U) {
        (void)close(socket_fd);
        (void)unlink(socket_path);
        return -EPERM;
    }
    if (listen(socket_fd, 16) != 0) {
        int saved_errno = errno;
        (void)close(socket_fd);
        (void)unlink(socket_path);
        return -saved_errno;
    }
    return socket_fd;
}

int tgc_transport_authenticate(int socket_fd, uid_t expected_uid,
                               pid_t *peer_pid)
{
    if (socket_fd < 0 || peer_pid == NULL) {
        return -EINVAL;
    }
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) !=
        0) {
        return -errno;
    }
    if (length != sizeof(credentials) || credentials.pid <= 0 ||
        credentials.uid != expected_uid) {
        return -EACCES;
    }
    *peer_pid = credentials.pid;
    return 0;
}

static int receive_exact(int socket_fd, uint8_t *output, size_t length,
                         int clean_eof_allowed)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = recv(socket_fd, output + offset, length - offset, 0);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received == 0) {
            return clean_eof_allowed != 0 && offset == 0 ? TGC_TRANSPORT_EOF
                                                         : -EPROTO;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

int tgc_transport_receive(int socket_fd, struct tgc_protocol_packet *packet)
{
    if (socket_fd < 0 || packet == NULL) {
        return -EINVAL;
    }
    uint8_t wire_header[TGC_PROTOCOL_HEADER_SIZE];
    int result = receive_exact(socket_fd, wire_header, sizeof(wire_header), 1);
    if (result != 0) {
        return result;
    }

    memset(packet, 0, sizeof(*packet));
    result = tgc_protocol_decode_header(wire_header, &packet->header);
    if (result != 0) {
        return result;
    }
    return receive_exact(socket_fd, packet->payload,
                         packet->header.payload_length, 0);
}

static int send_exact(int socket_fd, const uint8_t *input, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(socket_fd, input + offset, length - offset,
                            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent == 0) {
            return -EPIPE;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

int tgc_transport_send(int socket_fd,
                       const struct tgc_protocol_packet *packet)
{
    if (socket_fd < 0 || packet == NULL) {
        return -EINVAL;
    }
    uint8_t wire_header[TGC_PROTOCOL_HEADER_SIZE];
    int result = tgc_protocol_encode_header(wire_header, &packet->header);
    if (result != 0) {
        return result;
    }
    result = send_exact(socket_fd, wire_header, sizeof(wire_header));
    if (result != 0) {
        return result;
    }
    return send_exact(socket_fd, packet->payload,
                      packet->header.payload_length);
}
