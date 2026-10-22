#import <AppKit/AppKit.h>
#include "platform.hpp"
#include <mach/mach.h>
#include <sys/resource.h>
namespace test_platform {
void initialize() { [NSApplication sharedApplication]; }
void process_events() { @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, .001, false); } }
uint64_t resident_bytes()
{
  mach_task_basic_info_data_t i = {}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&i), &n) == KERN_SUCCESS ? i.resident_size : 0;
}
double cpu_seconds()
{
  rusage r = {}; getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
}
