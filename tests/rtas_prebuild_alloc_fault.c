/* Test-only glibc interposer. Load explicitly with LD_PRELOAD when running
 * test_raytracing_prebuild_allocation_failure. Never linked into the driver.
 * Only the armed thread's 17-element prebuild arrays are affected. */
#include <stddef.h>
#include <string.h>

extern void *__libc_calloc(size_t count, size_t size);
extern void __libc_free(void *pointer);

static _Thread_local struct
{
    unsigned int armed, fail, calls, failures;
    void *live[3];
} fault;

__attribute__((visibility("default"))) void helios_test_prebuild_alloc_arm(unsigned int fail)
{
    memset(&fault, 0, sizeof(fault));
    fault.fail = fail;
    fault.armed = 1;
}

__attribute__((visibility("default"))) void helios_test_prebuild_alloc_finish(
        unsigned int *calls, unsigned int *failures, unsigned int *live)
{
    unsigned int i;
    fault.armed = 0;
    *calls = fault.calls;
    *failures = fault.failures;
    *live = 0;
    for (i = 0; i < 3; ++i)
        *live += fault.live[i] != NULL;
}

__attribute__((visibility("default"))) void *calloc(size_t count, size_t size)
{
    void *result;
    unsigned int index;
    if (!fault.armed || count != 17)
        return __libc_calloc(count, size);
    index = fault.calls++;
    if (index == fault.fail)
    {
        ++fault.failures;
        return NULL;
    }
    result = __libc_calloc(count, size);
    if (index < 3)
        fault.live[index] = result;
    return result;
}

__attribute__((visibility("default"))) void free(void *pointer)
{
    unsigned int i;
    if (fault.armed && pointer)
        for (i = 0; i < 3; ++i)
            if (fault.live[i] == pointer)
                fault.live[i] = NULL;
    __libc_free(pointer);
}
