/*
 * folia_procstat — external process sampler for macOS (performance-and-QA).
 *
 *   folia_procstat <pid> <interval_ms> <count>
 *
 * Prints one JSON object per line (JSON Lines) per sample, read from outside
 * the target process with proc_pid_rusage(pid, RUSAGE_INFO_V6 or V4). Counter
 * keys are the exact rusage_info field names; CPU times are additionally
 * converted from Mach absolute time units to nanoseconds with
 * mach_timebase_info (on Apple silicon one unit is 125/3 ns, not 1 ns):
 *
 *   sample                          sample index (0-based)
 *   wall_ms                         ms since the first sample (monotonic)
 *   unix_ms                         wall-clock time of the sample (for correlation)
 *   pid, flavor                     target and rusage flavor used
 *   ri_phys_footprint               bytes (what Activity Monitor calls Memory)
 *   ri_lifetime_max_phys_footprint  bytes, peak since process start
 *   ri_interval_max_phys_footprint  bytes, peak since the previous sample (*)
 *   ri_resident_size                bytes
 *   ri_user_time, ri_system_time    raw Mach absolute time units
 *   ri_user_time_ns, ri_system_time_ns  converted to ns
 *   ri_interrupt_wkups, ri_pkg_idle_wkups  cumulative wakeup counts
 *   ri_pageins, ri_instructions, ri_cycles  cumulative
 *   ri_energy_nj                    cumulative (V6 only)
 *
 * (*) The kernel resets the interval maximum on every proc_pid_rusage call
 * for the process, so another sampler running at the same time disturbs it.
 *
 * Samples are scheduled on an absolute grid (start + i * interval) so the
 * period does not drift. <count> 0 samples until the process exits. When the
 * target is gone a final {"sample": i, "error": "..."} line is printed and the
 * exit status is 1. The sampler itself only sleeps between samples.
 */
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

#if defined(RUSAGE_INFO_V6)
typedef struct rusage_info_v6 folia_rusage;
#define FOLIA_FLAVOR RUSAGE_INFO_V6
#define FOLIA_FLAVOR_NAME "RUSAGE_INFO_V6"
#define FOLIA_HAVE_ENERGY 1
#else
typedef struct rusage_info_v4 folia_rusage;
#define FOLIA_FLAVOR RUSAGE_INFO_V4
#define FOLIA_FLAVOR_NAME "RUSAGE_INFO_V4"
#define FOLIA_HAVE_ENERGY 0
#endif

static mach_timebase_info_data_t g_tb;

static uint64_t abs_to_ns(uint64_t t)
{
    /* Split to avoid overflow of t * numer for long-running processes. */
    const uint64_t q = t / g_tb.denom, r = t % g_tb.denom;
    return q * g_tb.numer + (r * g_tb.numer) / g_tb.denom;
}

static int parse_long(const char *s, long long min, long long *out)
{
    char *end = NULL;
    errno = 0;
    const long long v = strtoll(s, &end, 10);
    if (errno || !end || *end || v < min)
        return 0;
    *out = v;
    return 1;
}

static void sleep_until_ns(uint64_t target_abs)
{
    for (;;) {
        const uint64_t now = mach_absolute_time();
        if (now >= target_abs)
            return;
        const uint64_t ns = abs_to_ns(target_abs - now);
        struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        if (nanosleep(&ts, NULL) == 0)
            return;
        if (errno != EINTR)
            return;
    }
}

int main(int argc, char **argv)
{
    long long pid = 0, interval = 0, count = 0;
    if (argc != 4 || !parse_long(argv[1], 1, &pid) || !parse_long(argv[2], 1, &interval)
        || !parse_long(argv[3], 0, &count)) {
        fprintf(stderr, "usage: folia_procstat <pid> <interval_ms >= 1> <count (0 = until exit)>\n");
        return 2;
    }
    if (mach_timebase_info(&g_tb) != KERN_SUCCESS || g_tb.denom == 0) {
        fprintf(stderr, "folia_procstat: mach_timebase_info failed\n");
        return 2;
    }
    const uint64_t interval_abs = (uint64_t)interval * 1000000ull * g_tb.denom / g_tb.numer;
    int flavor = FOLIA_FLAVOR;
    const char *flavor_name = FOLIA_FLAVOR_NAME;
    const uint64_t start = mach_absolute_time();
    for (long long i = 0; count == 0 || i < count; ++i) {
        if (i > 0)
            sleep_until_ns(start + (uint64_t)i * interval_abs);
        folia_rusage ri;
        memset(&ri, 0, sizeof ri);
        const uint64_t now = mach_absolute_time();
        int rc = proc_pid_rusage((int)pid, flavor, (rusage_info_t *)&ri);
        if (rc != 0 && errno == EINVAL && flavor != RUSAGE_INFO_V4) {
            /* Older kernel: fall back to V4 (a prefix of the newer structs). */
            flavor = RUSAGE_INFO_V4;
            flavor_name = "RUSAGE_INFO_V4";
            memset(&ri, 0, sizeof ri);
            rc = proc_pid_rusage((int)pid, flavor, (rusage_info_t *)&ri);
        }
        if (rc != 0) {
            const int e = errno;
            printf("{\"sample\": %lld, \"wall_ms\": %.3f, \"pid\": %lld, \"error\": \"proc_pid_rusage: %s\"}\n", i,
                   abs_to_ns(now - start) / 1e6, pid, strerror(e));
            fflush(stdout);
            return 1;
        }
        struct timeval tv;
        gettimeofday(&tv, NULL);
        const double unix_ms = (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
        printf("{\"sample\": %lld, \"wall_ms\": %.3f, \"unix_ms\": %.3f, \"pid\": %lld, \"flavor\": \"%s\", "
               "\"ri_phys_footprint\": %llu, \"ri_lifetime_max_phys_footprint\": %llu, "
               "\"ri_interval_max_phys_footprint\": %llu, \"ri_resident_size\": %llu, "
               "\"ri_user_time\": %llu, \"ri_system_time\": %llu, "
               "\"ri_user_time_ns\": %llu, \"ri_system_time_ns\": %llu, "
               "\"ri_interrupt_wkups\": %llu, \"ri_pkg_idle_wkups\": %llu, "
               "\"ri_pageins\": %llu, \"ri_instructions\": %llu, \"ri_cycles\": %llu",
               i, abs_to_ns(now - start) / 1e6, unix_ms, pid, flavor_name,
               (unsigned long long)ri.ri_phys_footprint, (unsigned long long)ri.ri_lifetime_max_phys_footprint,
               (unsigned long long)ri.ri_interval_max_phys_footprint, (unsigned long long)ri.ri_resident_size,
               (unsigned long long)ri.ri_user_time, (unsigned long long)ri.ri_system_time,
               (unsigned long long)abs_to_ns(ri.ri_user_time), (unsigned long long)abs_to_ns(ri.ri_system_time),
               (unsigned long long)ri.ri_interrupt_wkups, (unsigned long long)ri.ri_pkg_idle_wkups,
               (unsigned long long)ri.ri_pageins, (unsigned long long)ri.ri_instructions,
               (unsigned long long)ri.ri_cycles);
#if FOLIA_HAVE_ENERGY
        if (flavor == RUSAGE_INFO_V6)
            printf(", \"ri_energy_nj\": %llu", (unsigned long long)ri.ri_energy_nj);
#endif
        printf("}\n");
        fflush(stdout);
    }
    return 0;
}
