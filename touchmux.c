// touchmux-raw.c : Android 14-ready raw input passthrough
// Build: aarch64-linux-android34-clang touchmux-raw.c -static -o touchmux
// Usage: touchmux --src /dev/input/eventX [--grab=1] [--verbose=1] [--sx=1.0 --sy=1.0]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int verbose = 0;

static void die(const char* msg){ perror(msg); exit(1); }

static void emit(int fd, unsigned short type, unsigned short code, int value){
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    clock_gettime(CLOCK_MONOTONIC, &ev.time);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
        die("write(uinput)");
}

static inline int clampi(int v, int lo, int hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float scale_x = 1.0f, scale_y = 1.0f;
static int max_x = 1080, max_y = 2400;   // updated from source ABS if available

static void forward_event(int uifd, struct input_event* ev){
    int value = ev->value;

    // Optional scaling on absolute coordinates (legacy & MT)
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) {
            value = (int)((double)value * (double)scale_x);
            value = clampi(value, 0, max_x);
        } else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) {
            value = (int)((double)value * (double)scale_y);
            value = clampi(value, 0, max_y);
        }
    }

    emit(uifd, ev->type, ev->code, value);
}

static inline int test_bit(const unsigned long *bits, int bit){
    return bits[bit / (int)(8*sizeof(unsigned long))] &
           (1UL << (bit % (int)(8*sizeof(unsigned long))));
}

static inline void set_evbit(int fd, int t){
    if (ioctl(fd, UI_SET_EVBIT, t) < 0 && verbose)
        perror("UI_SET_EVBIT");
}

static inline void set_codebit(int fd, int ev_t, int code){
    int req = -1;
    switch (ev_t) {
        case EV_KEY: req = UI_SET_KEYBIT; break;
        case EV_ABS: req = UI_SET_ABSBIT; break;
        case EV_REL: req = UI_SET_RELBIT; break;
        case EV_MSC: req = UI_SET_MSCBIT; break;
        case EV_SW:  req = UI_SET_SWBIT;  break;
        case EV_LED: req = UI_SET_LEDBIT; break;
        case EV_SND: req = UI_SET_SNDBIT; break;
        case EV_FF:  req = UI_SET_FFBIT;  break;
        default: break;
    }
    if (req != -1) ioctl(fd, req, code);
}

// Try to copy ABS range into uinput_user_dev arrays
static void copy_abs_range(int src, struct uinput_user_dev* uidev, int code){
    struct input_absinfo ai;
    if (ioctl(src, EVIOCGABS(code), &ai) == 0) {
        uidev->absmin[code]  = ai.minimum;
        uidev->absmax[code]  = ai.maximum;
        uidev->absfuzz[code] = ai.fuzz;
        uidev->absflat[code] = ai.flat;
        // (ai.resolution is not part of uinput_user_dev arrays on older kernels)
        if (code == ABS_X || code == ABS_MT_POSITION_X) max_x = ai.maximum;
        if (code == ABS_Y || code == ABS_MT_POSITION_Y) max_y = ai.maximum;
    }
}

// Create a uinput device mirroring src capabilities (Android 14 friendly)
static int setup_uinput_from_src(int src){
    int ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ui < 0) ui = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (ui < 0) die("open(/dev/uinput)");

    // Get event type bitmask
    unsigned long evbit[(EV_MAX + 1 + (8*sizeof(unsigned long)-1)) / (8*sizeof(unsigned long))];
    memset(evbit, 0, sizeof(evbit));
    if (ioctl(src, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
        die("EVIOCGBIT(ev types)");

    // Enable all event types (we’ll add codes per type)
    for (int t = 0; t <= EV_MAX; t++) {
        if (!test_bit(evbit, t)) continue;
        set_evbit(ui, t);

        // Query per-type codes
        int max_code = 0;
        switch (t) {
            case EV_KEY: max_code = KEY_MAX; break;
            case EV_REL: max_code = REL_MAX; break;
            case EV_ABS: max_code = ABS_MAX; break;
            case EV_MSC: max_code = MSC_MAX; break;
            case EV_SW:  max_code = SW_MAX;  break;
            case EV_LED: max_code = LED_MAX; break;
            case EV_SND: max_code = SND_MAX; break;
            case EV_FF:  max_code = FF_MAX;  break;
            default:     max_code = 0;       break;
        }
        if (max_code <= 0) continue;

        unsigned long codebits[(/*max*/KEY_MAX + 1 + (8*sizeof(unsigned long)-1)) / (8*sizeof(unsigned long))];
        memset(codebits, 0, sizeof(codebits));
        if (ioctl(src, EVIOCGBIT(t, sizeof(codebits)), codebits) < 0) continue;

        for (int c = 0; c <= max_code; c++) {
            if (!test_bit(codebits, c)) continue;
            set_codebit(ui, t, c);
        }
    }

    // Mark as a direct-touch display so Android InputReader treats it as touchscreen.
    ioctl(ui, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    // Prepare device descriptor
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "touchmux-virtual");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x18D1;  // placeholder
    uidev.id.product = 0x4EE1;
    uidev.id.version = 1;

    // Copy ABS ranges for all ABS codes we can read
    for (int code = 0; code <= ABS_MAX; code++) {
        copy_abs_range(src, &uidev, code);
    }

    // Ensure essential MT and legacy ABS have sane defaults if source lacked them
    if (uidev.absmax[ABS_MT_POSITION_X] == 0) {
        uidev.absmin[ABS_MT_POSITION_X] = 0;
        uidev.absmax[ABS_MT_POSITION_X] = max_x;
    }
    if (uidev.absmax[ABS_MT_POSITION_Y] == 0) {
        uidev.absmin[ABS_MT_POSITION_Y] = 0;
        uidev.absmax[ABS_MT_POSITION_Y] = max_y;
    }
    if (uidev.absmax[ABS_X] == 0) {
        uidev.absmin[ABS_X] = 0;
        uidev.absmax[ABS_X] = max_x;
    }
    if (uidev.absmax[ABS_Y] == 0) {
        uidev.absmin[ABS_Y] = 0;
        uidev.absmax[ABS_Y] = max_y;
    }
    // Provide MT slot range (<=10 fingers is common on Android)
    if (uidev.absmax[ABS_MT_SLOT] == 0) {
        uidev.absmin[ABS_MT_SLOT] = 0;
        uidev.absmax[ABS_MT_SLOT] = 9;
    }

    // Write descriptor & create device
    if (write(ui, &uidev, sizeof(uidev)) < 0) die("write(uidev)");
    if (ioctl(ui, UI_DEV_CREATE) < 0) die("UI_DEV_CREATE");

    usleep(200 * 1000);
    return ui;
}

int main(int argc, char** argv){
    const char* srcpath = NULL;
    int grab = 0;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--src", 5)) {
            if (i + 1 < argc) srcpath = argv[++i];
        } else if (!strncmp(argv[i], "--grab=", 7)) {
            grab = atoi(argv[i] + 7);
        } else if (!strncmp(argv[i], "--verbose=", 10)) {
            verbose = atoi(argv[i] + 10);
        } else if (!strncmp(argv[i], "--sx=", 5)) {
            scale_x = atof(argv[i] + 5);
        } else if (!strncmp(argv[i], "--sy=", 5)) {
            scale_y = atof(argv[i] + 5);
        }
    }

    if (!srcpath) {
        fprintf(stderr, "usage: touchmux --src /dev/input/eventX [--grab=1] [--verbose=1] [--sx=1.0 --sy=1.0]\n");
        return 2;
    }

    int src = open(srcpath, O_RDONLY | O_NONBLOCK);
    if (src < 0) die("open(src)");

    if (grab) {
        if (ioctl(src, EVIOCGRAB, 1) < 0)
            perror("EVIOCGRAB failed (continuing)");
    }

    int ui = setup_uinput_from_src(src);

    if (verbose) fprintf(stderr, "touchmux: RAW forwarding from %s to virtual uinput\n", srcpath);

    for (;;) {
        struct input_event ev;
        ssize_t r = read(src, &ev, sizeof(ev));
        if (r == -1) {
            if (errno == EAGAIN) { usleep(1000); continue; }
            die("read(src)");
        }
        if (r != (ssize_t)sizeof(ev)) continue;

        // Forward ALL events, with optional X/Y scaling
        forward_event(ui, &ev);
    }

    return 0;
}
