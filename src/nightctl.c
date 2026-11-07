/*
 * nightctl - control tool for the nightpatch kernel module.
 *
 * The module does all the work: it watches for nightreign.exe, scans and
 * decodes its image, and arms the hardware breakpoints on its own.  This tool
 * only flips the switch and reads the result back, so every command returns
 * immediately - there is no daemon to keep running.
 *
 *   nightctl enable <fps>   start watching (survives this process exiting)
 *   nightctl disable        stop watching and unpatch
 *   nightctl fps <n>        change the frame rate live, without re-arming
 *   nightctl status         show what the module is doing
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nightpatch/np_ioctl.h"

static const char* const DEVICE_PATH = "/dev/nightpatch";

static const char* const site_names[NP_SITE_COUNT] = {
    [NP_SITE_FRAMETICK] = "frametick",
    [NP_SITE_HERTZ] = "hertz",
    [NP_SITE_INTRO_BNE] = "intro/bne",
    [NP_SITE_INTRO_FROMSOFT] = "intro/fromsoft",
};

static const char* watcher_state_name(uint32_t state)
{
    switch (state)
    {
    case NP_WATCHER_DISABLED:
        return "disabled";
    case NP_WATCHER_SEARCHING:
        return "searching for the game";
    case NP_WATCHER_ARMED:
        return "armed";
    case NP_WATCHER_FAILED:
        return "game found but not patchable";
    default:
        return "unknown";
    }
}

/*
 * What a located site is currently doing.  "retired" is a success, not a
 * failure: the intro sites fire once during boot and hand their debug register
 * straight back, so seeing one unarmed with a hit is exactly right.
 */
static const char* site_disposition(const struct np_site_status* site)
{
    if (site->armed)
    {
        return "armed";
    }
    return site->retired ? "retired" : "not armed";
}

static int open_device(void)
{
    int fd = open(DEVICE_PATH, O_RDWR);

    if (fd < 0)
    {
        fprintf(stderr, "error: cannot open %s: %s\n", DEVICE_PATH, strerror(errno));
        if (errno == ENOENT)
        {
            fprintf(stderr, "hint: is nightpatch.ko loaded?  try: sudo insmod build/kmod/nightpatch.ko\n");
        }
        else if (errno == EACCES)
        {
            fprintf(stderr, "hint: the device is root-only; run nightctl with sudo\n");
        }
    }
    return fd;
}

static int run_ioctl(int fd, unsigned long request, void* argument, const char* what)
{
    if (ioctl(fd, request, argument) < 0)
    {
        fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

/* Parse a frame rate argument.  The module clamps the value, so anything
 * numeric is accepted here and only outright garbage is rejected. */
static int parse_fps(const char* text, int32_t* out)
{
    char* end = NULL;
    long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX)
    {
        fprintf(stderr, "error: '%s' is not a frame rate\n", text);
        return -1;
    }
    *out = (int32_t)value;
    return 0;
}

/* Fetch a status snapshot, refusing one laid out for a different ABI. */
static int read_status(int fd, struct np_status_out* status)
{
    if (run_ioctl(fd, NP_IOC_STATUS, status, "status"))
    {
        return -1;
    }
    if (status->abi_version != NP_ABI_VERSION)
    {
        fprintf(stderr, "error: module speaks ABI %u, nightctl speaks %u - rebuild both\n", status->abi_version,
                NP_ABI_VERSION);
        return -1;
    }
    return 0;
}

/*
 * Print the frame time held in frame_time_bits.
 *
 * Userspace has an FPU, so this just reinterprets the bits the kernel built
 * with integer arithmetic - which doubles as a check that it built them right.
 */
static void print_frame_time(uint32_t frame_time_bits)
{
    float frame_time;

    memcpy(&frame_time, &frame_time_bits, sizeof(frame_time));
    printf("frame time : %.9f s (bits %#010x)\n", (double)frame_time, frame_time_bits);
}

static int command_enable(int fd, const char* fps_text)
{
    struct np_enable_req request = {0};

    if (parse_fps(fps_text, &request.target_fps))
    {
        return 1;
    }
    if (run_ioctl(fd, NP_IOC_ENABLE, &request, "enable"))
    {
        return 1;
    }
    printf("watching for nightreign.exe; the module patches every launch until 'nightctl disable'\n");
    return 0;
}

static int command_disable(int fd)
{
    if (run_ioctl(fd, NP_IOC_DISABLE, NULL, "disable"))
    {
        return 1;
    }
    printf("disabled; the game runs stock again\n");
    return 0;
}

static int command_fps(int fd, const char* fps_text)
{
    int32_t requested_fps;
    struct np_status_out status;

    if (parse_fps(fps_text, &requested_fps))
    {
        return 1;
    }
    if (run_ioctl(fd, NP_IOC_SET_FPS, &requested_fps, "set fps"))
    {
        return 1;
    }
    if (read_status(fd, &status))
    {
        return 1;
    }
    printf("frame rate now %d fps\n", status.target_fps);
    return 0;
}

static int command_status(int fd)
{
    struct np_status_out status;
    int i;

    if (read_status(fd, &status))
    {
        return 1;
    }

    printf("state      : %s\n", watcher_state_name(status.watcher_state));
    printf("target fps : %d\n", status.target_fps);
    print_frame_time(status.frame_time_bits);

    if (status.game_pid)
    {
        printf("game       : pid %d, image %#llx + %#llx\n", status.game_pid,
               (unsigned long long)status.image_base, (unsigned long long)status.image_size);
    }
    else
    {
        printf("game       : not attached\n");
    }

    for (i = 0; i < NP_SITE_COUNT; i++)
    {
        const struct np_site_status* site = &status.sites[i];

        printf("  %-14s: ", site_names[i]);
        if (site->found)
        {
            printf("%#llx (image+%#llx)%s %s", (unsigned long long)site->address,
                   (unsigned long long)(site->address - status.image_base), site->fuzzy ? " [fuzzy]" : "",
                   site_disposition(site));
        }
        else
        {
            printf("not located");
        }
        printf("  hits %llu  faults %llu\n", (unsigned long long)site->hits, (unsigned long long)site->faults);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: nightctl <command> [args]\n"
                    "  enable <fps>   watch for the game and patch every launch\n"
                    "  disable        stop watching and restore stock behaviour\n"
                    "  fps <n>        change the frame rate live\n"
                    "  status         show module state, sites and hit counters\n");
}

int main(int argc, char** argv)
{
    const char* command;
    int fd;
    int ret;

    if (argc < 2)
    {
        usage();
        return 1;
    }
    command = argv[1];

    if ((!strcmp(command, "enable") || !strcmp(command, "fps")) && argc < 3)
    {
        usage();
        return 1;
    }

    fd = open_device();
    if (fd < 0)
    {
        return 1;
    }

    if (!strcmp(command, "enable"))
    {
        ret = command_enable(fd, argv[2]);
    }
    else if (!strcmp(command, "disable"))
    {
        ret = command_disable(fd);
    }
    else if (!strcmp(command, "fps"))
    {
        ret = command_fps(fd, argv[2]);
    }
    else if (!strcmp(command, "status"))
    {
        ret = command_status(fd);
    }
    else
    {
        usage();
        ret = 1;
    }

    close(fd);
    return ret;
}
