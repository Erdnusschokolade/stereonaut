#define _GNU_SOURCE
#include "pcvr-packet-ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libavutil/buffer.h>

static int write_all(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        offset += (size_t)written;
    }
    return 0;
}

int pcvr_packet_send(int socket_fd, struct nv12_ipc_message *message,
                     const AVPacket *packet)
{
    if (!message || !packet || packet->size < 0)
        return -1;
    int fd = memfd_create("stereonaut-vulkan-packet", MFD_CLOEXEC);
    if (fd < 0 || write_all(fd, packet->data, (size_t)packet->size) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    message->packet_size = (uint64_t)packet->size;
    char control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = {.iov_base = message, .iov_len = sizeof(*message)};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = control,
                            .msg_controllen = sizeof(control)};
    struct cmsghdr *item = CMSG_FIRSTHDR(&header);
    item->cmsg_level = SOL_SOCKET;
    item->cmsg_type = SCM_RIGHTS;
    item->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(item), &fd, sizeof(fd));
    ssize_t sent;
    for (;;) {
        sent = sendmsg(socket_fd, &header, MSG_NOSIGNAL);
        if (sent >= 0 || (errno != EINTR && errno != EAGAIN &&
                          errno != EWOULDBLOCK))
            break;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd writable = {.fd = socket_fd, .events = POLLOUT};
            if (poll(&writable, 1, 1000) <= 0)
                break;
        }
    }
    close(fd);
    return sent == (ssize_t)sizeof(*message) ? 0 : -1;
}

static void release_mapped_packet(void *opaque, uint8_t *data)
{
    size_t size = (size_t)(uintptr_t)opaque;
    if (data && size)
        munmap(data, size);
}

int pcvr_packet_receive(const struct msghdr *received,
                        const struct nv12_ipc_message *message,
                        AVPacket **packet)
{
    *packet = NULL;
    if (!received || !message || !message->packet_size ||
        message->packet_size > INT_MAX)
        return -1;
    struct cmsghdr *item = CMSG_FIRSTHDR((struct msghdr *)received);
    if (!item || item->cmsg_level != SOL_SOCKET ||
        item->cmsg_type != SCM_RIGHTS ||
        item->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(item), sizeof(fd));
    size_t size = (size_t)message->packet_size;
    void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        return -1;
    AVBufferRef *buffer = av_buffer_create(mapping, size,
                                           release_mapped_packet,
                                           (void *)(uintptr_t)size, 0);
    if (!buffer) {
        munmap(mapping, size);
        return -1;
    }
    AVPacket *result = av_packet_alloc();
    if (!result) {
        av_buffer_unref(&buffer);
        return -1;
    }
    result->buf = buffer;
    result->data = buffer->data;
    result->size = (int)size;
    *packet = result;
    return 0;
}
