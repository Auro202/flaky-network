/* RECEIVER — reorder/dedup buffer + XOR FEC recovery, zero-latency output.
 *
 * Ports:
 *   bind 47002  <- media from sender via relay
 *   send 47020  -> harness player (4-byte big-endian seq + 160-byte payload)
 *
 * Design: the harness player records each frame's FIRST arrival and checks
 * it against a deadline — that is a LATEST bound, not a schedule. Delivering
 * a frame early is never penalised, and the receiver->player leg does not
 * cross the relay, so it costs nothing against the overhead cap.
 *
 * Therefore we do NOT pace output. Every frame is forwarded the instant it
 * exists: on arrival, or the moment FEC reconstructs it. An earlier version
 * paced sends to just before each deadline and lost frames whenever the OS
 * scheduler woke us late — pure downside, since holding a frame we already
 * have can only reduce its chance of beating the deadline.
 *
 * The buffer's only remaining job is to retain payloads for XOR recovery
 * and to suppress duplicate forwards.
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

typedef struct {
    uint8_t data[FRAME_LEN];
    int     present;
} Slot;

static Slot slots[MAX_FRAMES];

/* Player socket + address: frames are emitted the moment they materialise. */
static int                g_out_fd;
static struct sockaddr_in g_player;

/* Parity store, indexed by the group's base seq. Each parity is the XOR of
 * the K payloads at { base + m*stride : 0 <= m < K }. */
static uint8_t parity_payload[MAX_FRAMES][PAYLOAD_LEN];
static int     parity_present[MAX_FRAMES];

/* FEC params are learned from the first parity packet (sender-chosen). */
static int learned_k = 0, learned_stride = 0;

/* Record a frame (harness format: seq32 + payload) and forward it to the
 * player immediately. `present` doubles as the dedup flag: the player keeps
 * only a frame's first arrival, so re-sending a known frame is pure waste. */
static void store(uint32_t seq, const uint8_t *frame, int n_frames) {
    if (seq >= (uint32_t)n_frames || slots[seq].present) return;

    memcpy(slots[seq].data, frame, FRAME_LEN);
    slots[seq].present = 1;

    sendto(g_out_fd, slots[seq].data, FRAME_LEN, 0,
           (struct sockaddr *)&g_player, sizeof g_player);
}

/* Base seq of the parity group that owns `seq`, given the FEC geometry.
 * Frames are laid out in blocks of K*STRIDE; group j within a block owns
 * members { block + j + m*STRIDE : 0 <= m < K }. */
static uint32_t group_base(uint32_t seq, int k, int stride) {
    uint32_t block_len = (uint32_t)(k * stride);
    uint32_t block     = (seq / block_len) * block_len;
    uint32_t j         = (seq % block_len) % (uint32_t)stride;
    return block + j;
}

/* If this group has its parity and exactly one missing data frame, rebuild
 * the missing frame as parity XOR (all surviving members). */
static void try_reconstruct(uint32_t base, int n_frames) {
    if (base >= (uint32_t)n_frames || !parity_present[base]) return;
    if (learned_k <= 0) return;

    int      n_missing   = 0;
    uint32_t missing_seq = 0;

    for (int m = 0; m < learned_k; m++) {
        uint32_t s = base + (uint32_t)(m * learned_stride);
        if (s >= (uint32_t)n_frames) continue;   /* group truncated at stream end */
        if (!slots[s].present) {
            n_missing++;
            missing_seq = s;
            if (n_missing > 1) return;           /* unrecoverable: 2+ gone */
        }
    }
    if (n_missing != 1) return;                  /* nothing to do */

    /* missing payload = parity XOR every surviving member's payload */
    uint8_t rebuilt[FRAME_LEN];
    rebuilt[0] = (uint8_t)(missing_seq >> 24);
    rebuilt[1] = (uint8_t)(missing_seq >> 16);
    rebuilt[2] = (uint8_t)(missing_seq >>  8);
    rebuilt[3] = (uint8_t)(missing_seq);
    memcpy(rebuilt + 4, parity_payload[base], PAYLOAD_LEN);

    for (int m = 0; m < learned_k; m++) {
        uint32_t s = base + (uint32_t)(m * learned_stride);
        if (s >= (uint32_t)n_frames || s == missing_seq) continue;
        const uint8_t *p = slots[s].data + 4;
        for (int b = 0; b < PAYLOAD_LEN; b++) rebuilt[4 + b] ^= p[b];
    }

    /* store() forwards the recovered frame to the player right away. */
    store(missing_seq, rebuilt, n_frames);
}

/* Process one packet from the relay.
 *
 * Wire format from sender:
 *   DATA   : [0x00][seq:4 BE][payload:160]
 *   PARITY : [0x01][base:4 BE][k:1][stride:1][xor_payload:160]
 */
static void handle(const uint8_t *pkt, ssize_t n, int n_frames) {
        if (n < 1) return;

        if (pkt[0] == 0x00 && n >= 1 + FRAME_LEN) {
            /* DATA: bytes after the type byte are the harness frame verbatim */
            uint32_t seq = (uint32_t)pkt[1] << 24 | (uint32_t)pkt[2] << 16
                         | (uint32_t)pkt[3] <<  8 | (uint32_t)pkt[4];
            store(seq, pkt + 1, n_frames);

            /* A late member can be the one that leaves its group with a
             * single hole — retry that group. */
            if (learned_k > 0 && seq < (uint32_t)n_frames)
                try_reconstruct(group_base(seq, learned_k, learned_stride),
                                n_frames);

        } else if (pkt[0] == 0x01 && n >= 7 + PAYLOAD_LEN) {
            uint32_t base = (uint32_t)pkt[1] << 24 | (uint32_t)pkt[2] << 16
                          | (uint32_t)pkt[3] <<  8 | (uint32_t)pkt[4];
            int k      = pkt[5];
            int stride = pkt[6];
            if (k <= 0 || stride <= 0) return;

            learned_k      = k;
            learned_stride = stride;

            if (base < (uint32_t)n_frames && !parity_present[base]) {
                memcpy(parity_payload[base], pkt + 7, PAYLOAD_LEN);
                parity_present[base] = 1;
            }
            try_reconstruct(base, n_frames);
        }
}

int main(void) {
    const char *dur_env = getenv("DURATION_S");
    double dur      = dur_env ? atof(dur_env) : 30.0;
    int    n_frames = (int)(dur * 1000.0 / FRAME_MS) + 64;
    if (n_frames > MAX_FRAMES) n_frames = MAX_FRAMES;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002"); return 1;
    }

    g_out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&g_player, 0, sizeof g_player);
    g_player.sin_family      = AF_INET;
    g_player.sin_port        = htons(47020);
    g_player.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* No playout clock: forward on arrival. The harness kills us at run end. */
    uint8_t pkt[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, pkt, sizeof pkt, 0, NULL, NULL);
        if (n <= 0) continue;
        handle(pkt, n, n_frames);
    }
    return 0;
}
