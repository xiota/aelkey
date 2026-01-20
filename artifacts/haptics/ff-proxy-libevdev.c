// gcc -o ff-proxy-libevdev ff-proxy-libevdev.c $(pkgconf --cflags --libs libevdev)

// Simple force‑feedback proxy: mirrors FF uploads/plays from a virtual uinput device to a real
// device. Usage: ./ff-proxy-libevdev /dev/input/eventX

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#define MAX_EFFECTS 128
static int real_ids[MAX_EFFECTS];

static int
upload_effect_to_real(int real_fd, const struct ff_effect *src, int existing_real_id) {
  struct ff_effect eff;
  memset(&eff, 0, sizeof(eff));

  // Forward the type directly
  eff.type = src->type;

  // Reuse existing real effect if present
  eff.id = (existing_real_id >= 0) ? existing_real_id : -1;

  // Copy the entire effect struct
  eff.replay = src->replay;
  eff.direction = src->direction;

  switch (src->type) {
    case FF_RUMBLE:
      eff.u.rumble = src->u.rumble;
      break;

    case FF_PERIODIC:
      eff.u.periodic = src->u.periodic;
      break;

    case FF_SINE:
    case FF_TRIANGLE:
    case FF_SQUARE:
      eff.u.periodic = src->u.periodic;
      break;

    default:
      // Unsupported → fallback to rumble
      eff.type = FF_RUMBLE;
      eff.u.rumble.strong_magnitude = 0x4000;
      eff.u.rumble.weak_magnitude = 0x4000;
      break;
  }

  if (ioctl(real_fd, EVIOCSFF, &eff) < 0) {
    perror("EVIOCSFF (real device)");
    return -1;
  }

  return eff.id;
}

static void play_effect_on_real(int real_fd, int id, int repeat) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));

  ev.type = EV_FF;
  ev.code = id;
  ev.value = repeat;

  if (write(real_fd, &ev, sizeof(ev)) < 0) {
    perror("write EV_FF to real device");
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
    return 1;
  }

  const char *real_path = argv[1];

  int real_fd = open(real_path, O_RDWR | O_NONBLOCK);
  if (real_fd < 0) {
    perror("open real device");
    return 1;
  }

  struct libevdev *real_dev = NULL;
  int rc = libevdev_new_from_fd(real_fd, &real_dev);
  if (rc < 0) {
    fprintf(stderr, "libevdev_new_from_fd failed: %s\n", strerror(-rc));
    close(real_fd);
    return 1;
  }

  const char *oldname = libevdev_get_name(real_dev);
  char newname[256];
  snprintf(newname, sizeof(newname), "%s (Proxy)", oldname);
  libevdev_set_name(real_dev, newname);

  printf("Real device: %s\n", libevdev_get_name(real_dev));

  struct libevdev_uinput *uidev = NULL;
  rc = libevdev_uinput_create_from_device(real_dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
  if (rc < 0) {
    fprintf(stderr, "libevdev_uinput_create_from_device failed: %s\n", strerror(-rc));
    libevdev_free(real_dev);
    close(real_fd);
    return 1;
  }

  const char *virt_node = libevdev_uinput_get_devnode(uidev);
  int ufd = libevdev_uinput_get_fd(uidev);

  printf("Virtual FF proxy device created at: %s\n", virt_node);
  printf("Use ff-test-libevdev against this virtual device.\n");

  for (int i = 0; i < MAX_EFFECTS; i++) {
    real_ids[i] = -1;
  }

  // Main loop
  for (;;) {
    struct input_event ev;
    ssize_t n = read(ufd, &ev, sizeof(ev));

    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        usleep(1000);
        continue;
      }
      perror("read uinput fd");
      break;
    }
    if (n != sizeof(ev)) {
      continue;
    }

    if (ev.type == EV_UINPUT) {
      // -----------------------------
      // UPLOAD
      // -----------------------------
      if (ev.code == UI_FF_UPLOAD) {
        struct uinput_ff_upload up;
        memset(&up, 0, sizeof(up));
        up.request_id = ev.value;

        if (ioctl(ufd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
          perror("UI_BEGIN_FF_UPLOAD");
          continue;
        }

        struct ff_effect *eff = &up.effect;
        int virt_id = eff->id;

        if (virt_id < 0 || virt_id >= MAX_EFFECTS) {
          fprintf(stderr, "FF_UPLOAD: virt_id %d out of range\n", virt_id);
        }

        int existing_real_id = (virt_id >= 0 && virt_id < MAX_EFFECTS) ? real_ids[virt_id] : -1;

        printf(
            "FF_UPLOAD: type=%u virt_id=%d existing_real_id=%d\n",
            eff->type,
            virt_id,
            existing_real_id
        );

        int real_id = upload_effect_to_real(real_fd, eff, existing_real_id);

        if (real_id < 0) {
          up.retval = -1;
        } else {
          up.retval = 0;
          if (virt_id >= 0 && virt_id < MAX_EFFECTS) {
            real_ids[virt_id] = real_id;
          }

          up.effect.id = virt_id;  // virtual device keeps its own ID
        }

        if (ioctl(ufd, UI_END_FF_UPLOAD, &up) < 0) {
          perror("UI_END_FF_UPLOAD");
        }
      }

      // -----------------------------
      // ERASE
      // -----------------------------
      else if (ev.code == UI_FF_ERASE) {
        struct uinput_ff_erase er;
        memset(&er, 0, sizeof(er));
        er.request_id = ev.value;

        if (ioctl(ufd, UI_BEGIN_FF_ERASE, &er) < 0) {
          perror("UI_BEGIN_FF_ERASE");
          continue;
        }

        int virt_id = er.effect_id;
        int real_id = (virt_id >= 0 && virt_id < MAX_EFFECTS) ? real_ids[virt_id] : -1;

        printf("FF_ERASE: virt_id=%d real_id=%d\n", virt_id, real_id);

        if (real_id >= 0) {
          if (ioctl(real_fd, EVIOCRMFF, real_id) < 0) {
            perror("EVIOCRMFF (real device)");
            er.retval = -1;
          } else {
            er.retval = 0;
            real_ids[virt_id] = -1;
          }
        } else {
          er.retval = 0;
        }

        if (ioctl(ufd, UI_END_FF_ERASE, &er) < 0) {
          perror("UI_END_FF_ERASE");
        }
      }
    }

    // -----------------------------
    // PLAY / STOP
    // -----------------------------
    else if (ev.type == EV_FF) {
      int virt_id = ev.code;
      int real_id = (virt_id >= 0 && virt_id < MAX_EFFECTS) ? real_ids[virt_id] : -1;

      printf("EV_FF play: virt_id=%d real_id=%d value=%d\n", virt_id, real_id, ev.value);

      if (real_id >= 0) {
        play_effect_on_real(real_fd, real_id, ev.value);
      }
    }
  }

  libevdev_uinput_destroy(uidev);
  libevdev_free(real_dev);
  close(real_fd);
  return 0;
}
