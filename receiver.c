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

#define NUM_LAYERS 2

/* Parity store, per layer, indexed by the group's base seq. Each parity is
 * the XOR of the k payloads at { base .. base+k-1 }. */
static uint8_t parity_payload[NUM_LAYERS][MAX_FRAMES][PAYLOAD_LEN];
static int     parity_present[NUM_LAYERS][MAX_FRAMES];

/* Per-layer geometry, learned from parity headers (sender-chosen). */
static int layer_k[NUM_LAYERS];
static int layer_off[NUM_LAYERS];

static void on_frame_available(uint32_t seq, int n_frames);

/* Record a frame (harness format: seq32 + payload) and forward it to the
 * player immediately. `present` doubles as the dedup flag: the player keeps
 * only a frame's first arrival, so re-sending a known frame is pure waste.
 * Marking present before recursing also terminates the recovery cascade. */
static void store(uint32_t seq, const uint8_t *frame, int n_frames) {
    if (seq >= (uint32_t)n_frames || slots[seq].present) return;

    memcpy(slots[seq].data, frame, FRAME_LEN);
    slots[seq].present = 1;

    sendto(g_out_fd, slots[seq].data, FRAME_LEN, 0,
           (struct sockaddr *)&g_player, sizeof g_player);

    on_frame_available(seq, n_frames);
}

/* Base seq of layer L's group owning `seq`: groups are disjoint runs of
 * layer_k[L] consecutive frames starting at layer_off[L]. */
static uint32_t group_base(uint32_t seq, int L) {
    uint32_t off = (uint32_t)layer_off[L];
    uint32_t k   = (uint32_t)layer_k[L];
    return off + ((seq - off) / k) * k;
}

/* Forward declaration: reconstruction feeds store(), which retries the
 * other layer, which can reconstruct again — a short recovery cascade. */
static void try_reconstruct(uint32_t base, int L, int n_frames);

/* If layer L's group has its parity and exactly one missing member, rebuild
 * that member as parity XOR (all surviving members). */
static void try_reconstruct(uint32_t base, int L, int n_frames) {
    if (layer_k[L] <= 0) return;
    if (base >= (uint32_t)n_frames || !parity_present[L][base]) return;

    const int k = layer_k[L];
    int      n_missing   = 0;
    uint32_t missing_seq = 0;

    for (int m = 0; m < k; m++) {
        uint32_t s = base + (uint32_t)m;
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
    memcpy(rebuilt + 4, parity_payload[L][base], PAYLOAD_LEN);

    for (int m = 0; m < k; m++) {
        uint32_t s = base + (uint32_t)m;
        if (s >= (uint32_t)n_frames || s == missing_seq) continue;
        const uint8_t *p = slots[s].data + 4;
        for (int b = 0; b < PAYLOAD_LEN; b++) rebuilt[4 + b] ^= p[b];
    }

    /* store() forwards the recovered frame to the player right away. */
    store(missing_seq, rebuilt, n_frames);
}

/* A frame just became available (received or reconstructed). It may be the
 * last hole in either layer's group, so retry both. */
static void on_frame_available(uint32_t seq, int n_frames) {
    for (int L = 0; L < NUM_LAYERS; L++) {
        if (layer_k[L] <= 0) continue;
        if (seq < (uint32_t)layer_off[L]) continue;
        try_reconstruct(group_base(seq, L), L, n_frames);
    }
}

/* Process one packet from the relay.
 *
 * Wire format from sender:
 *   DATA   : [0x00][seq:4 BE][payload:160]
 *   PARITY : [0x01][base:4 BE][k:1][layer:1][off:1][xor_payload:160]
 */
static void handle(const uint8_t *pkt, ssize_t n, int n_frames) {
    if (n < 1) return;

    if (pkt[0] == 0x00 && n >= 1 + FRAME_LEN) {
        /* DATA: bytes after the type byte are the harness frame verbatim.
         * store() forwards it and retries both layers. */
        uint32_t seq = (uint32_t)pkt[1] << 24 | (uint32_t)pkt[2] << 16
                     | (uint32_t)pkt[3] <<  8 | (uint32_t)pkt[4];
        store(seq, pkt + 1, n_frames);

    } else if (pkt[0] == 0x01 && n >= 8 + PAYLOAD_LEN) {
        uint32_t base = (uint32_t)pkt[1] << 24 | (uint32_t)pkt[2] << 16
                      | (uint32_t)pkt[3] <<  8 | (uint32_t)pkt[4];
        int k   = pkt[5];
        int L   = pkt[6];
        int off = pkt[7];
        if (k <= 0 || L < 0 || L >= NUM_LAYERS) return;

        layer_k[L]   = k;
        layer_off[L] = off;

        if (base < (uint32_t)n_frames && !parity_present[L][base]) {
            memcpy(parity_payload[L][base], pkt + 8, PAYLOAD_LEN);
            parity_present[L][base] = 1;
        }
        try_reconstruct(base, L, n_frames);
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
