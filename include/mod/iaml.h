#pragma once
#include <stdint.h>

class IAML {
public:
    virtual void*    GetSym(const char* lib, const char* sym) = 0;
    virtual int      Hook(void* addr, void* hook, void** orig) = 0;
    virtual void     ShowToast(bool longDur, const char* fmt, ...) = 0;
    virtual int      MLSSetInt(const char* key, int val) = 0;
    virtual int      MLSGetInt(const char* key, int def) = 0;
    virtual void*    GetJNIEnvironment() = 0;
    virtual void*    GetAppContextObject() = 0;
    virtual uintptr_t GetLib(const char* name) = 0;
};
