/* RECEIVER — jitter buffer, single-threaded, select()-driven playout clock.
 *
 * Ports:
 *   bind 47002  <- media from sender via relay
 *   send 47020  -> harness player (4-byte big-endian seq + 160-byte payload)
 *
 * Design: drain all pending packets non-blocking, then sleep with ONE
 * blocking select() call until the playout deadline, then drain again and
 * send. Sending 10 ms before the harness deadline absorbs OS scheduling
 * jitter (relay max jitter is 40/80 ms; 10 ms headroom still leaves ≥10 ms
 * for late packets to arrive in the buffer).
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAMES  8192
#define PAYLOAD_LEN 160
#define FRAME_LEN   (4 + PAYLOAD_LEN)
#define FRAME_MS    20
#define SEND_EARLY_MS 10.0   /* send this many ms before harness deadline */

typedef struct {
    uint8_t data[FRAME_LEN];
    int     present;
} Slot;

static Slot slots[MAX_FRAMES];

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Store a decoded frame (harness format: seq32 + payload) into its slot. */
static void store(uint32_t seq, const uint8_t *frame, int n_frames) {
    if (seq < (uint32_t)n_frames && !slots[seq].present) {
        memcpy(slots[seq].data, frame, FRAME_LEN);
        slots[seq].present = 1;
    }
}

/* Read all currently-available packets into the buffer; never blocks.
 *
 * Wire format from sender:
 *   DATA   : [0x00][seq:4 BE][payload:160]
 *   PARITY : [0x01][base:4 BE][k:1][stride:1][xor_payload:160]  (Step 4)
 */
static void drain(int fd, int n_frames) {
    for (;;) {
        struct timeval zero = {0, 0};
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &zero) <= 0) break;

        uint8_t pkt[2048];
        ssize_t n = recvfrom(fd, pkt, sizeof pkt, 0, NULL, NULL);
        if (n < 1) continue;

        if (pkt[0] == 0x00 && n >= 1 + FRAME_LEN) {
            /* DATA: bytes after the type byte are the harness frame verbatim */
            uint32_t seq = (uint32_t)pkt[1] << 24 | (uint32_t)pkt[2] << 16
                         | (uint32_t)pkt[3] <<  8 | (uint32_t)pkt[4];
            store(seq, pkt + 1, n_frames);
        }
        /* PARITY (0x01) handled in Step 4 — ignored for now. */
    }
}

int main(void) {
    const char *t0_env  = getenv("T0");
    const char *dly_env = getenv("DELAY_MS");
    const char *dur_env = getenv("DURATION_S");

    double g_t0       = t0_env  ? atof(t0_env)  : now_s();
    double g_delay_ms = dly_env ? atof(dly_env) : 100.0;
    double dur        = dur_env ? atof(dur_env) : 30.0;
    int    n_frames   = (int)(dur * 1000.0 / FRAME_MS) + 64;
    if (n_frames > MAX_FRAMES) n_frames = MAX_FRAMES;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002"); return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family      = AF_INET;
    player.sin_port        = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int next_play = 0;

    for (;;) {
        if (next_play >= n_frames) break;

        /* Harness deadline: t0 + delay_ms + i*20ms.
         * We aim to send SEND_EARLY_MS before that so loopback + jitter
         * never push us past the deadline. */
        double send_at = g_t0 + g_delay_ms / 1000.0
                       + next_play * FRAME_MS / 1000.0
                       - SEND_EARLY_MS / 1000.0;

        /* Sleep until send_at, reading any arriving packets along the way. */
        double wait;
        while ((wait = send_at - now_s()) > 0.0) {
            /* Non-blocking drain first — pick up any already-queued packets. */
            drain(in_fd, n_frames);

            /* Recheck in case draining took time. */
            wait = send_at - now_s();
            if (wait <= 0.0) break;

            /* Block until a packet arrives OR the deadline arrives. */
            struct timeval tv;
            tv.tv_sec  = (long)wait;
            tv.tv_usec = (long)((wait - (long)wait) * 1e6);
            if (tv.tv_usec < 0) tv.tv_usec = 0;

            fd_set fds; FD_ZERO(&fds); FD_SET(in_fd, &fds);
            select(in_fd + 1, &fds, NULL, NULL, &tv);
            /* Whether packet arrived or timeout fired, drain in next iteration */
        }

        /* Final drain to catch any last-instant arrivals. */
        drain(in_fd, n_frames);

        if (slots[next_play].present) {
            sendto(out_fd, slots[next_play].data, FRAME_LEN, 0,
                   (struct sockaddr *)&player, sizeof player);
        }
        next_play++;
    }
    return 0;
}
