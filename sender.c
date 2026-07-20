/* SENDER — forwards each frame once, plus TWO layers of XOR parity (FEC).
 *
 * Ports:
 *   bind 47010  <- harness source, frame i at t0 + i*20ms (seq32 + 160 payload)
 *   send 47001  -> relay uplink toward receiver (MY wire format below)
 *
 * Wire format (sender -> receiver):
 *   DATA   : [0x00][seq:4 BE][payload:160]                            = 165 B
 *   PARITY : [0x01][base:4 BE][k:1][layer:1][off:1][xor_payload:160] = 168 B
 *
 * FEC scheme — two overlapping XOR parity layers.
 *   A layer with group size K covers disjoint runs of K consecutive frames,
 *   starting at `offset`. Its parity is the XOR of those K payloads, emitted
 *   the instant the group's last frame arrives. Any ONE lost frame in a
 *   group is recoverable as parity XOR the survivors.
 *
 *   Two layers with different K and offset put every frame in two DIFFERENT
 *   groups, so a frame survives unless BOTH of its groups are unrecoverable.
 *   With one layer the residual miss rate is
 *       P(lost) * [1 - P(partner ok)*P(parity ok)]
 *   which on a 5% loss link is ~0.5% — right at the 1% cap. A second,
 *   independent layer squares the failure term and drops it to ~0.1%.
 *
 * Timing constraint (this is what sets K):
 *   A group's parity cannot be computed until its LAST frame arrives, so
 *   parity lags the group's EARLIEST frame by (K-1)*20ms. That parity is
 *   only useful if it still beats that frame's deadline. Hence small K:
 *   K=2 lags 20ms, K=3 lags 40ms, K=4 would lag 60ms and arrive too late
 *   to rescue the head of each group on a high-jitter link.
 *
 * Budget: 165*1500 data + 168*(750+500) parity = 1.91x, under the 2.0x cap.
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

#define NUM_LAYERS 2

/* Per-layer FEC geometry: group size and starting offset. Offsets differ so
 * the two layers' group boundaries never line up, giving each frame two
 * independent recovery chances. Override K via env for tuning sweeps. */
#define DEFAULT_K0  2
#define DEFAULT_K1  3

typedef struct {
    int     k;
    int     offset;
    uint8_t acc[PAYLOAD_LEN];   /* running XOR of the current group */
} Layer;

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

int main(void) {
    Layer layers[NUM_LAYERS] = {
        { env_int("FEC_K0", DEFAULT_K0), 0, {0} },
        { env_int("FEC_K1", DEFAULT_K1), 1, {0} },
    };

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

    uint8_t in[2048], out[2048];

    for (;;) {
        ssize_t n = recvfrom(in_fd, in, sizeof in, 0, NULL, NULL);
        if (n < HARNESS_LEN) continue;

        uint32_t seq = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16
                     | (uint32_t)in[2] <<  8 | (uint32_t)in[3];
        const uint8_t *payload = in + 4;

        /* Forward the data packet immediately — never delay real media. */
        out[0] = 0x00;
        memcpy(out + 1, in, HARNESS_LEN);   /* seq32 + payload, verbatim */
        sendto(out_fd, out, 1 + HARNESS_LEN, 0,
               (struct sockaddr *)&relay, sizeof relay);

        /* Fold the payload into each layer's current group. */
        for (int L = 0; L < NUM_LAYERS; L++) {
            Layer *ly = &layers[L];
            if (seq < (uint32_t)ly->offset) continue;   /* layer not started */

            for (int b = 0; b < PAYLOAD_LEN; b++)
                ly->acc[b] ^= payload[b];

            /* Group closes on its last member; emit parity and reset. */
            int member = (int)((seq - (uint32_t)ly->offset) % (uint32_t)ly->k);
            if (member != ly->k - 1) continue;

            uint32_t base = seq - (uint32_t)(ly->k - 1);
            out[0] = 0x01;
            out[1] = (uint8_t)(base >> 24);
            out[2] = (uint8_t)(base >> 16);
            out[3] = (uint8_t)(base >>  8);
            out[4] = (uint8_t)(base);
            out[5] = (uint8_t)ly->k;
            out[6] = (uint8_t)L;
            out[7] = (uint8_t)ly->offset;
            memcpy(out + 8, ly->acc, PAYLOAD_LEN);
            sendto(out_fd, out, 8 + PAYLOAD_LEN, 0,
                   (struct sockaddr *)&relay, sizeof relay);

            memset(ly->acc, 0, PAYLOAD_LEN);
        }
    }
    return 0;
}
