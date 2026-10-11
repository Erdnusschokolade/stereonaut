#ifndef STEREONAUT_PCVR_PACKET_IPC_H
#define STEREONAUT_PCVR_PACKET_IPC_H

#include <stddef.h>
#include <sys/socket.h>

#include <libavcodec/packet.h>

#include "nv12-playback-ipc.h"

/* A packet crosses the local SOCK_SEQPACKET socket as an immutable memfd.
 * This keeps large HEVC access units out of the socket payload and lets the
 * renderer use the packet without a second CPU-side copy. */
int pcvr_packet_send(int socket_fd, struct nv12_ipc_message *message,
                     const AVPacket *packet);
int pcvr_packet_receive(const struct msghdr *received,
                        const struct nv12_ipc_message *message,
                        AVPacket **packet);

#endif
