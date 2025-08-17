// touchmux.c (simplified MIT-licensed example)
// Build: aarch64-linux-android20-clang touchmux.c -static -o touchmux
// Usage: touchmux --src /dev/input/eventX [--grab=1] [--verbose=1]

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

static int get_abs(int fd, int code, struct input_absinfo *ai) {
  memset(ai, 0, sizeof(*ai));
  ai->value = 0;
  if (ioctl(fd, EVIOCGABS(code), ai) < 0) return -1;
  return 0;
}

static int set_bit(int fd, int a, int b) { return ioctl(fd, UI_SET_ABSBIT, b); }
static int set_key(int fd, int k) { return ioctl(fd, UI_SET_KEYBIT, k); }
static int set_ev(int fd, int e) { return ioctl(fd, UI_SET_EVBIT, e); }

static void die(const char* msg) { perror(msg); exit(1); }

static void emit(int fd, unsigned short type, unsigned short code, int value){
  struct input_event ev = {0};
  clock_gettime(CLOCK_MONOTONIC, &ev.time);
  ev.type = type; ev.code = code; ev.value = value;
  if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) die("write(uinput)");
}

static void sync_report(int fd){ emit(fd, EV_SYN, SYN_REPORT, 0); }

static int setup_uinput_from_src(int src, int* max_x, int* max_y){
    int ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ui < 0) die("open(/dev/uinput)");

    // Event types
    set_ev(ui, EV_KEY);
    set_ev(ui, EV_ABS);
    set_ev(ui, EV_SYN);

    // Keys
    set_key(ui, BTN_TOUCH);
    set_key(ui, BTN_TOOL_FINGER);

    // Touchscreen property
    ioctl(ui, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev uidev;
    memset(&uidev,0,sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "touchmux-virtual");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x18D1; // Google as placeholder
    uidev.id.product = 0x4EE1;
    uidev.id.version = 1;

    // Axis defaults
    *max_x = 1080; *max_y = 2400;
    uidev.absmin[ABS_X] = 0; uidev.absmax[ABS_X] = *max_x;
    uidev.absmin[ABS_Y] = 0; uidev.absmax[ABS_Y] = *max_y;
    uidev.absmin[ABS_MT_POSITION_X] = 0; uidev.absmax[ABS_MT_POSITION_X] = *max_x;
    uidev.absmin[ABS_MT_POSITION_Y] = 0; uidev.absmax[ABS_MT_POSITION_Y] = *max_y;
    uidev.absmin[ABS_MT_SLOT] = 0; uidev.absmax[ABS_MT_SLOT] = 9;
    uidev.absmin[ABS_MT_TRACKING_ID] = 0; uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

    // Write device + create
    if (write(ui, &uidev, sizeof(uidev)) < 0) die("write(uidev)");
    if (ioctl(ui, UI_DEV_CREATE) < 0) die("UI_DEV_CREATE");

    usleep(200 * 1000);
    return ui;
}

static inline int clamp(int v, int lo, int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }

// Example transform: simple linear scaling (1.0 = passthrough)
static float scale_x = 1.0f, scale_y = 1.0f;
static int max_x = 0, max_y = 0;

static void forward_event(int uifd, struct input_event* ev){
  int value = ev->value;

  // Optional transforms
  if (ev->type == EV_ABS) {
    if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) {
      value = (int)(value * scale_x);
      value = clamp(value, 0, max_x);
    } else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) {
      value = (int)(value * scale_y);
      value = clamp(value, 0, max_y);
    }
  }

  emit(uifd, ev->type, ev->code, value);
}

int main(int argc, char** argv){
  const char* srcpath = NULL;
  int grab = 0;

  for (int i=1;i<argc;i++){
    if (!strncmp(argv[i],"--src",5)) { srcpath = argv[++i]; }
    else if (!strncmp(argv[i],"--grab=",7)) { grab = atoi(argv[i]+7); }
    else if (!strncmp(argv[i],"--verbose=",10)) { verbose = atoi(argv[i]+10); }
    else if (!strncmp(argv[i],"--sx=",5)) { scale_x = atof(argv[i]+5); }
    else if (!strncmp(argv[i],"--sy=",5)) { scale_y = atof(argv[i]+5); }
  }
  if (!srcpath) { fprintf(stderr,"usage: touchmux --src /dev/input/eventX [--grab=1] [--sx=1.0 --sy=1.0]\n"); return 2; }

  int src = open(srcpath, O_RDONLY | O_NONBLOCK);
  if (src < 0) die("open(src)");

  if (grab) {
    if (ioctl(src, EVIOCGRAB, 1) < 0) perror("EVIOCGRAB failed (continuing)");
  }

  int ui = setup_uinput_from_src(src, &max_x, &max_y);

  if (verbose) fprintf(stderr,"touchmux: forwarding from %s to /dev/uinput (max %d x %d)\n", srcpath, max_x, max_y);

  for (;;) {
    struct input_event ev;
    ssize_t r = read(src, &ev, sizeof(ev));
    if (r == -1) {
      if (errno == EAGAIN) { usleep(1000); continue; }
      die("read(src)");
    }
    if (r != sizeof(ev)) continue;

    // Forward all relevant types (ABS/KEY/SYN)
    if (ev.type == EV_ABS || ev.type == EV_KEY || ev.type == EV_SYN) {
      forward_event(ui, &ev);
    }
  }

  // not reached
  return 0;
}
