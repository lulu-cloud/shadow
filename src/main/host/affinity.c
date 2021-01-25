#include "affinity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // Make sure this is defined for CPU_* macros
#endif              // _GNU_SOURCE

#include <assert.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysinfo.h>
#include <sys/types.h>

#include <glib.h>
#include <gmodule.h>

#include "main/utility/priority_queue.h"
#include "support/logger/logger.h"

/*
 * One core has one or more logical CPUs.
 * One socket has one or more cores.
 * One node has one or more sockets.
 *
 * Logical CPU number is the unique key for each processing unit.
 */
typedef struct _CPUInfo {
    int logical_cpu_num, core, socket, node;
} CPUInfo;


typedef struct _PlatformCPUInfo {
    CPUInfo *p_cpus;
    size_t n_cpus;
    int max_cpu_num;
    // Keep track of how many workers are assigned to each core, socket, and
    // node.
    GHashTable *core_loads, *socket_loads, *node_loads;

    PriorityQueue *cpu_queue;
} PlatformCPUInfo;

static PlatformCPUInfo _global_platform_info = {0};

/*
 * Helper function, takes care of casting.
 */
static inline int _hash_table_lookup(GHashTable *htab, int key_value) {
    assert(htab);

    return
        GPOINTER_TO_INT(g_hash_table_lookup(htab, GINT_TO_POINTER(key_value)));
}

static gint _cpuinfo_compare(const CPUInfo* lhs, const CPUInfo* rhs, gpointer _) {
    assert(lhs && rhs);

    // Always prefer a CPU with lower core load
    int core_load_lhs = _hash_table_lookup(_global_platform_info.core_loads, lhs->core);
    int core_load_rhs = _hash_table_lookup(_global_platform_info.core_loads, rhs->core);
    if (core_load_lhs != core_load_rhs) {
        return core_load_lhs < core_load_rhs ? -1 : 1;
    }

    // If core loads are the same, prefer a *hotter* socket for locality
    int socket_load_lhs = _hash_table_lookup(_global_platform_info.socket_loads, lhs->socket);
    int socket_load_rhs = _hash_table_lookup(_global_platform_info.socket_loads, rhs->socket);
    if (socket_load_lhs != socket_load_rhs) {
        return socket_load_lhs < socket_load_rhs ? 1 : -1;
    }

    // If socket heat is the same, prefer a hotter node for locality
    int node_load_lhs = _hash_table_lookup(_global_platform_info.node_loads, lhs->node);
    int node_load_rhs = _hash_table_lookup(_global_platform_info.node_loads, rhs->node);
    if (node_load_lhs != node_load_rhs) {
        return node_load_lhs < node_load_rhs ? 1 : -1;
    }

    return (lhs->logical_cpu_num > rhs->logical_cpu_num) -
           (lhs->logical_cpu_num < rhs->logical_cpu_num);
}

static gint _affinity_enabled = 1;

static void _increment_hash_table_value(GHashTable *htab, int key_value) {
    int current_value = _hash_table_lookup(htab, key_value);
    ++current_value;
    g_hash_table_insert(htab, GINT_TO_POINTER(key_value), GINT_TO_POINTER(current_value));
}

/*
 * Updates the platform loads assuming one new worker was assigned to input
 * CPU.
 */
static void _update_loads(const CPUInfo *cpu_info) {
    assert(cpu_info);
    _increment_hash_table_value(_global_platform_info.core_loads, cpu_info->core);
    _increment_hash_table_value(_global_platform_info.socket_loads, cpu_info->socket);
    _increment_hash_table_value(_global_platform_info.node_loads, cpu_info->node);
}

int affinity_getGoodWorkerAffinity() {
    // FIXME (rwails): This assumes that the returned affinity was actually
    // used.
    CPUInfo *p_best_cpu = (CPUInfo *)priorityqueue_pop(_global_platform_info.cpu_queue);
    _update_loads(p_best_cpu);
    priorityqueue_push(_global_platform_info.cpu_queue, p_best_cpu);
    return p_best_cpu->logical_cpu_num;
}

/*
 * Read the output of the lscpu command, allocates a buffer, and sets contents
 * to point to the buffer.
 *
 * !!! THE CONTENT BUFFER IS ALLOCATED ON BEHALF OF THE CALLER, AND MUST BE
 * FREED BY THE CALLER !!!
 *
 * RETURN VALUE
 *
 * 0 on success, -1 on error.
 */
static int _affinity_readLSCPU(char **contents) {
    enum { BUFFER_NBYTES = 2048 };
    static const char *LSCPU_COMMAND =
        "lscpu --online --parse=CPU,CORE,SOCKET,NODE";
    assert(contents);

    // Use a memory stream to read in the file contents.
    size_t contents_nbytes = 0;
    FILE *p_sstream = NULL;

    p_sstream = open_memstream(contents, &contents_nbytes);
    if (!p_sstream) { return -1; }

    FILE *p_pipe = popen(LSCPU_COMMAND, "r");
    bool read_success = false;

    if (p_pipe) {
        char buffer[BUFFER_NBYTES] = {0};

        while (fgets(buffer, BUFFER_NBYTES, p_pipe) != NULL) {
            fprintf(p_sstream, buffer);
        }

        int rc = fflush(p_sstream);
        rc |= fclose(p_sstream);
        rc |= pclose(p_pipe);

        read_success = (rc == 0);
    }

    return read_success ? 0 : -1;
}

/*
 * PARAMETERS
 *
 * line
 *  Input. The input buffer will be modified during tokenization
 *
 * parsed_info
 *  Output. Pass in pointer to CPUInfo struct and it will be populated.
 *
 * RETURN VALUE
 *
 * 0 on success, -1 on error.
 */
static int _affinity_parseLSCPULine(char *line, CPUInfo *parsed_info) {
    assert(line && parsed_info);
    static const char *DELIM = ",";
    char *p_save = NULL; // Used by strtok_r.
    int field_ctr = 0;

    char *token = strtok_r(line, DELIM, &p_save);

    while (token) {
        int value = atoi(token);

        switch (field_ctr) {
            case 0:
                parsed_info->logical_cpu_num = value;
                break;
            case 1:
                parsed_info->core = value;
                break;
            case 2:
                parsed_info->socket = value;
                break;
            case 3:
                parsed_info->node = value;
                break;
            default:
                return -1;
        }

        ++field_ctr;
        token = strtok_r(NULL, DELIM, &p_save);
    }

    // Make sure we've parsed four fields.
    return field_ctr == 4 ? 0 : -1;
}

/*
 * !!! THE CPUS BUFFER IS ALLOCATED ON BEHALF OF THE CALLER, AND MUST BE
 * FREED BY THE CALLER !!!
 */
static int _affinity_parseLSCPUOutput(char *contents, CPUInfo **cpus, size_t *n) {
    assert(contents && cpus && n);

    FILE *p_sstream = fmemopen(contents, strlen(contents), "r");
    if (!p_sstream) { return -1; }

    char *line_buffer = NULL;
    size_t buffer_nbytes = 0;
    ssize_t nbytes_read = 0;

    CPUInfo cpu_info = {-1};

    *n = 0;

    while ((nbytes_read = getline(&line_buffer, &buffer_nbytes, p_sstream)) > 0) {
        if (line_buffer[0] != '#') { // Skip the comments
            _affinity_parseLSCPULine(line_buffer, &cpu_info);
            ++(*n);
            *cpus = realloc(*cpus, sizeof(CPUInfo) * (*n));
            assert(*cpus);
            (*cpus)[*n - 1] = cpu_info;
        }
    }

    if (line_buffer) { free(line_buffer); }

    return 0;
}

static void _global_platform_info_hash_tables_init() {

    assert(_global_platform_info.p_cpus);

    _global_platform_info.core_loads = g_hash_table_new(g_direct_hash, g_direct_equal);
    _global_platform_info.socket_loads = g_hash_table_new(g_direct_hash, g_direct_equal);
    _global_platform_info.node_loads = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (size_t idx = 0; idx < _global_platform_info.n_cpus; ++idx) {
        CPUInfo *p_info = &_global_platform_info.p_cpus[idx];

        g_hash_table_insert(_global_platform_info.core_loads,
                            GINT_TO_POINTER(p_info->core),
                            GINT_TO_POINTER(0));

        g_hash_table_insert(_global_platform_info.socket_loads,
                            GINT_TO_POINTER(p_info->socket),
                            GINT_TO_POINTER(0));

        g_hash_table_insert(_global_platform_info.node_loads,
                            GINT_TO_POINTER(p_info->node),
                            GINT_TO_POINTER(0));
    }
}


int affinity_initPlatformInfo() {

    char *lscpu_contents = NULL;
    int rc = _affinity_readLSCPU(&lscpu_contents);
    assert(lscpu_contents);

    if (rc) {
        error("Could not run `lscpu`, which is required for CPU pinning.");
        return -1;
    }

    rc = _affinity_parseLSCPUOutput(lscpu_contents,
                                    &_global_platform_info.p_cpus,
                                    &_global_platform_info.n_cpus);

    _global_platform_info_hash_tables_init();

    for (size_t idx = 0; idx < _global_platform_info.n_cpus; ++idx) {
        CPUInfo *p_info = &_global_platform_info.p_cpus[idx];
        _global_platform_info.max_cpu_num =
            MAX(_global_platform_info.max_cpu_num, p_info->logical_cpu_num);
    }

    _global_platform_info.cpu_queue = priorityqueue_new((GCompareDataFunc)_cpuinfo_compare, NULL, NULL);

    // _update_loads(&_global_platform_info.p_cpus[0]);

    priorityqueue_push(_global_platform_info.cpu_queue, &_global_platform_info.p_cpus[0]);
    priorityqueue_push(_global_platform_info.cpu_queue, &_global_platform_info.p_cpus[1]);

    CPUInfo *x = (CPUInfo *)priorityqueue_peek(_global_platform_info.cpu_queue);

    printf("Peeked %d\n", x->logical_cpu_num);

    for (size_t idx = 0; idx < _global_platform_info.n_cpus; ++idx) {
        printf("%d %d %d %d\n", _global_platform_info.p_cpus[idx].logical_cpu_num,
                _global_platform_info.p_cpus[idx].core,
                _global_platform_info.p_cpus[idx].socket,
                _global_platform_info.p_cpus[idx].node);
    }

    if (rc) {
        // error("Could not run `lscpu`, which is required for CPU pinning.");
        return -1;
    }

    if (lscpu_contents) {
        free(lscpu_contents);
    }

    return 0;
}

int affinity_setProcessAffinity(pid_t pid, int new_cpu_num, int old_cpu_num) {
    assert(_ncpus > 0 && pid >= 0);

    // We can short-circuit if there's no work to do.
    if (!_affinity_enabled || new_cpu_num == AFFINITY_UNINIT || new_cpu_num == old_cpu_num) {
        return old_cpu_num;
    }

    cpu_set_t* cpu_set = NULL;
    bool set_affinity_suceeded = false;
    int retval = new_cpu_num;

    cpu_set = CPU_ALLOC(_global_platform_info.max_cpu_num);

    if (cpu_set) { // Good, the CPU set allocation succeeded

        size_t cpu_set_size = CPU_ALLOC_SIZE(_global_platform_info.max_cpu_num);

        // Clear the CPU set
        CPU_ZERO_S(cpu_set_size, cpu_set);
        // And then add the new_cpu_num as the only element of the set
        CPU_SET_S(new_cpu_num, cpu_set_size, cpu_set);

        int rc = sched_setaffinity(pid, cpu_set_size, cpu_set);

        set_affinity_suceeded = (rc == 0);
    }

    if (!set_affinity_suceeded) {
        critical("cpu-pin was set, but the CPU affinity for PID %d could not be set to %d",
                 (int)pid, new_cpu_num);
        retval = old_cpu_num;
    }

    if (cpu_set) {
        CPU_FREE(cpu_set);
    }

    return retval;
}
