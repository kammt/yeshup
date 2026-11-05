/*
 * yeshup - run a command that WILL get hung up.
 *
 * The counterpart to nohup(1):
 *   - SIGHUP is never ignored: the command starts with SIGHUP at its default
 *     disposition and unblocked, and any SIGHUP sent to yeshup is passed
 *     through to the command.
 *   - At a random time between 2 and 10 minutes after start, yeshup sends
 *     SIGHUP to the command itself.
 *
 * yeshup exits with the command's exit status. If the command is killed by a
 * signal, yeshup terminates itself with the same signal.
 *
 * Portable to Linux and macOS (POSIX).
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef YESHUP_VERSION
#define YESHUP_VERSION "1.0.0"
#endif

/* Bounds of the self-inflicted hangup, in milliseconds. Overridable at build
 * time (used by the test suite). */
#ifndef YESHUP_MIN_MS
#define YESHUP_MIN_MS (2u * 60u * 1000u)
#endif
#ifndef YESHUP_MAX_MS
#define YESHUP_MAX_MS (10u * 60u * 1000u)
#endif

/* Exit codes, matching nohup(1). */
#define EXIT_INTERNAL 125
#define EXIT_CANNOT_INVOKE 126
#define EXIT_ENOENT 127

static const char *progname = "yeshup";

static volatile sig_atomic_t got_hup;
static volatile sig_atomic_t got_term;
static volatile sig_atomic_t got_alarm;
static volatile sig_atomic_t got_chld;

static void on_signal(int sig)
{
    switch (sig) {
    case SIGHUP:  got_hup = 1;   break;
    case SIGTERM: got_term = 1;  break;
    case SIGALRM: got_alarm = 1; break;
    case SIGCHLD: got_chld = 1;  break;
    }
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: %s COMMAND [ARG]...\n"
            "  or:  %s --help | --version\n"
            "\n"
            "Run COMMAND, making sure it gets hung up.\n"
            "\n"
            "  - SIGHUP is never ignored and is always passed through to COMMAND.\n"
            "  - At a random time between 2 and 10 minutes, %s sends SIGHUP\n"
            "    to COMMAND itself.\n"
            "\n"
            "Exit status is that of COMMAND. If COMMAND is killed by a signal,\n"
            "%s dies of the same signal. Otherwise:\n"
            "  125  if %s itself fails\n"
            "  126  if COMMAND is found but cannot be invoked\n"
            "  127  if COMMAND cannot be found\n",
            progname, progname, progname, progname, progname);
}

/* Uniformly random integer in [lo, hi]. */
static uint32_t random_between(uint32_t lo, uint32_t hi)
{
    uint32_t range = hi - lo + 1; /* hi >= lo; range 0 means full 2^32 */
    uint32_t limit, r;
    int fd = open("/dev/urandom", O_RDONLY);

    if (range == 0)
        range = UINT32_MAX;
    /* Largest multiple of range, to avoid modulo bias. */
    limit = UINT32_MAX - (UINT32_MAX % range);

    for (;;) {
        if (fd < 0 || read(fd, &r, sizeof r) != (ssize_t)sizeof r) {
            /* Fallback: not cryptographic, but good enough for this. */
            static int seeded;
            if (!seeded) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                srandom((unsigned)(tv.tv_sec ^ tv.tv_usec ^ (getpid() << 16)));
                seeded = 1;
            }
            r = ((uint32_t)random() << 16) ^ (uint32_t)random();
        }
        if (r < limit)
            break;
    }
    if (fd >= 0)
        close(fd);
    return lo + r % range;
}

/* Die of the given signal, like the child did, so our parent sees the same
 * termination status. Falls back to the shell convention 128+sig. */
static void die_of_signal(int sig)
{
    struct sigaction sa;
    sigset_t set;
    struct rlimit nocore = { 0, 0 };

    /* Don't dump a core of yeshup just because the child dumped one. */
    setrlimit(RLIMIT_CORE, &nocore);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);

    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);

    kill(getpid(), sig);
    _exit(128 + sig);
}

int main(int argc, char **argv)
{
    /* Signals yeshup handles itself (and must restore in the child). */
    static const int handled[] = { SIGHUP, SIGTERM, SIGALRM, SIGCHLD };
    /* Signals yeshup ignores while the child runs: the terminal delivers
     * them to the whole foreground process group already. */
    static const int ignored[] = { SIGINT, SIGQUIT };
    enum { NHANDLED = sizeof handled / sizeof handled[0],
           NIGNORED = sizeof ignored / sizeof ignored[0] };

    struct sigaction old_handled[NHANDLED], old_ignored[NIGNORED];
    struct sigaction sa;
    sigset_t block, orig_mask, wait_mask;
    struct itimerval timer;
    uint32_t delay_ms;
    pid_t child;
    int status, i;

    if (argc > 0 && argv[0] && argv[0][0]) {
        const char *slash = strrchr(argv[0], '/');
        progname = slash ? slash + 1 : argv[0];
    }

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", progname, YESHUP_VERSION);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--") == 0) {
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "%s: missing operand\n", progname);
        usage(stderr);
        return EXIT_INTERNAL;
    }

    /* Block our signals until we're ready to wait for them, so nothing slips
     * through between fork() and sigsuspend(). */
    sigemptyset(&block);
    for (i = 0; i < NHANDLED; i++)
        sigaddset(&block, handled[i]);
    if (sigprocmask(SIG_BLOCK, &block, &orig_mask) < 0) {
        fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
        return EXIT_INTERNAL;
    }

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    for (i = 0; i < NHANDLED; i++) {
        if (sigaction(handled[i], &sa, &old_handled[i]) < 0) {
            fprintf(stderr, "%s: sigaction: %s\n", progname, strerror(errno));
            return EXIT_INTERNAL;
        }
    }
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    for (i = 0; i < NIGNORED; i++) {
        if (sigaction(ignored[i], &sa, &old_ignored[i]) < 0) {
            fprintf(stderr, "%s: sigaction: %s\n", progname, strerror(errno));
            return EXIT_INTERNAL;
        }
    }

    child = fork();
    if (child < 0) {
        fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
        return EXIT_INTERNAL;
    }

    if (child == 0) {
        sigset_t child_mask = orig_mask;
        int err;

        /* Restore whatever dispositions we inherited... */
        for (i = 0; i < NHANDLED; i++)
            sigaction(handled[i], &old_handled[i], NULL);
        for (i = 0; i < NIGNORED; i++)
            sigaction(ignored[i], &old_ignored[i], NULL);

        /* ...except SIGHUP, which is the whole point: undo any nohup. */
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGHUP, &sa, NULL);
        sigdelset(&child_mask, SIGHUP);
        sigprocmask(SIG_SETMASK, &child_mask, NULL);

        execvp(argv[1], &argv[1]);
        err = errno;
        fprintf(stderr, "%s: failed to run command '%s': %s\n",
                progname, argv[1], strerror(err));
        _exit(err == ENOENT ? EXIT_ENOENT : EXIT_CANNOT_INVOKE);
    }

    /* Parent: arm the random hangup. */
    delay_ms = random_between(YESHUP_MIN_MS, YESHUP_MAX_MS);
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_sec = delay_ms / 1000;
    timer.it_value.tv_usec = (delay_ms % 1000) * 1000;
    if (setitimer(ITIMER_REAL, &timer, NULL) < 0) {
        fprintf(stderr, "%s: setitimer: %s\n", progname, strerror(errno));
        kill(child, SIGHUP); /* Can't wait, so hang up right away. */
    }

    wait_mask = orig_mask;
    for (i = 0; i < NHANDLED; i++)
        sigdelset(&wait_mask, handled[i]);

    for (;;) {
        pid_t r;

        sigsuspend(&wait_mask);

        if (got_hup) {
            got_hup = 0;
            kill(child, SIGHUP);
        }
        if (got_alarm) {
            got_alarm = 0;
            kill(child, SIGHUP);
        }
        if (got_term) {
            got_term = 0;
            kill(child, SIGTERM);
        }
        if (got_chld) {
            got_chld = 0;
            r = waitpid(child, &status, WNOHANG);
            if (r == child && (WIFEXITED(status) || WIFSIGNALED(status)))
                break;
            if (r < 0 && errno != EINTR) {
                fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));
                return EXIT_INTERNAL;
            }
        }
    }

    if (WIFSIGNALED(status))
        die_of_signal(WTERMSIG(status));
    return WEXITSTATUS(status);
}
