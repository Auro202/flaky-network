/* SENDER — forwards each frame once, plus interleaved XOR parity (FEC).
 *
 * Ports:
 *   bind 47010  <- harness source, frame i at t0 + i*20ms (seq32 + 160 payload)
 *   send 47001  -> relay uplink toward receiver (MY wire format below)
 *
 * Wire format (sender -> receiver):
 *   DATA   : [0x00][seq:4 BE][payload:160]                    = 165 bytes
 *   PARITY : [0x01][base:4 BE][k:1][stride:1][xor_payload:160] = 167 bytes
 *
 * FEC scheme — interleaved XOR parity:
 *   K      = frames per parity group  -> overhead ~ 1 + 1/K
 *   STRIDE = interleave depth. A group's K members are STRIDE apart, so a
 *            burst of up to STRIDE consecutive losses lands in distinct
 *            groups (each loses at most one frame = recoverable).
 *   Parity for a group is XOR of its K payloads, emitted the instant the
 *   group's last frame arrives.
 *
 * Grouping: within each block of K*STRIDE consecutive frames, group j
 * (0 <= j < STRIDE) owns members {block + j + m*STRIDE : 0 <= m < K}.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define HARNESS_LEN (4 + PAYLOAD_LEN)   /* seq32 + payload from source */

/* FEC parameters — tuned in Step 5. */
#define K       2      /* frames per parity group */
#define STRIDE  1      /* interleave depth (burst tolerance) */

#define MAX_STRIDE 64

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010"); return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family      = AF_INET;
    relay.sin_port        = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* One XOR accumulator per group within the current block. */
    uint8_t acc[MAX_STRIDE][PAYLOAD_LEN];
    memset(acc, 0, sizeof acc);

    const int block = K * STRIDE;

    uint8_t in[2048];
    uint8_t out[2048];

    for (;;) {
        ssize_t n = recvfrom(in_fd, in, sizeof in, 0, NULL, NULL);
        if (n < HARNESS_LEN) continue;

        uint32_t seq = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16
                     | (uint32_t)in[2] <<  8 | (uint32_t)in[3];
        const uint8_t *payload = in + 4;

        /* Forward the data packet immediately. */
        out[0] = 0x00;
        memcpy(out + 1, in, HARNESS_LEN);   /* seq32 + payload, verbatim */
        sendto(out_fd, out, 1 + HARNESS_LEN, 0,
               (struct sockaddr *)&relay, sizeof relay);

        /* Fold into this frame's parity group. */
        int within = (int)(seq % (uint32_t)block);
        int group  = within % STRIDE;   /* 0..STRIDE-1 */
        int member = within / STRIDE;   /* 0..K-1     */

        for (int b = 0; b < PAYLOAD_LEN; b++)
            acc[group][b] ^= payload[b];

        /* Last member of the group just arrived — emit its parity. */
        if (member == K - 1) {
            uint32_t base = seq - (uint32_t)((K - 1) * STRIDE);  /* first seq */
            out[0] = 0x01;
            out[1] = (uint8_t)(base >> 24);
            out[2] = (uint8_t)(base >> 16);
            out[3] = (uint8_t)(base >>  8);
            out[4] = (uint8_t)(base);
            out[5] = (uint8_t)K;
            out[6] = (uint8_t)STRIDE;
            memcpy(out + 7, acc[group], PAYLOAD_LEN);
            sendto(out_fd, out, 7 + PAYLOAD_LEN, 0,
                   (struct sockaddr *)&relay, sizeof relay);

            memset(acc[group], 0, PAYLOAD_LEN);   /* reset for next block */
        }
    }
    return 0;
}
