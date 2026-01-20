// gcc -o ff-test-libevdev ff-test-libevdev.c $(pkgconf --cflags --libs libevdev)

// Simple force-feedback test using libevdev + evdev ioctls
// Usage: ./ff-test-libevdev /dev/input/eventX

#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void play_effect(int fd, int id, int duration_ms) {
  struct input_event play = { .type = EV_FF, .code = id, .value = 1 };
  write(fd, &play, sizeof(play));
  usleep(duration_ms * 1000);

  struct input_event stop = { .type = EV_FF, .code = id, .value = 0 };
  write(fd, &stop, sizeof(stop));
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
    return 1;
  }

  const char *devnode = argv[1];
  int fd = open(devnode, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  struct libevdev *dev = NULL;
  int rc = libevdev_new_from_fd(fd, &dev);
  if (rc < 0) {
    fprintf(stderr, "libevdev init failed: %s\n", strerror(-rc));
    return 1;
  }

  printf("Opened device: %s\n", libevdev_get_name(dev));

  if (!libevdev_has_event_type(dev, EV_FF) || !libevdev_has_event_code(dev, EV_FF, FF_RUMBLE)) {
    fprintf(stderr, "Device does not support FF_RUMBLE\n");
    return 1;
  }

  printf("Device supports FF_RUMBLE\n");

  // -----------------------------
  // Create and play effects
  // -----------------------------
  struct ff_effect dyn = { .type = FF_RUMBLE,
                           .id = -1,
                           .u.rumble.strong_magnitude = 0x0000,
                           .u.rumble.weak_magnitude = 0x0000,
                           .replay.length = 250,
                           .replay.delay = 0 };

  // Magnitudes to cycle through
  static const __u16 mags[] = {
    0x0250, 0x0500, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000,
    0x8000, 0x9000, 0xa000, 0xb000, 0xc000, 0xd000, 0xe000, 0xf000, 0xffff,
    0xf000, 0xe000, 0xd000, 0xc000, 0xb000, 0xa000, 0x9000, 0x8000, 0x7000,
    0x6000, 0x5000, 0x4000, 0x2000, 0x1000, 0x0500, 0x0250,
  };
  const int count = sizeof(mags) / sizeof(mags[0]);

  while (1) {
    // --- Weak-only cycle ---
    for (int i = 0; i < count; i++) {
      dyn.u.rumble.strong_magnitude = 0;
      dyn.u.rumble.weak_magnitude = mags[i];

      if (ioctl(fd, EVIOCSFF, &dyn) < 0) {
        perror("reupload weak");
        break;
      }

      printf("Weak rumble: 0x%04x\n", mags[i]);
      play_effect(fd, dyn.id, 250);
    }

    // --- Strong-only cycle ---
    for (int i = 0; i < count; i++) {
      dyn.u.rumble.strong_magnitude = mags[i];
      dyn.u.rumble.weak_magnitude = 0;

      if (ioctl(fd, EVIOCSFF, &dyn) < 0) {
        perror("reupload strong");
        break;
      }

      printf("Strong rumble: 0x%04x\n", mags[i]);
      play_effect(fd, dyn.id, 250);
    }
  }

  // -----------------------------
  // Cleanup
  // -----------------------------
  ioctl(fd, EVIOCRMFF, dyn.id);

  libevdev_free(dev);
  close(fd);

  printf("Done.\n");
  return 0;
}
