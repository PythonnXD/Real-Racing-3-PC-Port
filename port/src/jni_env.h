#pragma once

#include "cpu_dynarmic.h"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class FakeJvm;

struct JniVal {
    uint64_t u64 = 0;
    double f64 = 0.0;
    float f32 = 0.0f;

    static JniVal Void() { return {}; }
    static JniVal Bool(bool v) { JniVal r; r.u64 = v ? 1 : 0; return r; }
    static JniVal Int(int32_t v) { JniVal r; r.u64 = static_cast<uint64_t>(static_cast<int64_t>(v)); return r; }
    static JniVal Long(int64_t v) { JniVal r; r.u64 = static_cast<uint64_t>(v); return r; }
    static JniVal Float(float v) { JniVal r; r.f32 = v; r.f64 = v; return r; }
    static JniVal Double(double v) { JniVal r; r.f64 = v; r.f32 = static_cast<float>(v); return r; }
    static JniVal Obj(uint64_t handle) { JniVal r; r.u64 = handle; return r; }

    bool AsBool() const { return u64 != 0; }
    int32_t AsInt() const { return static_cast<int32_t>(u64); }
    int64_t AsLong() const { return static_cast<int64_t>(u64); }
    float AsFloat() const { return f32; }
    double AsDouble() const { return f64; }
    uint64_t AsObj() const { return u64; }
};

using JniMethodFn = std::function<JniVal(FakeJvm&, uint64_t self_or_cls, const std::vector<JniVal>& args)>;

struct JniClassDef {
    std::string name;       // e.g. "com/firemint/realracing/Platform"
    std::string super_name; // optional superclass, default "java/lang/Object"

    // Key: "methodName:signature" (or "methodName" wildcard fallback if signature omitted)
    std::unordered_map<std::string, JniMethodFn> methods;
    std::unordered_map<std::string, JniMethodFn> static_methods;

    // Static fields: name -> value
    std::unordered_map<std::string, JniVal> static_fields;
    // Default instance field values: name -> initial value
    std::unordered_map<std::string, JniVal> default_fields;

    // Optional factory for AllocObject / NewObject
    std::function<std::any()> create_native_state;
};

struct JniObject {
    enum class Kind {
        Class,
        Instance,
        String,
        BoolArray,
        ByteArray,
        CharArray,
        ShortArray,
        IntArray,
        LongArray,
        FloatArray,
        DoubleArray,
        ObjectArray,
        DirectBuffer,
    };

    uint64_t handle = 0;
    Kind kind = Kind::Instance;
    std::string class_name;
    uint32_t local_refs = 1;
    uint32_t global_refs = 0;
    bool pinned = false;

    // Instance fields & arbitrary C++ payload (e.g. DirectWrite font handle, bitmap, etc.)
    std::unordered_map<std::string, JniVal> fields;
    std::any native_state;

    // String storage
    std::string utf8;
    std::vector<uint16_t> utf16;

    // Array / DirectBuffer storage
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> obj_elems;
    std::string array_elem_class;
    void* external_direct_ptr = nullptr;
    size_t direct_capacity = 0;
};

class FakeJvm {
public:
    explicit FakeJvm(CpuRuntime& cpu);
    ~FakeJvm();

    // Guest-readable pointers to JavaVM* and JNIEnv* (double-indirected per JNI spec).
    uint64_t GetJavaVmGuestPtr() const { return java_vm_ptr_; }
    uint64_t GetJniEnvGuestPtr() const { return jni_env_ptr_; }
    CpuRuntime& Cpu() { return cpu_; }

    // Class registration (also creates pinned jclass handle).
    uint64_t RegisterJniClass(JniClassDef def);
    JniClassDef* FindClassDef(const std::string& name);
    uint64_t FindClassHandle(const std::string& name);

    // Object / String / Array / DirectByteBuffer helpers for host C++ shims.
    uint64_t NewObjectInstance(const std::string& class_name, std::any native_state = {});
    uint64_t NewStringUtf(const std::string& utf8);
    std::string GetStringUtf(uint64_t jstr);
    uint64_t NewByteArray(const void* data, size_t len);
    uint64_t NewIntArray(const int32_t* data, size_t len);
    uint64_t NewFloatArray(const float* data, size_t len);
    uint64_t NewObjectArray(size_t len, const std::string& elem_class, uint64_t init_elem = 0);
    uint64_t NewDirectByteBuffer(void* host_ptr, size_t capacity, bool take_copy = false);
    uint64_t NewDirectByteBufferOwned(std::vector<uint8_t> data);

    std::shared_ptr<JniObject> GetJniObject(uint64_t handle);
    void PinHandle(uint64_t handle);

    // Field helpers for C++ shims.
    JniVal GetInstanceField(uint64_t obj, const std::string& field_name);
    void SetInstanceField(uint64_t obj, const std::string& field_name, JniVal val);
    JniVal GetStaticField(const std::string& class_name, const std::string& field_name);
    void SetStaticField(const std::string& class_name, const std::string& field_name, JniVal val);

    // Lookup functions registered via JNI RegisterNatives.
    uint64_t FindRegisteredNative(const std::string& class_name, const std::string& method_name,
                                  const std::string& sig = "") const;

    // Exception state (per-thread in JNI).
    void ThrowException(const std::string& class_name, const std::string& message);
    bool HasException() const;
    void ClearException();

    // Parse JNI method signature "(params)ret" -> vector of single-char descriptors ('Z','B','C','S','I','J','F','D','L').
    static std::vector<char> ParseParamTypes(const std::string& sig);
    static char ParseReturnType(const std::string& sig);

private:
    struct MethodIdInfo {
        std::string class_name;
        std::string name;
        std::string sig;
        bool is_static = false;
        std::vector<char> param_types;
        char ret_type = 'V';
        JniMethodFn fn;
    };

    struct FieldIdInfo {
        std::string class_name;
        std::string name;
        std::string sig;
        bool is_static = false;
    };

    void InitBuiltinClasses();
    void InitTables();

    uint64_t AllocHandle(std::shared_ptr<JniObject> obj);
    uint64_t ResolveMethodId(uint64_t cls_handle, const char* name, const char* sig, bool is_static, uint64_t caller_pc);
    uint64_t ResolveFieldId(uint64_t cls_handle, const char* name, const char* sig, bool is_static, uint64_t caller_pc);

    std::vector<JniVal> ExtractArgsVariadic(CpuContext& c, const std::vector<char>& types, int first_gp);
    std::vector<JniVal> ExtractArgsVaList(CpuContext& c, const std::vector<char>& types, uint64_t valist_addr);
    std::vector<JniVal> ExtractArgsArray(const std::vector<char>& types, uint64_t jvalues_addr);
    void DispatchMethodCall(CpuContext& c, uint64_t target, uint64_t mid,
                            const std::vector<JniVal>& args, char expected_ret);
    void WriteReturnValue(CpuContext& c, const JniVal& v, char ret_type);

    CpuRuntime& cpu_;

    // Guest-readable JNI tables (236 slots for JNINativeInterface + 8 slots for JNIInvokeInterface)
    uint64_t* env_vtable_ = nullptr;
    uint64_t* vm_vtable_ = nullptr;
    uint64_t* env_instance_ = nullptr; // points to env_vtable_
    uint64_t* vm_instance_ = nullptr;  // points to vm_vtable_
    uint64_t jni_env_ptr_ = 0;
    uint64_t java_vm_ptr_ = 0;

    mutable std::mutex mtx_;
    uint64_t next_handle_ = 0x10000001ULL;
    uint64_t next_mid_ = 0x20000001ULL;
    uint64_t next_fid_ = 0x30000001ULL;

    std::unordered_map<uint64_t, std::shared_ptr<JniObject>> objects_;
    std::unordered_map<std::string, JniClassDef> classes_;
    std::unordered_map<std::string, uint64_t> class_handles_;

    std::unordered_map<uint64_t, MethodIdInfo> method_ids_;
    std::unordered_map<std::string, uint64_t> method_id_cache_;

    std::unordered_map<uint64_t, FieldIdInfo> field_ids_;
    std::unordered_map<std::string, uint64_t> field_id_cache_;

    // Native methods registered via RegisterNatives: "class:name:sig" -> guest fn ptr
    std::unordered_map<std::string, uint64_t> registered_natives_;
};
