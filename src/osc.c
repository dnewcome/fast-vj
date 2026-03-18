/*
 * osc.c — OSC UDP listener.
 *
 * Runs on a dedicated POSIX thread. Receives UDP datagrams, parses
 * them with tinyosc, and writes results into g_osc (atomic state).
 * The main/render thread reads g_osc each frame with atomic_exchange,
 * so commands are never lost even if they arrive mid-frame.
 */

#include "osc.h"
#include "tinyosc/tinyosc.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

OscState g_osc;

static pthread_t  s_thread;
static int        s_sock = -1;
static atomic_int s_running;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void osc_set_gain(float f) {
    int bits;
    __builtin_memcpy(&bits, &f, sizeof(bits));
    atomic_store_explicit(&g_osc.gain_bits, bits, memory_order_relaxed);
}

static void store_gain(float f) { osc_set_gain(f); }

static void dispatch(tosc_message *msg) {
    const char *addr = tosc_getAddress(msg);
    const char *fmt  = tosc_getFormat(msg);

    if (strcmp(addr, "/vj/audio") == 0 && fmt[0] == 'i') {
        int idx = tosc_getNextInt32(msg);
        atomic_store_explicit(&g_osc.pending_audio, idx, memory_order_release);
        printf("osc: /vj/audio %d\n", idx);

    } else if (strcmp(addr, "/vj/image") == 0 && fmt[0] == 'i') {
        int idx = tosc_getNextInt32(msg);
        atomic_store_explicit(&g_osc.pending_image, idx, memory_order_release);
        printf("osc: /vj/image %d\n", idx);

    } else if (strcmp(addr, "/vj/gain") == 0 && fmt[0] == 'f') {
        float g = tosc_getNextFloat(msg);
        store_gain(g);
        printf("osc: /vj/gain %.3f\n", g);

    } else if (strcmp(addr, "/vj/video") == 0 && fmt[0] == 'i') {
        int idx = tosc_getNextInt32(msg);
        atomic_store_explicit(&g_osc.pending_video, idx, memory_order_release);
        printf("osc: /vj/video %d\n", idx);

    } else if (strcmp(addr, "/vj/stop") == 0) {
        atomic_store_explicit(&g_osc.stop_audio, 1, memory_order_release);
        printf("osc: /vj/stop\n");

    } else {
        printf("osc: unhandled %s %s\n", addr, fmt);
    }
}

/* ------------------------------------------------------------------ */
/* Listener thread                                                     */
/* ------------------------------------------------------------------ */

static void *listener(void *arg) {
    (void)arg;
    char buf[4096];

    while (atomic_load(&s_running)) {
        int len = recv(s_sock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) continue;   /* timeout (SO_RCVTIMEO) or error */

        if (tosc_isBundle(buf)) {
            /* walk bundle — dispatch each contained message */
            tosc_bundle bundle;
            tosc_parseBundle(&bundle, buf, len);
            tosc_message msg;
            while (tosc_getNextMessage(&bundle, &msg))
                dispatch(&msg);
        } else {
            tosc_message msg;
            if (tosc_parseMessage(&msg, buf, len) == 0)
                dispatch(&msg);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void osc_init(int port) {
    /* Initial state: no pending commands, gain = 1.0 */
    atomic_store(&g_osc.pending_audio, -1);
    atomic_store(&g_osc.pending_image, -1);
    atomic_store(&g_osc.pending_video, -1);
    atomic_store(&g_osc.stop_audio,     0);
    store_gain(1.0f);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) { perror("osc: socket"); return; }

    /* 100ms receive timeout so the thread can check s_running */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("osc: bind");
        close(s_sock);
        s_sock = -1;
        return;
    }

    atomic_store(&s_running, 1);
    pthread_create(&s_thread, NULL, listener, NULL);
    printf("osc: listening on UDP port %d\n", port);
}

void osc_shutdown(void) {
    atomic_store(&s_running, 0);
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    pthread_join(s_thread, NULL);
}
