#define _POSIX_C_SOURCE 200809L

/*
 * Phase 3 test:
 *  Verify bootstrap metadata serialization and exact-length TCP-style I/O.
 *
 * A Unix socket pair provides two connected byte streams locally. It tests the
 * protocol helpers without requiring two course nodes or an RDMA device.
 */
#define main ring_allreduce_program_main
#include "../ring_allreduce.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int metadata_equal(const pg_metadata_t *left, const pg_metadata_t *right)
{
    return left->rank == right->rank &&
           left->size == right->size &&
           left->qpn == right->qpn &&
           left->psn == right->psn &&
           left->lid == right->lid &&
           left->buffer_addr == right->buffer_addr &&
           left->rkey == right->rkey &&
           memcmp(left->gid.raw, right->gid.raw, sizeof(left->gid.raw)) == 0;
}

static int test_metadata_round_trip(void)
{
    pg_metadata_t original = {0};
    pg_metadata_t decoded = {0};
    unsigned char wire[PG_METADATA_WIRE_SIZE];
    int sockets[2];
    int i;

    original.rank = 3;
    original.size = 4;
    original.qpn = 0x123456;
    original.psn = 0x654321;
    original.lid = 0x4321;
    original.buffer_addr = UINT64_C(0x123456789abcdef0);
    original.rkey = 0xaabbccdd;
    for (i = 0; i < (int)sizeof(original.gid.raw); ++i) {
        original.gid.raw[i] = (uint8_t)(i + 1);
    }

    serialize_metadata(&original, wire);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    /* Two writes model a message arriving in multiple TCP read fragments. */
    if (write_full(sockets[0], wire, 7) != 0 ||
        write_full(sockets[0], wire + 7, sizeof(wire) - 7) != 0 ||
        read_full(sockets[1], wire, sizeof(wire)) != 0 ||
        deserialize_metadata(wire, &decoded) != 0 ||
        !metadata_equal(&original, &decoded)) {
        fprintf(stderr, "metadata socket round trip failed\n");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

static int test_invalid_io(void)
{
    unsigned char byte = 0;
    pg_metadata_t metadata = {0};

    serialize_metadata(NULL, NULL);
    if (write_full(-1, &byte, sizeof(byte)) != -1 ||
        read_full(-1, &byte, sizeof(byte)) != -1 ||
        write_full(1, NULL, sizeof(byte)) != -1 ||
        read_full(0, NULL, sizeof(byte)) != -1 ||
        deserialize_metadata(NULL, &metadata) != -1) {
        fprintf(stderr, "invalid bootstrap I/O was accepted\n");
        return -1;
    }

    return 0;
}

static int test_peer_metadata_validation(void)
{
    pg_metadata_t metadata = {
        .rank = 1,
        .size = 4,
        .qpn = 7,
        .buffer_addr = 0x1000,
        .rkey = 9
    };

    if (validate_peer_metadata(&metadata, 1, 4) != 0 ||
        validate_peer_metadata(&metadata, 0, 4) != -1 ||
        validate_peer_metadata(&metadata, 1, 3) != -1) {
        fprintf(stderr, "peer metadata validation returned an unexpected result\n");
        return -1;
    }

    metadata.qpn = 0;
    if (validate_peer_metadata(&metadata, 1, 4) != -1) {
        fprintf(stderr, "peer metadata accepted an invalid QPN\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    if (test_metadata_round_trip() != 0 || test_invalid_io() != 0 ||
        test_peer_metadata_validation() != 0) {
        return EXIT_FAILURE;
    }

    printf("Phase 3 bootstrap helper tests passed\n");
    return EXIT_SUCCESS;
}
