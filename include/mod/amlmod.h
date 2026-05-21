#pragma once
#include "interface.h"
#include "iaml.h"

IAML* aml = nullptr;

struct AMLInitStub {
    AMLInitStub() {
        aml = (IAML*)GetInterface("AMLInterface");
    }
};

#define MYMOD(guid, name, ver, author) \
    AMLInitStub _amlStub __attribute__((init_priority(101))); \
    extern "C" __attribute__((visibility("default"))) void* __GetModInfo() { \
        static const char* info = #name "|" #ver "|AML Mod|" #author; \
        return (void*)info; \
    }

#define ON_MOD_PRELOAD() \
    extern "C" __attribute__((visibility("default"))) void OnModPreLoad()

#define ON_MOD_LOAD() \
    extern "C" __attribute__((visibility("default"))) void OnModLoad()
