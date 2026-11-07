/*
 * guest-probe: a known memory target for exercising mpfshost from inside a Windows guest.
 *
 * It allocates a page-aligned buffer, fills it with a repeatable magic byte and prints one
 * machine-readable line with the process id, buffer address, size and magic value. The process
 * then idles so the host can read the buffer; --watch additionally reports every changed byte.
 */

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* program)
{
    fprintf(stderr,
            "Usage: %s [--size <bytes>] [--magic <hex byte>] [--watch]\n"
            "  --size <bytes>      Size of the probe buffer (default: 4096)\n"
            "  --magic <hex byte>  Byte the buffer is filled with (default: 0xa5)\n"
            "  --watch             Poll the buffer and print every changed byte\n",
            program);
}

static int parse_size(const char* text, size_t* value)
{
    char*              end    = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 || parsed > (unsigned long long)(16u * 1024u * 1024u))
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

int main(int argc, char** argv)
{
    size_t   size  = 4096;
    unsigned magic = 0xA5;
    int      watch = 0;

    for (int index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--watch") == 0)
        {
            watch = 1;
        }
        else if (strcmp(argv[index], "--size") == 0 && index + 1 < argc)
        {
            if (!parse_size(argv[++index], &size))
            {
                fprintf(stderr, "guest-probe: --size needs a positive byte count\n");
                return 1;
            }
        }
        else if (strcmp(argv[index], "--magic") == 0 && index + 1 < argc)
        {
            if (!parse_magic(argv[++index], &magic))
            {
                fprintf(stderr, "guest-probe: --magic needs a hex byte, e.g. 0xa5\n");
                return 1;
            }
        }
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }

    unsigned char* buffer = (unsigned char*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buffer == NULL)
    {
        fprintf(stderr, "guest-probe: VirtualAlloc failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    memset(buffer, (int)magic, size);
    printf("PROBE pid=%lu address=0x%llx size=%llu magic=0x%02x\n",
           (unsigned long)GetCurrentProcessId(),
           (unsigned long long)(uintptr_t)buffer,
           (unsigned long long)size,
           magic);
    fflush(stdout);

    unsigned char* snapshot = NULL;
    if (watch)
    {
        snapshot = (unsigned char*)malloc(size);
        if (snapshot == NULL)
        {
            fprintf(stderr, "guest-probe: out of memory for the watch snapshot\n");
            VirtualFree(buffer, 0, MEM_RELEASE);
            return 1;
        }
        memcpy(snapshot, buffer, size);
    }

    for (;;)
    {
        Sleep(500);

        if (!watch)
        {
            continue;
        }

        for (size_t offset = 0; offset < size; ++offset)
        {
            if (buffer[offset] != snapshot[offset])
            {
                printf("CHANGE offset=0x%llx old=0x%02x new=0x%02x\n",
                       (unsigned long long)offset,
                       (unsigned)snapshot[offset],
                       (unsigned)buffer[offset]);
                snapshot[offset] = buffer[offset];
            }
        }
        fflush(stdout);
    }
}
