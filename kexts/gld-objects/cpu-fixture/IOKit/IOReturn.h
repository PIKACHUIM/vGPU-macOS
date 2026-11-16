#ifndef VGO_CPU_FIXTURE_IORETURN_H
#define VGO_CPU_FIXTURE_IORETURN_H
/* 仅本地 CPU fixture 的编译依赖，禁止加入 macOS 生产 include 路径。 */
#include <stdint.h>
typedef int32_t IOReturn;
#define kIOReturnSuccess ((IOReturn)0)
#define kIOReturnBadArgument ((IOReturn)0xe00002c2u)
#define kIOReturnNoMemory ((IOReturn)0xe00002bdu)
#define kIOReturnNoResources ((IOReturn)0xe00002beu)
#define kIOReturnUnsupported ((IOReturn)0xe00002c7u)
#define kIOReturnExclusiveAccess ((IOReturn)0xe00002c5u)
#define kIOReturnNotReady ((IOReturn)0xe00002d8u)
#endif
