// Copyright 2003-2022 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <memory>
#include <algorithm>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <nk/from_string.hpp>
extern "C" {
#include "nk/log.h"
#include "nk/io.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"

#define NCRON_VERSION "2.0"

int gflags_debug;
static volatile sig_atomic_t pending_save_and_exit;
static volatile sig_atomic_t pending_reload_config;
static bool gflags_background{false};
static std::optional<int> s6_notify_fd;

/* Time (in msec) to sleep before dispatching events at startup.
   Set to a nonzero value so as not to compete for cpu with init scripts at
   boot time. */
static unsigned g_initial_sleep = 0;

static std::string g_ncron_conf(CONFIG_FILE_DEFAULT);
static std::string g_ncron_execfile(EXEC_FILE_DEFAULT);
static int g_ncron_execmode = 0;

static std::vector<StackItem> stack;
static std::vector<StackItem> deadstack;

static void reload_config(void)
{
    if (g_ncron_execmode != 2)
        save_stack(g_ncron_execfile, stack, deadstack);

    stack.clear();
    deadstack.clear();
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);
    log_line("SIGHUP - Reloading config: %s.", g_ncron_conf.c_str());
    std::fflush(stdout);
    pending_reload_config = 0;
}

static void save_and_exit(void)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, stack, deadstack);
        log_line("Saving stack to %s.", g_ncron_execfile.c_str());
    }
    log_line("Exited.");
    exit(EXIT_SUCCESS);
}

static void signal_handler(int sig)
{
    int serrno = errno;
    if (sig == SIGTERM || sig == SIGINT) {
        pending_save_and_exit = 1;
    } else if (sig == SIGHUP) {
        pending_reload_config = 1;
    } else if (sig == SIGCHLD) {
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
    errno = serrno;
}

static void fix_signals(void)
{
    static const int ss[] = {
        SIGCHLD, SIGHUP, SIGINT, SIGTERM, SIGKILL
    };
    sigset_t mask;
    if (sigprocmask(0, 0, &mask) < 0)
        suicide("sigprocmask failed");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigdelset(&mask, ss[i]))
            suicide("sigdelset failed");
    if (sigaddset(&mask, SIGPIPE))
        suicide("sigaddset failed");
    if (sigprocmask(SIG_SETMASK, &mask, (sigset_t *)0) < 0)
        suicide("sigprocmask failed");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART|SA_NOCLDWAIT;
    if (sigemptyset(&sa.sa_mask))
        suicide("sigemptyset failed");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigaction(ss[i], &sa, NULL))
            suicide("sigaction failed");
}

static void fail_on_fdne(std::string_view file, int mode)
{
    if (access(file.data(), mode))
        exit(EXIT_FAILURE);
}

static void sleep_or_die(struct timespec *ts, bool pending_save)
{
    struct timespec rem;
sleep:
    if (nanosleep(ts, &rem)) {
        switch (errno) {
            case EINTR:
                if (pending_save_and_exit)
                    save_and_exit();

                if (g_ncron_execmode == 1 || pending_save)
                    save_stack(g_ncron_execfile, stack, deadstack);

                if (pending_reload_config)
                    reload_config();

                memcpy(ts, &rem, sizeof(struct timespec));
                goto sleep;
            default:
                log_line("%s: nanosleep failed: %s", __func__, strerror(errno));
                std::exit(EXIT_FAILURE);
        }
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        log_line("%s: clock_gettime failed: %s", __func__, strerror(errno));
        std::exit(EXIT_FAILURE);
    }
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    log_line("do_work: ts.tv_sec = %lu  stack.front().exectime = %lu", ts.tv_sec, stack.front().exectime);
    for (const auto &i: stack)
        log_line("do_work: job %u exectime = %lu", i.ce->id, i.ce->exectime);
}

static void do_work(unsigned initial_sleep)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec=initial_sleep * 1000000;

    sleep_or_die(&ts, false);

    while (1) {
        bool pending_save = false;

        clock_or_die(&ts);

        while (stack.front().exectime <= ts.tv_sec) {
            auto &i = *stack.front().ce;
            if (gflags_debug)
                log_line("do_work: DISPATCH");

            i.exec(ts);
            stack.front().exectime = i.exectime;
            if (i.journal)
                pending_save = true;

            if ((i.numruns < i.maxruns || i.maxruns == 0)
                && i.exectime != 0) {
                std::make_heap(stack.begin(), stack.end(), GtCronEntry);
            } else {
                std::pop_heap(stack.begin(), stack.end(), GtCronEntry);
                deadstack.emplace_back(std::move(stack.back()));
                stack.pop_back();
            }
            if (stack.empty())
                save_and_exit();

            clock_or_die(&ts);
        }

        debug_stack_print(ts);
        if (ts.tv_sec <= stack.front().exectime) {
            struct timespec sts;
            sts.tv_sec = stack.front().exectime - ts.tv_sec;
            sts.tv_nsec = 0;
            if (gflags_debug)
                log_line("do_work: SLEEP %zu seconds", sts.tv_sec);
            sleep_or_die(&sts, pending_save);
        }
    }
}

static void usage()
{
    printf("ncron " NCRON_VERSION ", cron/at daemon.\n"
           "Copyright 2003-2022 Nicholas J. Kain\n"
           "Usage: ncron [options]...\n\nOptions:\n"
           "--help         -h    Print usage and exit.\n"
           "--version      -v    Print version and exit.\n"
           "--background   -b    Run as a background daemon.\n"
           "--sleep        -s [] Initial sleep time in seconds.\n"
           "--noexecsave   -0    Don't save execution history at all.\n"
           "--journal      -j    Save exectimes at each job invocation.\n"
           "--crontab      -t [] Path to crontab file.\n"
           "--history      -H [] Path to execution history file.\n"
           "--verbose      -V    Log diagnostic information.\n"
    );
}

static void print_version()
{
    log_line("ncron " NCRON_VERSION ", cron/at daemon.\n"
             "Copyright 2003-2022 Nicholas J. Kain\n\n"
"Permission is hereby granted, free of charge, to any person obtaining\n"
"a copy of this software and associated documentation files (the\n"
"\"Software\"), to deal in the Software without restriction, including\n"
"without limitation the rights to use, copy, modify, merge, publish,\n"
"distribute, sublicense, and/or sell copies of the Software, and to\n"
"permit persons to whom the Software is furnished to do so, subject to\n"
"the following conditions:\n\n"
"The above copyright notice and this permission notice shall be\n"
"included in all copies or substantial portions of the Software.\n\n"
"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n"
"EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF\n"
"MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND\n"
"NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE\n"
"LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION\n"
"OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION\n"
"WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
             );
}

static void process_options(int ac, char *av[])
{
    static struct option long_options[] = {
        {"help", 0, (int *)0, 'h'},
        {"version", 0, (int *)0, 'v'},
        {"background", 0, (int *)0, 'b'},
        {"sleep", 1, (int *)0, 's'},
        {"noexecsave", 0, (int *)0, '0'},
        {"journal", 0, (int *)0, 'j'},
        {"crontab", 1, (int *)0, 't'},
        {"history", 1, (int *)0, 'H'},
        {"s6-notify", 1, (int *)0, 'd'},
        {"verbose", 0, (int *)0, 'V'},
        {(const char *)0, 0, (int *)0, 0 }
    };
    for (;;) {
        auto c = getopt_long(ac, av, "hvbs:0jt:H:d:V", long_options, (int *)0);
        if (c == -1) break;
        switch (c) {
            case 'h': usage(); std::exit(EXIT_SUCCESS); break;
            case 'v': print_version(); std::exit(EXIT_SUCCESS); break;
            case 'b': gflags_background = true; break;
            case 's': if (auto t = nk::from_string<unsigned>(optarg)) g_initial_sleep = *t; else {
                          log_line("invalid sleep '%s' specified", optarg);
                          std::exit(EXIT_FAILURE);
                      }
                      break;
            case '0': g_ncron_execmode = 2; break;
            case 'j': g_ncron_execmode = 1; break;
            case 't': g_ncron_conf = optarg; break;
            case 'H': g_ncron_execfile = optarg; break;
            case 'd': s6_notify_fd = atoi(optarg); break;
            case 'V': gflags_debug = 1; break;
            default: break;
        }
    }
}

int main(int argc, char* argv[])
{
    process_options(argc, argv);
    fail_on_fdne(g_ncron_conf, R_OK);
    fail_on_fdne(g_ncron_execfile, R_OK | W_OK);
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);

    if (stack.empty()) {
        log_line("%s: no jobs, exiting", __func__);
        std::exit(EXIT_FAILURE);
    }

    if (gflags_background) {
        if (daemon(0,0)) {
            log_line("%s: daemon failed: %s", __func__, strerror(errno));
            std::exit(EXIT_FAILURE);
        }
    }

    umask(077);
    fix_signals();

#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif

    if (s6_notify_fd) {
        char buf[] = "\n";
        safe_write(*s6_notify_fd, buf, 1);
        close(*s6_notify_fd);
    }

    do_work(g_initial_sleep);
    exit(EXIT_SUCCESS);
}

