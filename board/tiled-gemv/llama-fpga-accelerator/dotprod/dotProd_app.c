#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

#define CMD_START_ACC      (1U << 27)
#define CMD_CHECK_STATUS   (2U << 27)
#define CMD_RESET_ACC      (3U << 27)
#define STATUS_DONE        0x2U

static volatile sig_atomic_t det_int = 0;
static volatile sig_atomic_t timed_out = 0;

/* Keep the signal handler small; read the device from main(). */
static void sighandler(int signo)
{
    if (signo == SIGIO)
        det_int = 1;
    else if (signo == SIGALRM)
        timed_out = 1;
}

int main(int argc, char *argv[])
{
    unsigned long num_elements = 32, status;
    unsigned char inputs[68] = {0}; /* One 34-byte X block, then one Y block. */
    struct { uint32_t result; uint32_t status; } output;
    struct sigaction action = {0};
    sigset_t blocked, oldmask, waitmask;
    ssize_t transferred;
    float result;
    int fd, flags;

    if (argc != 1) {
        fprintf(stderr, "Usage: %s\n", argv[0]);
        return EXIT_FAILURE;
    }

    sigemptyset(&action.sa_mask);
    action.sa_handler = sighandler;
    if (sigaction(SIGIO, &action, NULL) || sigaction(SIGALRM, &action, NULL))
        err(1, "Installing signal handlers");

    /* Block before starting: a fast IRQ stays pending until sigsuspend(). */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGIO);
    sigaddset(&blocked, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &blocked, &oldmask))
        err(1, "Blocking signals");
    waitmask = oldmask;
    sigdelset(&waitmask, SIGIO);
    sigdelset(&waitmask, SIGALRM);

    fd = open("/dev/fpga", O_RDWR);
    if (fd < 0)
        err(1, "Opening /dev/fpga (load fpga_drv.ko first)");
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETOWN, getpid()) < 0 ||
        fcntl(fd, F_SETFL, flags | O_ASYNC) < 0)
        err(1, "Enabling SIGIO");

    /* FP16 scale 1.0 = 0x3c00, stored little-endian in each block. */
    inputs[1] = 0x3c;
    inputs[35] = 0x3c;
    memset(inputs + 2, 1, 32);  /* X = [1, ..., 1]. */
    memset(inputs + 36, 2, 32); /* Y = [2, ..., 2]. */

    if (ioctl(fd, CMD_RESET_ACC, 0) < 0)
        err(1, "Clearing previous completion");
    transferred = write(fd, inputs, sizeof(inputs));
    if (transferred < 0)
        err(1, "Uploading X and Y");
    if (transferred != (ssize_t)sizeof(inputs))
        errx(1, "Short input write: %zd bytes", transferred);

    if (ioctl(fd, CMD_START_ACC, &num_elements) < 0)
        err(1, "Starting dotProd");

    alarm(30);
    while (!det_int && !timed_out)
        sigsuspend(&waitmask);
    alarm(0);
    if (!det_int)
        errx(1, "Timed out after 30 seconds waiting for SIGIO");

    /* The driver preserves completion status after acknowledging the IRQ. */
    if (ioctl(fd, CMD_CHECK_STATUS, &status) < 0)
        err(1, "Checking completion status");
    transferred = read(fd, &output, sizeof(output));
    if (transferred < 0)
        err(1, "Reading result/status");
    if (transferred != (ssize_t)sizeof(output))
        errx(1, "Short completion read: %zd bytes", transferred);

    memcpy(&result, &output.result, sizeof(result));
    if (status != STATUS_DONE || output.status != STATUS_DONE || result != 64.0f)
        errx(1, "FAIL: ioctl status=%#lx, read status=%#x, result=%g (expected 64)",
             status, (unsigned int)output.status, (double)result);

    printf("PASS: interrupt received; dot([1 x 32], [2 x 32]) = %.1f\n",
           (double)result);
    if (close(fd))
        err(1, "Closing device");
    if (sigprocmask(SIG_SETMASK, &oldmask, NULL))
        err(1, "Restoring signal mask");
    return EXIT_SUCCESS;
}

