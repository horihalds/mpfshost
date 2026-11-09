/*
 * guest-probe: a known fixture for exercising mpfshost from inside a Windows guest.
 *
 * It allocates known regions, exports stable markers, and optionally mutates a trigger byte on a timer.
 * Everything needed to drive a live test from the host is printed once at startup as key=value lines.
 * The process then idles until Ctrl-C or --duration, so the host can read, write, search, watch and
 * break on the fixture.
 */

#include <windows.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_MAX_BUFFER           (16u * 1024u * 1024u)
#define PROBE_MAX_THREADS          64u
#define PROBE_REGION_SIZE          4096u
#define PROBE_DEFAULT_HEARTBEAT_MS 500u
#define PROBE_DEFAULT_THREADS      2u
#define PROBE_DEFAULT_WATCH_MS     500u
#define PROBE_PATTERN_SIZE         64u

static const unsigned char kBreakpointStub[] = {0xF3, 0x0F, 0x1E, 0xFA, 0xC3};

#if defined(__GNUC__)
#define PROBE_NOINLINE __attribute__((noinline, used))
#else
#define PROBE_NOINLINE
#endif

#define PROBE_EXPORT __declspec(dllexport)

typedef struct
{
    size_t      size;
    unsigned    magic;
    int         pattern;
    int         regions;
    int         watch;
    unsigned    watchIntervalMs;
    unsigned    heartbeatMs;
    unsigned    threads;
    unsigned    durationSeconds;
    int         logEnabled;
    int         help;
    const char* logPath;
    unsigned    heartbeatCpu;
} Options;

typedef struct
{
    const char* name;
    void*       address;
    size_t      size;
} Region;

static HANDLE g_stopEvent  = NULL;
static FILE*  g_log        = NULL;
static LONG   g_stopReason = 0;
static Region g_regions[8];
static size_t g_regionCount    = 0;
static size_t g_regionCapacity = sizeof(g_regions) / sizeof(g_regions[0]);

static volatile unsigned char* g_trigger      = NULL;
static unsigned                g_heartbeatMs  = PROBE_DEFAULT_HEARTBEAT_MS;
static unsigned                g_heartbeatCpu = 0;
static void (*g_breakpointTarget)(void)       = NULL;
static unsigned char* g_watchBuffer           = NULL;
static unsigned char* g_watchSnapshot         = NULL;
static size_t         g_watchSize             = 0;
static unsigned       g_watchIntervalMs       = PROBE_DEFAULT_WATCH_MS;

#if defined(__cplusplus)
extern "C"
{
#endif

    PROBE_EXPORT volatile unsigned long long GuestProbeCounter = 0;

    PROBE_EXPORT PROBE_NOINLINE void GuestProbeMarker(unsigned long long tick);
    PROBE_EXPORT PROBE_NOINLINE void GuestProbeHeartbeat(unsigned long long tick);

    PROBE_EXPORT PROBE_NOINLINE void GuestProbeMarker(unsigned long long tick)
    {
        GuestProbeCounter = tick;
    }

    PROBE_EXPORT PROBE_NOINLINE void GuestProbeHeartbeat(unsigned long long tick)
    {
        if (g_trigger != NULL)
        {
            g_trigger[0] = (unsigned char)((tick & 1u) != 0 ? 0xA5u : 0x5Au);
        }
        if (g_breakpointTarget != NULL)
        {
            g_breakpointTarget();
        }
        GuestProbeMarker(tick);
    }

#if defined(__cplusplus)
}
#endif

static void emit(const char* format, ...)
{
    va_list arguments;
    va_list copy;

    va_start(arguments, format);
    va_copy(copy, arguments);
    vfprintf(stdout, format, arguments);
    if (g_log != NULL)
    {
        vfprintf(g_log, format, copy);
    }
    va_end(copy);
    va_end(arguments);
    fflush(stdout);
    if (g_log != NULL)
    {
        fflush(g_log);
    }
}

static void report_last_error(const char* what)
{
    fprintf(stderr, "guest-probe: %s failed: %lu\n", what, (unsigned long)GetLastError());
}

static void print_usage(const char* program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --size <bytes>          Size of the main buffer (default: 4096, max 16 MiB)\n"
            "  --magic <hex byte>      Byte the main buffer is filled with (default: 0xa5)\n"
            "  --pattern               Overlay a known 64-byte pattern at offset 0 (default: on)\n"
            "  --no-pattern            Keep the main buffer uniformly magic-filled\n"
            "  --watch                 Report external writes to the main buffer (default: on)\n"
            "  --no-watch              Do not scan the main buffer for external writes\n"
            "  --watch-interval <ms>   Delay between main-buffer scans (default: 500, min 50)\n"
            "  --regions               Create the ro/rx/noaccess/guard/reserve regions (default: on)\n"
            "  --no-regions            Create only the main buffer and the trigger page\n"
            "  --heartbeat <ms>        Toggle the trigger byte and call GuestProbeMarker (default: 500)\n"
            "  --heartbeat-cpu <n>     Pin the heartbeat thread to this vCPU (default: 0, max 63)\n"
            "  --no-heartbeat          Disable the trigger byte and the marker calls\n"
            "  --threads <n>           Number of idle worker threads (default: 2, max 64)\n"
            "  --duration <seconds>    Exit after this many seconds (default: run until Ctrl-C)\n"
            "  --log <path>            Mirror output to this file (default: %%TEMP%%\\guest-probe-<pid>.log)\n"
            "  --no-log                Do not write a log file\n"
            "  --help                  Show this help\n",
            program);
}

static int parse_size(const char* text, size_t* value)
{
    char*              end    = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 || parsed > (unsigned long long)PROBE_MAX_BUFFER)
    {
        return 0;
    }
    *value = (size_t)parsed;
    return 1;
}

static int parse_magic(const char* text, unsigned* value)
{
    char*         end    = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    if (end == text || *end != '\0' || parsed > 0xFFu)
    {
        return 0;
    }
    *value = (unsigned)parsed;
    return 1;
}

static int parse_uint(const char* text, unsigned minimum, unsigned maximum, unsigned* value)
{
    char*         end    = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum)
    {
        return 0;
    }
    *value = (unsigned)parsed;
    return 1;
}

static int parse_options(int argc, char** argv, Options* options)
{
    for (int index = 1; index < argc; ++index)
    {
        const char* argument = argv[index];

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
        {
            options->help = 1;
        }
        else if (strcmp(argument, "--size") == 0 && index + 1 < argc)
        {
            if (!parse_size(argv[++index], &options->size))
            {
                fprintf(stderr, "guest-probe: --size needs a positive byte count up to 16 MiB\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--magic") == 0 && index + 1 < argc)
        {
            if (!parse_magic(argv[++index], &options->magic))
            {
                fprintf(stderr, "guest-probe: --magic needs a hex byte, e.g. 0xa5\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--pattern") == 0)
        {
            options->pattern = 1;
        }
        else if (strcmp(argument, "--no-pattern") == 0)
        {
            options->pattern = 0;
        }
        else if (strcmp(argument, "--watch") == 0)
        {
            options->watch = 1;
        }
        else if (strcmp(argument, "--no-watch") == 0)
        {
            options->watch = 0;
        }
        else if (strcmp(argument, "--watch-interval") == 0 && index + 1 < argc)
        {
            if (!parse_uint(argv[++index], 50, 600000, &options->watchIntervalMs))
            {
                fprintf(stderr, "guest-probe: --watch-interval needs milliseconds between 50 and 600000\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--regions") == 0)
        {
            options->regions = 1;
        }
        else if (strcmp(argument, "--no-regions") == 0)
        {
            options->regions = 0;
        }
        else if (strcmp(argument, "--heartbeat") == 0 && index + 1 < argc)
        {
            if (!parse_uint(argv[++index], 0, 600000, &options->heartbeatMs))
            {
                fprintf(stderr, "guest-probe: --heartbeat needs milliseconds between 0 and 600000\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--heartbeat-cpu") == 0 && index + 1 < argc)
        {
            if (!parse_uint(argv[++index], 0, 63, &options->heartbeatCpu))
            {
                fprintf(stderr, "guest-probe: --heartbeat-cpu needs a vCPU number between 0 and 63\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--no-heartbeat") == 0)
        {
            options->heartbeatMs = 0;
        }
        else if (strcmp(argument, "--threads") == 0 && index + 1 < argc)
        {
            if (!parse_uint(argv[++index], 0, PROBE_MAX_THREADS, &options->threads))
            {
                fprintf(stderr, "guest-probe: --threads needs a count between 0 and 64\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--duration") == 0 && index + 1 < argc)
        {
            if (!parse_uint(argv[++index], 0, 86400, &options->durationSeconds))
            {
                fprintf(stderr, "guest-probe: --duration needs seconds between 0 and 86400\n");
                return 0;
            }
        }
        else if (strcmp(argument, "--log") == 0 && index + 1 < argc)
        {
            options->logPath = argv[++index];
        }
        else if (strcmp(argument, "--no-log") == 0)
        {
            options->logEnabled = 0;
        }
        else
        {
            fprintf(stderr, "guest-probe: unknown option '%s'\n", argument);
            return 0;
        }
    }

    return 1;
}

static void build_pattern(unsigned char* pattern)
{
    static const char prefix[] = "MPFSHOST-GUEST-PROBE";
    size_t            used     = sizeof(prefix) - 1;

    memset(pattern, 0, PROBE_PATTERN_SIZE);
    memcpy(pattern, prefix, used);
    for (size_t index = used; index < PROBE_PATTERN_SIZE; ++index)
    {
        pattern[index] = (unsigned char)(index * 7u + 3u);
    }
}

static void format_hex(char* output, size_t capacity, const unsigned char* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    size_t            position = 0;

    for (size_t index = 0; index < size && position + 2 < capacity; ++index)
    {
        output[position++] = digits[(data[index] >> 4) & 0xFu];
        output[position++] = digits[data[index] & 0xFu];
    }
    output[position] = '\0';
}

static void remember_region(const char* name, void* address, size_t size)
{
    if (g_regionCount < g_regionCapacity)
    {
        g_regions[g_regionCount].name    = name;
        g_regions[g_regionCount].address = address;
        g_regions[g_regionCount].size    = size;
        ++g_regionCount;
    }
}

static void emit_region(const char* name, void* address, size_t size, const char* state, const char* protection)
{
    emit("PROBE-REGION name=%s address=0x%llx size=%llu state=%s protection=%s\n",
         name,
         (unsigned long long)(uintptr_t)address,
         (unsigned long long)size,
         state,
         protection);
}

static void free_regions(void)
{
    for (size_t index = 0; index < g_regionCount; ++index)
    {
        if (g_regions[index].address != NULL)
        {
            VirtualFree(g_regions[index].address, 0, MEM_RELEASE);
        }
    }
    g_regionCount = 0;
}

static void print_module_inventory(void)
{
    HMODULE            module = GetModuleHandleA(NULL);
    char               path[MAX_PATH];
    DWORD              length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    const char*        name   = path;
    unsigned long long size   = 0;

    if (length == 0 || length >= sizeof(path))
    {
        strcpy(path, "guest-probe.exe");
    }

    const char* separator = strrchr(path, '\\');
    if (separator != NULL)
    {
        name = separator + 1;
    }

    if (module != NULL)
    {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const unsigned char*)module + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                size = (unsigned long long)nt->OptionalHeader.SizeOfImage;
            }
        }
    }

    emit("PROBE-MODULE name=%s base=0x%llx size=0x%llx path=%s\n",
         name,
         (unsigned long long)(uintptr_t)module,
         size,
         path);
}

static void print_export_inventory(void)
{
    emit("PROBE-EXPORT name=GuestProbeMarker address=0x%llx\n", (unsigned long long)(uintptr_t)&GuestProbeMarker);
    emit("PROBE-EXPORT name=GuestProbeHeartbeat address=0x%llx\n", (unsigned long long)(uintptr_t)&GuestProbeHeartbeat);
    emit("PROBE-EXPORT name=GuestProbeCounter address=0x%llx\n", (unsigned long long)(uintptr_t)&GuestProbeCounter);
}

static void* create_region(const char*          name,
                           unsigned char        fill,
                           const char*          protection,
                           DWORD                protect,
                           int                  isReserve,
                           const unsigned char* code,
                           size_t               codeSize)
{
    DWORD oldProtection = 0;
    void* address       = NULL;

    if (isReserve)
    {
        address = VirtualAlloc(NULL, PROBE_REGION_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (address == NULL)
        {
            report_last_error("VirtualAlloc(reserve)");
            return NULL;
        }
        remember_region(name, address, PROBE_REGION_SIZE);
        emit_region(name, address, PROBE_REGION_SIZE, "reserve", protection);
        return address;
    }

    address = VirtualAlloc(NULL, PROBE_REGION_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (address == NULL)
    {
        report_last_error("VirtualAlloc");
        return NULL;
    }

    memset(address, fill, PROBE_REGION_SIZE);
    if (code != NULL && codeSize > 0 && codeSize <= PROBE_REGION_SIZE)
    {
        memcpy(address, code, codeSize);
    }
    if (!VirtualProtect(address, PROBE_REGION_SIZE, protect, &oldProtection))
    {
        report_last_error("VirtualProtect");
        VirtualFree(address, 0, MEM_RELEASE);
        return NULL;
    }

    remember_region(name, address, PROBE_REGION_SIZE);
    emit_region(name, address, PROBE_REGION_SIZE, "commit", protection);
    return address;
}

static DWORD WINAPI heartbeat_thread(void* parameter)
{
    unsigned long long tick = 0;
    (void)parameter;

    if (g_heartbeatCpu < 64)
    {
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1u << g_heartbeatCpu);
    }

    for (;;)
    {
        if (WaitForSingleObject(g_stopEvent, g_heartbeatMs) != WAIT_TIMEOUT)
        {
            break;
        }
        ++tick;
        GuestProbeHeartbeat(tick);
    }

    return 0;
}

static DWORD WINAPI watch_thread(void* parameter)
{
    (void)parameter;

    for (;;)
    {
        if (WaitForSingleObject(g_stopEvent, g_watchIntervalMs) != WAIT_TIMEOUT)
        {
            break;
        }

        for (size_t offset = 0; offset < g_watchSize; ++offset)
        {
            unsigned char current = g_watchBuffer[offset];
            if (current != g_watchSnapshot[offset])
            {
                emit("CHANGE offset=0x%llx old=0x%02x new=0x%02x\n",
                     (unsigned long long)offset,
                     (unsigned)g_watchSnapshot[offset],
                     (unsigned)current);
                g_watchSnapshot[offset] = current;
            }
        }
    }

    return 0;
}

static DWORD WINAPI worker_thread(void* parameter)
{
    (void)parameter;
    WaitForSingleObject(g_stopEvent, INFINITE);
    return 0;
}

static BOOL WINAPI console_handler(DWORD control)
{
    (void)control;
    InterlockedCompareExchange(&g_stopReason, 1, 0);
    if (g_stopEvent != NULL)
    {
        SetEvent(g_stopEvent);
    }
    return TRUE;
}

int main(int argc, char** argv)
{
    Options options;
    memset(&options, 0, sizeof(options));
    options.size            = 4096;
    options.magic           = 0xA5;
    options.pattern         = 1;
    options.regions         = 1;
    options.watch           = 1;
    options.watchIntervalMs = PROBE_DEFAULT_WATCH_MS;
    options.heartbeatMs     = PROBE_DEFAULT_HEARTBEAT_MS;
    options.threads         = PROBE_DEFAULT_THREADS;
    options.durationSeconds = 0;
    options.logEnabled      = 1;
    options.logPath         = NULL;
    options.heartbeatCpu    = 0;

    if (!parse_options(argc, argv, &options))
    {
        print_usage(argv[0]);
        return 1;
    }
    if (options.help)
    {
        print_usage(argv[0]);
        return 0;
    }

    unsigned char* buffer   = NULL;
    unsigned char* snapshot = NULL;
    HANDLE         threads[2 + PROBE_MAX_THREADS];
    size_t         threadCount = 0;
    int            exitCode    = 1;

    g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_stopEvent == NULL)
    {
        report_last_error("CreateEvent");
        return 1;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);

    do
    {
        char        defaultLog[MAX_PATH];
        const char* logPath = options.logPath;
        if (options.logEnabled)
        {
            if (logPath == NULL)
            {
                char  temporary[MAX_PATH];
                DWORD length = GetTempPathA((DWORD)sizeof(temporary), temporary);
                if (length == 0 || length >= sizeof(temporary))
                {
                    strcpy(temporary, ".\\");
                }
                snprintf(defaultLog,
                         sizeof(defaultLog),
                         "%sguest-probe-%lu.log",
                         temporary,
                         (unsigned long)GetCurrentProcessId());
                logPath = defaultLog;
            }

            g_log = fopen(logPath, "w");
            if (g_log == NULL)
            {
                fprintf(stderr, "guest-probe: cannot open log '%s': %s\n", logPath, strerror(errno));
                if (options.logPath != NULL)
                {
                    break;
                }
                logPath = NULL;
            }
            else
            {
                emit("PROBE-LOG path=%s\n", logPath);
            }
        }

        buffer = (unsigned char*)VirtualAlloc(NULL, options.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (buffer == NULL)
        {
            report_last_error("VirtualAlloc(main)");
            break;
        }
        remember_region("main", buffer, options.size);
        memset(buffer, (int)options.magic, options.size);

        unsigned char pattern[PROBE_PATTERN_SIZE];
        size_t        patternSize = options.size < PROBE_PATTERN_SIZE ? options.size : PROBE_PATTERN_SIZE;
        build_pattern(pattern);
        if (options.pattern)
        {
            memcpy(buffer, pattern, patternSize);
        }

        emit("PROBE version=2 pid=%lu address=0x%llx size=%llu magic=0x%02x\n",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long long)(uintptr_t)buffer,
             (unsigned long long)options.size,
             options.magic);
        print_module_inventory();
        print_export_inventory();

        if (options.pattern)
        {
            char hexadecimal[PROBE_PATTERN_SIZE * 2 + 1];
            format_hex(hexadecimal, sizeof(hexadecimal), pattern, patternSize);
            emit("PROBE-PATTERN offset=0x0 size=%llu hex=%s\n", (unsigned long long)patternSize, hexadecimal);
        }

        emit_region("main", buffer, options.size, "commit", "rw");

        if (options.regions)
        {
            create_region("ro", 0x52, "r", PAGE_READONLY, 0, NULL, 0);
            void* rx = create_region("rx", 0x90, "rx", PAGE_EXECUTE_READ, 0, kBreakpointStub, sizeof(kBreakpointStub));
            if (rx != NULL)
            {
                g_breakpointTarget = (void (*)(void))rx;
                emit("PROBE-BREAKPOINT address=0x%llx\n", (unsigned long long)(uintptr_t)rx);
            }
            create_region("noaccess", 0x4E, "noaccess", PAGE_NOACCESS, 0, NULL, 0);
            create_region("guard", 0x47, "guard", PAGE_READWRITE | PAGE_GUARD, 0, NULL, 0);
            create_region("reserve", 0, "noaccess", PAGE_NOACCESS, 1, NULL, 0);
        }

        if (options.heartbeatMs > 0)
        {
            g_trigger = (volatile unsigned char*)VirtualAlloc(
                NULL, PROBE_REGION_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (g_trigger == NULL)
            {
                report_last_error("VirtualAlloc(trigger)");
            }
            else
            {
                memset((void*)g_trigger, 0, PROBE_REGION_SIZE);
                remember_region("trigger", (void*)g_trigger, PROBE_REGION_SIZE);
                emit_region("trigger", (void*)g_trigger, PROBE_REGION_SIZE, "commit", "rw");
                emit("PROBE-TRIGGER address=0x%llx width=1 interval-ms=%u cpu=%u\n",
                     (unsigned long long)(uintptr_t)g_trigger,
                     options.heartbeatMs,
                     options.heartbeatCpu);
            }
        }

        if (options.watch)
        {
            snapshot = (unsigned char*)malloc(options.size);
            if (snapshot == NULL)
            {
                fprintf(stderr, "guest-probe: out of memory for the watch snapshot\n");
                options.watch = 0;
            }
            else
            {
                memcpy(snapshot, buffer, options.size);
                g_watchBuffer     = buffer;
                g_watchSnapshot   = snapshot;
                g_watchSize       = options.size;
                g_watchIntervalMs = options.watchIntervalMs;
            }
        }

        if (options.heartbeatMs > 0 && g_trigger != NULL)
        {
            DWORD threadId = 0;
            g_heartbeatMs  = options.heartbeatMs;
            g_heartbeatCpu = options.heartbeatCpu;
            HANDLE thread  = CreateThread(NULL, 0, heartbeat_thread, NULL, 0, &threadId);
            if (thread == NULL)
            {
                report_last_error("CreateThread(heartbeat)");
            }
            else
            {
                threads[threadCount++] = thread;
                emit("PROBE-THREAD role=heartbeat index=0 tid=%lu\n", (unsigned long)threadId);
            }
        }

        if (options.watch && snapshot != NULL)
        {
            DWORD  threadId = 0;
            HANDLE thread   = CreateThread(NULL, 0, watch_thread, NULL, 0, &threadId);
            if (thread == NULL)
            {
                report_last_error("CreateThread(watch)");
            }
            else
            {
                threads[threadCount++] = thread;
                emit("PROBE-THREAD role=watch index=0 tid=%lu\n", (unsigned long)threadId);
            }
        }

        for (unsigned index = 0; index < options.threads; ++index)
        {
            DWORD  threadId = 0;
            HANDLE thread   = CreateThread(NULL, 0, worker_thread, (void*)(uintptr_t)index, 0, &threadId);
            if (thread == NULL)
            {
                report_last_error("CreateThread(worker)");
                break;
            }
            threads[threadCount++] = thread;
            emit("PROBE-THREAD role=worker index=%u tid=%lu\n", index, (unsigned long)threadId);
        }

        if (options.durationSeconds > 0)
        {
            DWORD timeout = (DWORD)(options.durationSeconds * 1000u);
            DWORD result  = WaitForSingleObject(g_stopEvent, timeout);
            if (result == WAIT_TIMEOUT)
            {
                InterlockedCompareExchange(&g_stopReason, 2, 0);
                SetEvent(g_stopEvent);
            }
            else if (result == WAIT_OBJECT_0)
            {
                InterlockedCompareExchange(&g_stopReason, 1, 0);
            }
            else
            {
                report_last_error("WaitForSingleObject");
            }
        }
        else
        {
            DWORD result = WaitForSingleObject(g_stopEvent, INFINITE);
            if (result == WAIT_OBJECT_0)
            {
                InterlockedCompareExchange(&g_stopReason, 1, 0);
            }
            else
            {
                report_last_error("WaitForSingleObject");
            }
        }

        emit("PROBE-DONE reason=%s\n", g_stopReason == 2 ? "duration" : "signal");
        exitCode = 0;
    }
    while (0);

    if (g_stopEvent != NULL)
    {
        SetEvent(g_stopEvent);
    }
    for (size_t index = 0; index < threadCount; ++index)
    {
        WaitForSingleObject(threads[index], INFINITE);
        CloseHandle(threads[index]);
    }

    if (snapshot != NULL)
    {
        free(snapshot);
    }
    free_regions();
    if (g_log != NULL)
    {
        fclose(g_log);
        g_log = NULL;
    }
    if (g_stopEvent != NULL)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }

    return exitCode;
}
