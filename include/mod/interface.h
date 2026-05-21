#pragma once
#include <dlfcn.h>

// GetInterface di-provide oleh AML loader saat runtime.
// Kita resolve via dlsym dari proses yang sudah berjalan.
inline void* GetInterface(const char* name) {
    static void* (*fn)(const char*) = nullptr;
    if (!fn) {
        fn = (void*(*)(const char*))dlsym(RTLD_DEFAULT, "GetInterface");
    }
    if (!fn) return nullptr;
    return fn(name);
}
