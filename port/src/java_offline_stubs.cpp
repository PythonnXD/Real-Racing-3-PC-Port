#include "java_offline_stubs.h"

#include <cstdio>
#include <string>
#include <vector>

void RegisterOfflineStubs(ElfLoader& loader, FakeJvm& jvm, FiremintRuntime& firemint, const PcConfig& cfg) {
    // ---- Native C++ Nimble bridge exports imported by libRealRacing3.so ----
    loader.RegisterHostFunction("_ZN2EA6Nimble6getEnvEv", [&jvm](CpuContext& ctx) {
        ctx.SetX(0, jvm.GetJniEnvGuestPtr());
    });
    loader.RegisterHostFunction("_ZN2EA6Nimble9findClassEPKc", [&jvm](CpuContext& ctx) {
        const char* raw = reinterpret_cast<const char*>(static_cast<uintptr_t>(ctx.GetX(0)));
        if (!raw || !*raw) { ctx.SetX(0, 0); return; }
        uint64_t h = jvm.FindClassHandle(raw);
        if (!h) {
            if (IsVerboseLogging()) {
                std::fprintf(stderr, "[OFFLINE] Auto-stubbing Nimble class: %s\n", raw);
            }
            h = jvm.RegisterJniClass(JniClassDef{raw});
        }
        ctx.SetX(0, h);
    });

    // ---- Android utility classes ----
    {
        JniClassDef d;
        d.name = "java/io/File";
        d.methods["<init>:(Ljava/lang/String;)V"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (!a.empty()) j.SetInstanceField(self, "path", a[0]);
            return JniVal::Void();
        };
        d.methods["getAbsolutePath"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto v = j.GetInstanceField(self, "path");
            return v.AsObj() ? v : JniVal::Obj(j.NewStringUtf("/storage/rr3"));
        };
        d.methods["getPath"] = d.methods["getAbsolutePath"];
        d.methods["exists"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/ClassLoader";
        d.methods["loadClass"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (a.empty()) return JniVal::Obj(0);
            std::string nm = j.GetStringUtf(a[0].AsObj());
            uint64_t h = j.FindClassHandle(nm);
            if (!h && !nm.empty()) h = j.RegisterJniClass(JniClassDef{nm});
            return JniVal::Obj(h);
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/Iterator";
        d.methods["hasNext"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["next"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/Set";
        d.methods["iterator"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectInstance("java/util/Iterator"));
        };
        d.methods["size"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/net/Uri";
        d.static_methods["parse"] = [](FakeJvm& j, uint64_t, const auto& a) {
            uint64_t h = j.NewObjectInstance("android/net/Uri");
            if (!a.empty()) j.SetInstanceField(h, "uri", a[0]);
            return JniVal::Obj(h);
        };
        d.methods["toString"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto v = j.GetInstanceField(self, "uri");
            return v.AsObj() ? v : JniVal::Obj(j.NewStringUtf(""));
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/content/Intent";
        d.methods["setData"] = [](FakeJvm&, uint64_t self, const auto&) { return JniVal::Obj(self); };
        d.methods["setAction"] = [](FakeJvm&, uint64_t self, const auto&) { return JniVal::Obj(self); };
        d.methods["getStringExtra"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/content/Context";
        d.methods["getPackageName"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewStringUtf("com.ea.games.r3_row"));
        };
        d.methods["getApplicationContext"] = [](FakeJvm&, uint64_t self, const auto&) {
            return JniVal::Obj(self);
        };
        d.methods["getFilesDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            uint64_t f = j.NewObjectInstance("java/io/File");
            j.SetInstanceField(f, "path", JniVal::Obj(j.NewStringUtf("/storage/rr3")));
            return JniVal::Obj(f);
        };
        d.methods["getExternalFilesDir"] = d.methods["getFilesDir"];
        d.methods["getAssets"] = [&firemint](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Obj(firemint.AssetManagerHandle());
        };
        d.methods["getClassLoader"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectInstance("java/lang/ClassLoader"));
        };
        jvm.RegisterJniClass(std::move(d));
    }
    jvm.RegisterJniClass(JniClassDef{"android/app/Activity", "android/content/Context"});
    jvm.RegisterJniClass(JniClassDef{"android/content/res/AssetManager"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/CC_Activity", "android/app/Activity"});

    // ---- NetworkStatusMonitor (CRITICAL: CONNECTION_TYPE_NONE = 1, NOT 0!) ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/NetworkStatusMonitor";
        d.static_fields["CONNECTION_TYPE_NONE"] = JniVal::Int(1);
        d.static_methods["getNetworkConnectivity"] = [](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Int(1);
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- CC_Component ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/CC_Component";
        uint64_t cc_inst = jvm.NewObjectInstance("com/firemonkeys/cloudcellapi/CC_Component");
        jvm.PinHandle(cc_inst);
        d.static_methods["GetInstance"] = [cc_inst](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(cc_inst); };
        d.static_methods["GetActivity"] = [&firemint](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Obj(firemint.MainActivityHandle());
        };
        d.static_methods["getNetworkConnectivity"] = [](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Int(1); // CONNECTION_TYPE_NONE
        };
        d.static_methods["getIsUsingImmersiveMode"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["setIsUsingImmersiveMode"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["canCallSuspendResume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["startDownloadService"] = [](FakeJvm&, uint64_t, const auto&) {
            std::fprintf(stderr,
                         "[OFFLINE] RR3 requested asset download (suppressed in offline mode). "
                         "If stuck on 'Downloading assets', check that your com.ea.games.r3_row/.depot "
                         "has all game assets downloaded and matches your APK version.\n");
            return JniVal::Void();
        };
        d.static_methods["stopDownloadService"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["setServiceStarted"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["openSettings"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["staticStartActivity"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["getStaticSystemService"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        d.static_methods["RequestMissingPermissions"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["runOnGLThreadonCloudell"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- GetInfo ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/util/GetInfo";
        bool tv = cfg.android_tv_mode;
        d.static_methods["Initialize"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["IsInitialized"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetApiLevel"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(31); };
        d.static_methods["GetAdvertisingEnabled"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetAdvertisingID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetImei"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetIsTablet"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetIsRunningOnTv"] = [tv](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(tv); };
        d.static_methods["GetDeviceCode"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("dm3q")); };
        d.static_methods["GetDeviceName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung SM-S918B")); };
        d.static_methods["GetDeviceManufacturer"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung")); };
        d.static_methods["GetDeviceModel"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("SM-S918B")); };
        d.static_methods["GetDeviceBrand"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung")); };
        d.static_methods["GetDeviceFirmwareVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("12")); };
        d.static_methods["GetBuildVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("14.0.1")); };
        d.static_methods["GetDeviceMacAddress"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("00:00:00:00:00:00")); };
        d.static_methods["GetPackageName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("com.ea.games.r3_row")); };
        d.static_methods["GetIsAmazonDevice"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetIsChinaBuild"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetDeviceUID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("rr3pc-offline-0001")); };
        d.static_methods["GetLocalIp"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("127.0.0.1")); };
        d.static_methods["GetDeviceCountry"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("USA")); };
        d.static_methods["GetDeviceLanguage"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("en")); };
        d.static_methods["GetVolume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(15); };
        d.static_methods["DoesPackageExist"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsPackageEnabled"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetRequestedPermissions"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectArray(0, "java/lang/String"));
        };
        d.static_methods["getAppInstallTime"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Long(1700000000000LL); };
        d.static_methods["getExternalStorageDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewStringUtf("/storage/rr3"));
        };
        d.static_methods["getLegacyExternalStorageDirPath"] = d.static_methods["getExternalStorageDir"];
        d.static_methods["ShouldUseLegacyExternalFilesDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            uint64_t b = j.NewObjectInstance("java/lang/Boolean");
            j.SetInstanceField(b, "value", JniVal::Bool(false));
            return JniVal::Obj(b);
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- HttpRequest (offline: ZERO sockets, queues native errorCallback) ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/HttpRequest";
        d.methods["init"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            std::string url = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "";
            int64_t native_ptr = a.size() > 5 ? a[5].AsLong() : 0;
            j.SetInstanceField(self, "url", JniVal::Obj(j.NewStringUtf(url)));
            j.SetInstanceField(self, "native_ptr", JniVal::Long(native_ptr));
            j.SetInstanceField(self, "closed", JniVal::Bool(false));
            if (IsVerboseLogging()) {
                std::fprintf(stdout, "[OFFLINE] HttpRequest.init blocked: %s\n", url.c_str());
            }
            return JniVal::Void();
        };
        d.methods["addHeader"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["post"] = [&loader, &firemint](FakeJvm& j, uint64_t self, const auto&) {
            int64_t ptr = j.GetInstanceField(self, "native_ptr").AsLong();
            if (ptr != 0) {
                uint64_t err_cb = loader.FindExport("Java_com_firemonkeys_cloudcellapi_HttpRequest_errorCallback");
                if (err_cb) {
                    j.PinHandle(self);
                    firemint.PostGlCallback([&j, err_cb, self, ptr]() {
                        if (!j.GetInstanceField(self, "closed").AsBool()) {
                            j.Cpu().CallGuest(err_cb, {
                                j.GetJniEnvGuestPtr(), self,
                                static_cast<uint64_t>(ptr), static_cast<uint64_t>(-1LL)
                            });
                        }
                    });
                }
            }
            return JniVal::Void();
        };
        d.methods["close"] = [](FakeJvm& j, uint64_t self, const auto&) {
            j.SetInstanceField(self, "closed", JniVal::Bool(true));
            return JniVal::Void();
        };
        d.methods["shutdown"] = d.methods["close"];
        d.methods["isClosed"] = [](FakeJvm& j, uint64_t self, const auto&) {
            return j.GetInstanceField(self, "closed");
        };
        d.methods["getSSLCheck"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["setClosedBySSLCheck"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["getServerTime"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Double(0.0); };
        d.methods["getTimeoutMilliseconds"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(5000); };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- Cloudcell Logging ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/Logging";
        auto log_fn = [](const char* lvl) -> JniMethodFn {
            return [lvl](FakeJvm& j, uint64_t, const auto& a) {
                std::string tag = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "CC";
                std::string msg = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "";
                if (IsVerboseLogging()) {
                    std::fprintf(stdout, "[CC_%s] %s: %s\n", lvl, tag.c_str(), msg.c_str());
                }
                return JniVal::Void();
            };
        };
        d.static_methods["CC_INFO"] = log_fn("INFO");
        d.static_methods["CC_ERROR"] = log_fn("ERR");
        d.static_methods["CC_WARN"] = log_fn("WARN");
        d.static_methods["CC_TRACE"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- AndroidAccountManager / GooglePlayWorker / FacebookWorker / AssetManagerService / Prompts / UI ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/AndroidAccountManager";
        uint64_t inst = jvm.NewObjectInstance("com/firemonkeys/cloudcellapi/AndroidAccountManager");
        jvm.PinHandle(inst);
        d.static_methods["getInstance"] = [inst](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(inst); };
        d.methods["isLoggedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["getAccountName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.methods["getAccountType"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.methods["login"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["logout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["onResume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/GooglePlayWorker";
        d.static_methods["isSignedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isGooglePlayServicesAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsInitialPhaseComplete"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetSessionChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsSignedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["Constructor"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["IsInitialPhaseComplete"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["GetSessionChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["GetSessionValid"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["GetCurrentPlayer"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        d.static_methods["SignIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SignOut"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["signIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["signOut"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["onStart"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["onStop"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/FacebookWorker";
        d.static_methods["isLoggedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["login"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["logout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/FacebookWorker$AvatarInfo"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/AndroidAssetManagerService"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/AppPromptManager"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/UserInterfaceManager"});

    // ---- Nimble / Push / GluAnalytics / Ads / Notifications (offline no-op stubs) ----
    const char* kStubClasses[] = {
        "com/ea/nimble/bridge/NimbleCppComponentRegistrar",
        "com/ea/nimble/INimbleLocalNotifications",
        "com/ea/nimble/NimbleLocalNotifications",
        "com/ea/nimble/pushtng/IPushNotification",
        "com/ea/nimble/pushtng/PushNotification",
        "com/firemint/realracing/GoogleNativeAdManager",
        "com/firemint/realracing/LocalNotificationsCenter",
        "com/glu/plugins/gluanalytics/AnalyticsFactory",
        "com/glu/plugins/gluanalytics/GeoLocation",
        "com/glu/plugins/gluanalytics/PerfMeasure",
        "csdk/glucentralservices/util/AndroidPlatform",
        "csdk/glucentralservices/eventbus/ServerEventBus",
        "csdk/gluads/GluAdsNativeBridge",
        "csdk/gluads/Reward",
        "csdk/glucustomersupport/NativeBridge",
        "csdk/glucustomersupport/Consts",
    };
    for (const char* nm : kStubClasses) {
        JniClassDef d;
        d.name = nm;
        if (std::string(nm) == "com/ea/nimble/bridge/NimbleCppComponentRegistrar") {
            d.static_methods["register"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        } else if (std::string(nm) == "com/firemint/realracing/LocalNotificationsCenter") {
            auto noop = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.static_methods["showNotification"] = noop;
            d.static_methods["CancelAllNotifications"] = noop;
            d.static_methods["CancelNotification"] = noop;
            d.static_methods["SaveNotifications"] = noop;
            d.static_methods["LoadNotifications"] = noop;
        } else if (std::string(nm) == "csdk/glucentralservices/util/AndroidPlatform") {
            d.methods["getGpuDeviceVendor"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("AMD")); };
            d.methods["getGpuDeviceName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("Radeon RX 580")); };
            d.methods["sendHTTPRequest"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.methods["scheduleEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.methods["downloadFile"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        }
        jvm.RegisterJniClass(std::move(d));
    }
}
