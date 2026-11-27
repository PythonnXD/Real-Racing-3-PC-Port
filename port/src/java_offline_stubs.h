#pragma once

#include "config.h"
#include "elf_loader.h"
#include "java_firemint.h"
#include "jni_env.h"

// Registers all offline Cloudcell, NetworkStatusMonitor, HttpRequest, Nimble, GooglePlay,
// Facebook, AccountManager, Analytics, and Android utility JNI classes plus C++ Nimble exports.
void RegisterOfflineStubs(ElfLoader& loader, FakeJvm& jvm, FiremintRuntime& firemint, const PcConfig& cfg);
