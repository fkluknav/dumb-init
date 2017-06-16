/*
 * dumb-init is a simple wrapper program designed to run as PID 1 and pass
 * signals to its children.
 *
 * Usage:
 *   ./dumb-init python -c 'while True: pass'
 *
 * To get debug output on stderr, run with '-v'.
 */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include "VERSION.h"

#define PRINTERR(...) do { \
    fprintf(stderr, "[dumb-init] " __VA_ARGS__); \
} while (0)

#define DEBUG(...) do { \
    if (debug) { \
        PRINTERR(__VA_ARGS__); \
    } \
} while (0)

// Signals we care about are numbered from 1 to 31, inclusive.
// (32 and above are real-time signals.)
// TODO: this is likely not portable outside of Linux, or on strange architectures
// EDIT: we actually care about some real-time signals as well
// SIGRTMAX is not a constant, use 64
#define MAXSIG 64

// Indices are one-indexed (signal 1 is at index 1). Index zero is unused.
int signal_rewrite[MAXSIG + 1] = {[0 ... MAXSIG] = -1};
char *signal_action[MAXSIG + 1] = {[0 ... MAXSIG] = ""};

pid_t child_pid = -1;
char debug = 0;
char use_setsid = 1;
static char survive_bereaving = 0;

int translate_signal(int signum) {
    if (signum <= 0 || signum > MAXSIG) {
        return signum;
    } else {
        int translated = signal_rewrite[signum];
        if (translated == -1) {
            return signum;
        } else {
            DEBUG("Translating signal %d to %d.\n", signum, translated);
            return translated;
        }
    }
}

void do_action(int signum) {
    DEBUG("Action for signal %d: running %s\n", signum, signal_action[signum]);
    int child_pid = fork();
    if (child_pid < 0) {
        PRINTERR("Unable to fork. Exiting.\n");
        exit(1);
    } else if (child_pid == 0) {
        /* child */
        sigset_t all_signals;
        sigfillset(&all_signals);
        sigprocmask(SIG_UNBLOCK, &all_signals, NULL);
        execlp("/bin/bash", "/bin/bash", "-c", signal_action[signum], (char *)NULL);

        // if this point is reached, exec failed, so we should exit nonzero
        PRINTERR("Could not exec %s: %s\n", signal_action[signum], strerror(errno));
        exit(1);
    }
}

void forward_signal(int signum) {
    int new_signum = translate_signal(signum);
    if (new_signum == -2) {
        do_action(signum);
    } else if (new_signum != -1) {
        kill(use_setsid ? -child_pid : child_pid, new_signum);
        DEBUG("Forwarded signal %d to children.\n", new_signum);
    } else {
        DEBUG("Not forwarding signal %d to children (ignored).\n", new_signum);
    }
}

/*
 * Read /proc and see if there are processes except init(PIDs)
 */
signed int process_count() {
    DIR *dp;
    struct dirent *ep;
    char nonnumber;
    signed int count = 0;

    dp = opendir ("/proc");
    if (dp != NULL)
    {
        while ((ep = readdir (dp)) != NULL) {
            nonnumber = 0;
            for (int i = 0; ep->d_name[i] != 0; ++i) {
                if (!isdigit(ep->d_name[i])) {
                    nonnumber = 1;
                    break;
                }
            }
            if (!nonnumber) {
                DEBUG("/proc/%s is a process\n", ep->d_name);
                ++count;
                if (count > 1) {
                    closedir(dp);
                    return 2; //2 is enough, do not count further
                }
            }
        }
        closedir(dp);
    } else {
        PRINTERR("Could not open /proc.\n");
        return -1;
    }
    return count;
}

/*
 * The dumb-init signal handler.
 *
 * The main job of this signal handler is to forward signals along to our child
 * process(es). In setsid mode, this means signaling the entire process group
 * rooted at our child. In non-setsid mode, this is just signaling the primary
 * child.
 *
 * In most cases, simply proxying the received signal is sufficient. If we
 * receive a job control signal, however, we should not only forward it, but
 * also sleep dumb-init itself.
 *
 * This allows users to run foreground processes using dumb-init and to
 * control them using normal shell job control features (e.g. Ctrl-Z to
 * generate a SIGTSTP and suspend the process).
 *
 * The libc manual is useful:
 * https://www.gnu.org/software/libc/manual/html_node/Job-Control-Signals.html
 *
*/
void handle_signal(int signum) {
    static char bereaved = 0;
    DEBUG("Received signal %d.\n", signum);
    if (signum == SIGCHLD) {
        int status, exit_status;
        pid_t killed_pid;
        while ((killed_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (WIFEXITED(status)) {
                exit_status = WEXITSTATUS(status);
                DEBUG("A child with PID %d exited with exit status %d.\n", killed_pid, exit_status);
            } else {
                assert(WIFSIGNALED(status));
                exit_status = 128 + WTERMSIG(status);
                DEBUG("A child with PID %d was terminated by signal %d.\n", killed_pid, exit_status - 128);
            }

            if (killed_pid == child_pid) {
                bereaved = 1;
                if (!survive_bereaving) {
                    forward_signal(SIGTERM);  // send SIGTERM to any remaining children
                    DEBUG("Child exited with status %d. Goodbye.\n", exit_status);
                    exit(exit_status);
                } else {
                    DEBUG("Child exited with status %d. Stay alive for your grandchildren.\n", exit_status);
                }
            }
        }
         
        if ((bereaved == 1) && survive_bereaving) {
            signed int pc = process_count();
            DEBUG("Process count: %d\n", pc);
            if (pc <= 1) {
                DEBUG("No process left, exitting.\n");
                exit(0);
            }
        }

    } else {
        forward_signal(signum);
        if (signum == SIGTSTP || signum == SIGTTOU || signum == SIGTTIN) {
            DEBUG("Suspending self due to TTY signal.\n");
            kill(getpid(), SIGSTOP);
        }
    }
}

void print_help(char *argv[]) {
    fprintf(stderr,
        "dumb-init v%s"
        "Usage: %s [option] command [[arg] ...]\n"
        "\n"
        "dumb-init is a simple process supervisor that forwards signals to children.\n"
        "It is designed to run as PID1 in minimal container environments.\n"
        "\n"
        "Optional arguments:\n"
        "   -c, --single-child      Run in single-child mode.\n"
        "                           In this mode, signals are only proxied to the\n"
        "                           direct child and not any of its descendants.\n"
        "   -b, --survive-bereaving Do not quit when the direct child dies.\n"
        "   -r, --rewrite s:r       Rewrite received signal s to new signal r before proxying.\n"
        "                           To ignore (not proxy) a signal, rewrite it to 0.\n"
        "                           To rewrite all signals, rewrite (otherwise nonexistent) signal 0.\n"
        "                           (Useful to ignore all signals, use '--rewrite 0:0').\n"
        "                           This option can be specified multiple times.\n"
        "   -a, --action s:exe      Run exe after receiving sinal s.\n"
        "                           For example, -a '2:echo hi there'.\n"
        "                           This option can be specified multiple times.\n"
        "   -v, --verbose           Print debugging information to stderr.\n"
        "   -h, --help              Print this help message and exit.\n"
        "   -V, --version           Print the current version and exit.\n"
        "\n"
        "Full help is available online at https://github.com/Yelp/dumb-init\n",
        VERSION,
        argv[0]
    );
}

void print_rewrite_signum_help() {
    fprintf(
        stderr,
        "Usage: -r option takes <signum>:<signum>, where <signum> "
        "is between 0 and %d.\n"
        "This option can be specified multiple times.\n"
        "Use --help for full usage.\n",
        MAXSIG
    );
    exit(1);
}

void print_action_help() {
    fprintf(
        stderr,
        "Usage: -a option takes <signum>:<path>, where <signum> "
        "is between 1 and %d.\n"
        "This option can be specified multiple times.\n"
        "Use --help for full usage.\n",
        MAXSIG
    );

    exit(1);
}

void parse_rewrite_signum(char *arg) {
    int signum, replacement;
    if (
        sscanf(arg, "%d:%d", &signum, &replacement) == 2 &&
        (signum >= 0 && signum <= MAXSIG) &&
        (replacement >= 0 && replacement <= MAXSIG)
    ) {
        if (signum == 0) {
            for (int i = 0; i <= MAXSIG; ++i) {
                signal_rewrite[i] = replacement;
            }
        } else {
            signal_rewrite[signum] = replacement;
        }
    } else {
        print_rewrite_signum_help();
    }
}

void parse_action(char *arg) {
    int signum;
    int status;
    int position;
    if (
        (status = sscanf(arg, "%d:%n", &signum, &position)) == 1 &&
        (signum >= 0 && signum <= MAXSIG)
    ) {
        DEBUG("signal action: %d, position %d\n", signum, position);
        signal_action[signum] = &(arg[position]);
        signal_rewrite[signum] = -2;
    } else {
        print_action_help();
    }
}
void set_rewrite_to_sigstop_if_not_defined(int signum) {
    if (signal_rewrite[signum] == -1)
        signal_rewrite[signum] = SIGSTOP;
}

char **parse_command(int argc, char *argv[]) {
    int opt;
    struct option long_options[] = {
        {"help",             no_argument,       NULL, 'h'},
        {"single-child",     no_argument,       NULL, 'c'},
        {"rewrite",          required_argument, NULL, 'r'},
        {"verbose",          no_argument,       NULL, 'v'},
        {"version",          no_argument,       NULL, 'V'},
        {"survive-bereaving",no_argument,       NULL, 'b'},
        {"action",           required_argument, NULL, 'a'},
        {NULL,                     0,       NULL,   0},
    };
    while ((opt = getopt_long(argc, argv, "+hvVcbr:a:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_help(argv);
                exit(0);
            case 'v':
                debug = 1;
                break;
            case 'V':
                fprintf(stderr, "dumb-init v%s", VERSION);
                exit(0);
            case 'c':
                use_setsid = 0;
                break;
            case 'r':
                parse_rewrite_signum(optarg);
                break;
            case 'a':
                parse_action(optarg);
                break;
            case 'b':
                survive_bereaving = 1;
                break;
            default:
                PRINTERR("Error while parsing arguments.\n");
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(
            stderr,
            "Usage: %s [option] program [args]\n"
            "Try %s --help for full usage.\n",
            argv[0], argv[0]
        );
        exit(1);
    }

    char *debug_env = getenv("DUMB_INIT_DEBUG");
    if (debug_env && strcmp(debug_env, "1") == 0) {
        debug = 1;
        DEBUG("Running in debug mode.\n");
    }

    char *setsid_env = getenv("DUMB_INIT_SETSID");
    if (setsid_env && strcmp(setsid_env, "0") == 0) {
        use_setsid = 0;
        DEBUG("Not running in setsid mode.\n");
    }

    if (use_setsid) {
        set_rewrite_to_sigstop_if_not_defined(SIGTSTP);
        set_rewrite_to_sigstop_if_not_defined(SIGTTOU);
        set_rewrite_to_sigstop_if_not_defined(SIGTTIN);
    }

    return &argv[optind];
}

// A dummy signal handler used for signals we care about.
// On the FreeBSD kernel, ignored signals cannot be waited on by `sigwait` (but
// they can be on Linux). We must provide a dummy handler.
// https://lists.freebsd.org/pipermail/freebsd-ports/2009-October/057340.html
void dummy(int signum) {}

int main(int argc, char *argv[]) {
    char **cmd = parse_command(argc, argv);
    sigset_t all_signals;
    sigfillset(&all_signals);
    sigprocmask(SIG_BLOCK, &all_signals, NULL);

    int i = 0;
    for (i = 1; i <= MAXSIG; i++)
        signal(i, dummy);

    /* detach dumb-init from controlling tty */
    if (use_setsid && ioctl(STDIN_FILENO, TIOCNOTTY) == -1) {
        DEBUG(
            "Unable to detach from controlling tty (errno=%d %s).\n",
            errno,
            strerror(errno)
        );
    }

    child_pid = fork();
    if (child_pid < 0) {
        PRINTERR("Unable to fork. Exiting.\n");
        return 1;
    } else if (child_pid == 0) {
        /* child */
        sigprocmask(SIG_UNBLOCK, &all_signals, NULL);
        if (use_setsid) {
            if (setsid() == -1) {
                PRINTERR(
                    "Unable to setsid (errno=%d %s). Exiting.\n",
                    errno,
                    strerror(errno)
                );
                exit(1);
            }

            if (ioctl(STDIN_FILENO, TIOCSCTTY, 0) == -1) {
                DEBUG(
                    "Unable to attach to controlling tty (errno=%d %s).\n",
                    errno,
                    strerror(errno)
                );
            }
            DEBUG("setsid complete.\n");
        }
        execvp(cmd[0], &cmd[0]);

        // if this point is reached, exec failed, so we should exit nonzero
        PRINTERR("%s: %s\n", cmd[0], strerror(errno));
        return 2;
    } else {
        /* parent */
        DEBUG("Child spawned with PID %d.\n", child_pid);
        for (;;) {
            struct timespec timeout = {1, 0};
            int signum = sigtimedwait(&all_signals, NULL, &timeout);
            if (signum == -1) {
                switch (errno) {
                    case EINVAL: 
                        PRINTERR("Invalid timeout, report this as a bug!\n");
                        exit(1);
                    case EINTR:
                        PRINTERR("Wait interrupted by a signal. This should never happen. Report this as a bug!\n");
                        exit(1);
                    case EAGAIN:
                        //pretend timeout to be SIGCHLD, check if we want to continue running
                        signum = SIGCHLD;
                        DEBUG("Heartbeat...\n");
                }
            }
            handle_signal(signum);
        }
    }
}
