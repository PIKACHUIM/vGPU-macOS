#include "gld_pixel_exports.h"
#include "gld_pixel_adapter.h"

#include <OpenGL/CGLTypes.h>

/* 本机SDK核验；成功0同时有GLEngine调用端及硬件destroy的EAX证据。 */
_Static_assert(kCGLNoError == 0, "SDK success");
_Static_assert(kCGLBadAttribute == 10000, "SDK BadAttribute");
_Static_assert(kCGLBadPixelFormat == 10002, "SDK BadPixelFormat");
_Static_assert(kCGLBadValue == 10008, "SDK BadValue");
_Static_assert(kCGLBadMatch == 10009, "SDK BadMatch");
_Static_assert(kCGLBadAlloc == 10016, "SDK BadAlloc");

static int32_t map_status(int32_t status)
{
    switch (status) {
    case GP_OK:
        return (int32_t)kCGLNoError;
    case GP_BAD_ARGUMENT:
    case GP_TRUNCATED_VALUE:
    case GP_UNTERMINATED:
    case GP_UNSUPPORTED_TOKEN:
    case GP_BAD_VALUE:
    case GP_OVERFLOW:
    case GPA_READ_FAILED:
    case GPA_WORD_LIMIT:
        return (int32_t)kCGLBadAttribute;
    case GP_UNSUPPORTED_FORMAT:
    case GPA_INVALID_HANDLE:
        return (int32_t)kCGLBadPixelFormat;
    case GP_NO_MATCH:
    case GPA_BUSY:
    case GPA_DRIVER_MISMATCH:
        return (int32_t)kCGLBadMatch;
    case GP_NO_MEMORY:
    case GPA_GENERATION_EXHAUSTED:
    case GPA_LOCK_FAILED:
        return (int32_t)kCGLBadAlloc;
    default:
        return (int32_t)kCGLBadPixelFormat;
    }
}

static int32_t map_system_choose_status(int32_t status)
{
    switch (status) {
    case GP_BAD_VALUE:
        return (int32_t)kCGLBadValue;
    case GP_NO_MATCH:
        return (int32_t)kCGLNoError;
    default:
        return map_status(status);
    }
}

int32_t gldChoosePixelFormat(void **out_head, const uint32_t *tokens)
{
    return map_system_choose_status(gpa_system_choose(out_head, tokens));
}

int32_t gldDestroyPixelFormat(void *head)
{
    return map_status(gpa_system_destroy_current(head));
}

int32_t gpx_configure(const gp_format_cap *formats, size_t count)
{
    return map_status(gpa_system_configure(formats, count));
}
