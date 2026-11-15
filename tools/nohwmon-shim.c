// Preload library for MangoHud/mangoapp: this phone's kernel is built without
// hardware-monitor support, so /sys/class/hwmon does not exist and MangoHud
// aborts while scanning it.  Listing that directory is answered with an empty
// directory instead.
//
// Build: gcc -shared -fPIC -O2 -o libaston-nohwmon.so nohwmon-shim.c -ldl
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>

#define EMPTY_DIR "/var/empty"

DIR *opendir(const char *name)
{
    static DIR *(*real)(const char *);
    if (!real)
        real = dlsym(RTLD_NEXT, "opendir");
    DIR *d = real(name);
    if (!d && name && !strncmp(name, "/sys/class/hwmon", 16))
        d = real(EMPTY_DIR);
    return d;
}
