#pragma once

#include "config.h"
#include "elf_loader.h"
#include "java_firemint.h"
#include "jni_env.h"

#include <string>

// Returns a persistent per-machine UID (saved under userdata/device_uid.txt) in online mode,
// or the fixed offline identifier when offline.
std::string GetOrCreateDeviceUid(const PcConfig& cfg);

// Registers all offline Cloudcell, NetworkStatusMonitor, HttpRequest, Nimble, GooglePlay,
// Facebook, AccountManager, Analytics, and Android utility JNI classes plus C++ Nimble exports.
void RegisterOfflineStubs(ElfLoader& loader, FakeJvm& jvm, FiremintRuntime& firemint, const PcConfig& cfg);
