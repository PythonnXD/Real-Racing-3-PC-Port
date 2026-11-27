#include "jni_env.h"
#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr int32_t kJniOk = 0;
constexpr int32_t kJniErr = -1;
constexpr int32_t kJniVersion16 = 0x00010006;

struct Aarch64VaList {
    uint64_t stack;
    uint64_t gr_top;
    uint64_t vr_top;
    int32_t gr_offs;
    int32_t vr_offs;
};

struct GuestJniNativeMethod {
    uint64_t name_ptr;
    uint64_t sig_ptr;
    uint64_t fn_ptr;
};

// Per-thread JNI exception + local frame stack
struct JniThreadState {
    uint64_t pending_exception = 0;
    std::vector<std::vector<uint64_t>> local_frames;
};

thread_local JniThreadState tls_jni;

std::string NormalizeClassName(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    for (char& ch : s) {
        if (ch == '.') ch = '/';
    }
    if (s.size() >= 2 && s.front() == 'L' && s.back() == ';') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::vector<uint16_t> Utf8ToUtf16(const std::string& utf8) {
    std::vector<uint16_t> out;
    if (utf8.empty()) return out;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return out;
    out.resize(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        reinterpret_cast<wchar_t*>(out.data()), wlen);
    return out;
}

std::string Utf16ToUtf8(const uint16_t* data, size_t len) {
    if (!data || len == 0) return {};
    int ulen = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(data),
                                   static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return {};
    std::string out(static_cast<size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(data),
                        static_cast<int>(len), out.data(), ulen, nullptr, nullptr);
    return out;
}

size_t ElemSizeForArrayKind(JniObject::Kind k) {
    switch (k) {
        case JniObject::Kind::BoolArray:
        case JniObject::Kind::ByteArray: return 1;
        case JniObject::Kind::CharArray:
        case JniObject::Kind::ShortArray: return 2;
        case JniObject::Kind::IntArray:
        case JniObject::Kind::FloatArray: return 4;
        case JniObject::Kind::LongArray:
        case JniObject::Kind::DoubleArray: return 8;
        default: return 0;
    }
}

} // namespace

FakeJvm::FakeJvm(CpuRuntime& cpu) : cpu_(cpu) {
    // Allocate guest-readable JNI tables via VirtualAlloc so pointers remain fixed for process lifetime.
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto* slots = static_cast<uint64_t*>(mem);
    env_vtable_ = slots;          // slots 0..235 (1888 bytes)
    vm_vtable_ = slots + 256;     // slots 0..7   (64 bytes)
    env_instance_ = slots + 280;  // *env_instance_ = env_vtable_
    vm_instance_ = slots + 282;   // *vm_instance_  = vm_vtable_

    *env_instance_ = reinterpret_cast<uint64_t>(env_vtable_);
    *vm_instance_ = reinterpret_cast<uint64_t>(vm_vtable_);
    jni_env_ptr_ = reinterpret_cast<uint64_t>(env_instance_);
    java_vm_ptr_ = reinterpret_cast<uint64_t>(vm_instance_);

    InitTables();
    InitBuiltinClasses();
}

FakeJvm::~FakeJvm() {
    if (env_vtable_) {
        VirtualFree(env_vtable_, 0, MEM_RELEASE);
        env_vtable_ = nullptr;
    }
}

std::vector<char> FakeJvm::ParseParamTypes(const std::string& sig) {
    std::vector<char> out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    ++i;
    while (i < sig.size() && sig[i] != ')') {
        char ch = sig[i];
        if (ch == 'L') {
            out.push_back('L');
            size_t semi = sig.find(';', i);
            i = (semi == std::string::npos) ? sig.size() : (semi + 1);
        } else if (ch == '[') {
            out.push_back('L'); // arrays are object references in JNI
            while (i < sig.size() && sig[i] == '[') ++i;
            if (i < sig.size()) {
                if (sig[i] == 'L') {
                    size_t semi = sig.find(';', i);
                    i = (semi == std::string::npos) ? sig.size() : (semi + 1);
                } else {
                    ++i;
                }
            }
        } else {
            out.push_back(ch);
            ++i;
        }
    }
    return out;
}

char FakeJvm::ParseReturnType(const std::string& sig) {
    size_t rp = sig.find(')');
    if (rp == std::string::npos || rp + 1 >= sig.size()) return 'V';
    char ch = sig[rp + 1];
    return (ch == '[') ? 'L' : ch;
}

uint64_t FakeJvm::AllocHandle(std::shared_ptr<JniObject> obj) {
    std::lock_guard<std::mutex> lk(mtx_);
    uint64_t h = next_handle_++;
    obj->handle = h;
    objects_[h] = std::move(obj);
    if (!tls_jni.local_frames.empty()) {
        tls_jni.local_frames.back().push_back(h);
    }
    return h;
}

std::shared_ptr<JniObject> FakeJvm::GetJniObject(uint64_t handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objects_.find(handle);
    return it != objects_.end() ? it->second : nullptr;
}

void FakeJvm::PinHandle(uint64_t handle) {
    if (auto o = GetJniObject(handle)) {
        o->pinned = true;
    }
}

uint64_t FakeJvm::RegisterJniClass(JniClassDef def) {
    def.name = NormalizeClassName(def.name.c_str());
    if (def.super_name.empty() && def.name != "java/lang/Object") {
        def.super_name = "java/lang/Object";
    }
    std::lock_guard<std::mutex> lk(mtx_);
    std::string name = def.name;
    classes_[name] = std::move(def);

    auto it = class_handles_.find(name);
    if (it != class_handles_.end()) {
        return it->second;
    }
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::Class;
    obj->class_name = name;
    obj->pinned = true;
    obj->global_refs = 1;
    uint64_t h = next_handle_++;
    obj->handle = h;
    objects_[h] = obj;
    class_handles_[name] = h;
    return h;
}

JniClassDef* FakeJvm::FindClassDef(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = classes_.find(NormalizeClassName(name.c_str()));
    return it != classes_.end() ? &it->second : nullptr;
}

uint64_t FakeJvm::FindClassHandle(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = class_handles_.find(NormalizeClassName(name.c_str()));
    return it != class_handles_.end() ? it->second : 0;
}

uint64_t FakeJvm::NewObjectInstance(const std::string& class_name, std::any native_state) {
    std::string norm = NormalizeClassName(class_name.c_str());
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::Instance;
    obj->class_name = norm;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = classes_.find(norm);
        if (it != classes_.end()) {
            obj->fields = it->second.default_fields;
            if (!native_state.has_value() && it->second.create_native_state) {
                native_state = it->second.create_native_state();
            }
        }
    }
    obj->native_state = std::move(native_state);
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewStringUtf(const std::string& utf8) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::String;
    obj->class_name = "java/lang/String";
    obj->utf8 = utf8;
    obj->utf16 = Utf8ToUtf16(utf8);
    return AllocHandle(std::move(obj));
}

std::string FakeJvm::GetStringUtf(uint64_t jstr) {
    auto o = GetJniObject(jstr);
    if (!o || o->kind != JniObject::Kind::String) return {};
    return o->utf8;
}

uint64_t FakeJvm::NewByteArray(const void* data, size_t len) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::ByteArray;
    obj->class_name = "[B";
    obj->bytes.resize(len);
    if (data && len) std::memcpy(obj->bytes.data(), data, len);
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewIntArray(const int32_t* data, size_t len) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::IntArray;
    obj->class_name = "[I";
    obj->bytes.resize(len * 4);
    if (data && len) std::memcpy(obj->bytes.data(), data, len * 4);
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewFloatArray(const float* data, size_t len) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::FloatArray;
    obj->class_name = "[F";
    obj->bytes.resize(len * 4);
    if (data && len) std::memcpy(obj->bytes.data(), data, len * 4);
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewObjectArray(size_t len, const std::string& elem_class, uint64_t init_elem) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::ObjectArray;
    obj->class_name = "[L" + elem_class + ";";
    obj->array_elem_class = elem_class;
    obj->obj_elems.assign(len, init_elem);
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewDirectByteBuffer(void* host_ptr, size_t capacity, bool take_copy) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::DirectBuffer;
    obj->class_name = "java/nio/DirectByteBuffer";
    if (take_copy) {
        obj->bytes.resize(capacity);
        if (host_ptr && capacity) std::memcpy(obj->bytes.data(), host_ptr, capacity);
        obj->external_direct_ptr = nullptr;
        obj->direct_capacity = capacity;
    } else {
        obj->external_direct_ptr = host_ptr;
        obj->direct_capacity = capacity;
    }
    return AllocHandle(std::move(obj));
}

uint64_t FakeJvm::NewDirectByteBufferOwned(std::vector<uint8_t> data) {
    auto obj = std::make_shared<JniObject>();
    obj->kind = JniObject::Kind::DirectBuffer;
    obj->class_name = "java/nio/DirectByteBuffer";
    obj->direct_capacity = data.size();
    obj->bytes = std::move(data);
    return AllocHandle(std::move(obj));
}

JniVal FakeJvm::GetInstanceField(uint64_t obj_h, const std::string& field_name) {
    auto o = GetJniObject(obj_h);
    if (!o) return {};
    auto it = o->fields.find(field_name);
    return it != o->fields.end() ? it->second : JniVal{};
}

void FakeJvm::SetInstanceField(uint64_t obj_h, const std::string& field_name, JniVal val) {
    if (auto o = GetJniObject(obj_h)) {
        o->fields[field_name] = val;
    }
}

JniVal FakeJvm::GetStaticField(const std::string& class_name, const std::string& field_name) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string cur = NormalizeClassName(class_name.c_str());
    while (!cur.empty()) {
        auto it = classes_.find(cur);
        if (it == classes_.end()) break;
        auto fit = it->second.static_fields.find(field_name);
        if (fit != it->second.static_fields.end()) return fit->second;
        cur = it->second.super_name;
    }
    return {};
}

void FakeJvm::SetStaticField(const std::string& class_name, const std::string& field_name, JniVal val) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = classes_.find(NormalizeClassName(class_name.c_str()));
    if (it != classes_.end()) {
        it->second.static_fields[field_name] = val;
    }
}

uint64_t FakeJvm::FindRegisteredNative(const std::string& class_name, const std::string& method_name,
                                       const std::string& sig) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string norm = NormalizeClassName(class_name.c_str());
    if (!sig.empty()) {
        auto it = registered_natives_.find(norm + ":" + method_name + ":" + sig);
        if (it != registered_natives_.end()) return it->second;
    }
    auto it2 = registered_natives_.find(norm + ":" + method_name);
    return it2 != registered_natives_.end() ? it2->second : 0;
}

void FakeJvm::ThrowException(const std::string& class_name, const std::string& message) {
    uint64_t ex = NewObjectInstance(class_name);
    SetInstanceField(ex, "message", JniVal::Obj(NewStringUtf(message)));
    tls_jni.pending_exception = ex;
}

bool FakeJvm::HasException() const {
    return tls_jni.pending_exception != 0;
}

void FakeJvm::ClearException() {
    tls_jni.pending_exception = 0;
}

uint64_t FakeJvm::ResolveMethodId(uint64_t cls_handle, const char* name, const char* sig,
                                  bool is_static, uint64_t caller_pc) {
    auto cls_obj = GetJniObject(cls_handle);
    if (!cls_obj || !name || !sig) return 0;

    std::string cls_name = cls_obj->class_name;
    std::string mname(name);
    std::string msig(sig);
    std::string cache_key = (is_static ? "S:" : "I:") + cls_name + ":" + mname + ":" + msig;

    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = method_id_cache_.find(cache_key); it != method_id_cache_.end()) {
        return it->second;
    }

    // Walk class hierarchy for registered method implementation
    JniMethodFn found_fn;
    std::string cur = cls_name;
    while (!cur.empty()) {
        auto cit = classes_.find(cur);
        if (cit == classes_.end()) break;
        const auto& tbl = is_static ? cit->second.static_methods : cit->second.methods;
        if (auto mit = tbl.find(mname + ":" + msig); mit != tbl.end()) {
            found_fn = mit->second;
            break;
        }
        if (auto mit = tbl.find(mname); mit != tbl.end()) {
            found_fn = mit->second;
            break;
        }
        cur = cit->second.super_name;
    }

    if (!found_fn && mname == "<init>") {
        // Default constructor no-op if class didn't register explicit <init>
        found_fn = [](FakeJvm&, uint64_t, const std::vector<JniVal>&) { return JniVal::Void(); };
    }

    if (!found_fn) {
        if (IsVerboseLogging()) {
            std::fprintf(stderr, "[JNI WARN] Missing %s method: %s.%s %s (caller=0x%llx)\n",
                         is_static ? "static" : "instance", cls_name.c_str(), mname.c_str(), msig.c_str(),
                         static_cast<unsigned long long>(caller_pc));
        }
        // Still issue a stub MethodID that logs when invoked (prevents immediate null-mid crashes on optional hooks).
        std::string full = cls_name + "." + mname + msig;
        found_fn = [full](FakeJvm&, uint64_t, const std::vector<JniVal>&) -> JniVal {
            if (IsVerboseLogging()) {
                std::fprintf(stderr, "[JNI STUB CALL] %s -> returning 0/null\n", full.c_str());
            }
            return JniVal{};
        };
    }

    MethodIdInfo info;
    info.class_name = cls_name;
    info.name = mname;
    info.sig = msig;
    info.is_static = is_static;
    info.param_types = ParseParamTypes(msig);
    info.ret_type = ParseReturnType(msig);
    info.fn = std::move(found_fn);

    uint64_t mid = next_mid_++;
    method_ids_[mid] = std::move(info);
    method_id_cache_[cache_key] = mid;
    return mid;
}

uint64_t FakeJvm::ResolveFieldId(uint64_t cls_handle, const char* name, const char* sig,
                                 bool is_static, uint64_t caller_pc) {
    auto cls_obj = GetJniObject(cls_handle);
    if (!cls_obj || !name || !sig) return 0;

    std::string cls_name = cls_obj->class_name;
    std::string fname(name);
    std::string fsig(sig);
    std::string cache_key = (is_static ? "SF:" : "IF:") + cls_name + ":" + fname + ":" + fsig;

    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = field_id_cache_.find(cache_key); it != field_id_cache_.end()) {
        return it->second;
    }

    bool known = false;
    std::string cur = cls_name;
    while (!cur.empty()) {
        auto cit = classes_.find(cur);
        if (cit == classes_.end()) break;
        if (is_static) {
            if (cit->second.static_fields.count(fname)) { known = true; break; }
        } else {
            if (cit->second.default_fields.count(fname)) { known = true; break; }
        }
        cur = cit->second.super_name;
    }
    if (!known && IsVerboseLogging()) {
        std::fprintf(stderr, "[JNI INFO] Dynamic %s field lookup: %s.%s (%s) caller=0x%llx\n",
                     is_static ? "static" : "instance", cls_name.c_str(), fname.c_str(), fsig.c_str(),
                     static_cast<unsigned long long>(caller_pc));
    }

    FieldIdInfo info{cls_name, fname, fsig, is_static};
    uint64_t fid = next_fid_++;
    field_ids_[fid] = std::move(info);
    field_id_cache_[cache_key] = fid;
    return fid;
}

std::vector<JniVal> FakeJvm::ExtractArgsVariadic(CpuContext& c, const std::vector<char>& types, int first_gp) {
    std::vector<JniVal> args;
    args.reserve(types.size());
    int gp = first_gp;
    int fp = 0;
    size_t stack_slot = 0;

    for (char t : types) {
        JniVal v{};
        if (t == 'F' || t == 'D') {
            double d = 0.0;
            if (fp < 8) {
                d = c.GetD(fp++);
            } else {
                uint64_t raw = c.ReadStackU64(stack_slot++);
                std::memcpy(&d, &raw, 8);
            }
            v.f64 = d;
            v.f32 = static_cast<float>(d);
        } else {
            uint64_t raw = (gp < 8) ? c.GetX(gp++) : c.ReadStackU64(stack_slot++);
            if (t == 'Z') v.u64 = (raw & 0xFF) ? 1 : 0;
            else if (t == 'B') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw)));
            else if (t == 'C') v.u64 = raw & 0xFFFF;
            else if (t == 'S') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw)));
            else if (t == 'I') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw)));
            else v.u64 = raw; // J, L, [
        }
        args.push_back(v);
    }
    return args;
}

std::vector<JniVal> FakeJvm::ExtractArgsVaList(CpuContext&, const std::vector<char>& types, uint64_t valist_addr) {
    std::vector<JniVal> args;
    if (!valist_addr) return args;
    Aarch64VaList vl = *reinterpret_cast<const Aarch64VaList*>(static_cast<uintptr_t>(valist_addr));
    args.reserve(types.size());

    for (char t : types) {
        JniVal v{};
        if (t == 'F' || t == 'D') {
            double d = 0.0;
            if (vl.vr_offs < 0) {
                int32_t off = vl.vr_offs;
                vl.vr_offs += 16;
                if (vl.vr_offs <= 0) {
                    d = *reinterpret_cast<const double*>(static_cast<uintptr_t>(vl.vr_top + off));
                } else {
                    d = *reinterpret_cast<const double*>(static_cast<uintptr_t>(vl.stack));
                    vl.stack += 8;
                }
            } else {
                d = *reinterpret_cast<const double*>(static_cast<uintptr_t>(vl.stack));
                vl.stack += 8;
            }
            v.f64 = d;
            v.f32 = static_cast<float>(d);
        } else {
            uint64_t raw = 0;
            if (vl.gr_offs < 0) {
                int32_t off = vl.gr_offs;
                vl.gr_offs += 8;
                if (vl.gr_offs <= 0) {
                    raw = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(vl.gr_top + off));
                } else {
                    raw = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(vl.stack));
                    vl.stack += 8;
                }
            } else {
                raw = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(vl.stack));
                vl.stack += 8;
            }
            if (t == 'Z') v.u64 = (raw & 0xFF) ? 1 : 0;
            else if (t == 'B') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw)));
            else if (t == 'C') v.u64 = raw & 0xFFFF;
            else if (t == 'S') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw)));
            else if (t == 'I') v.u64 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw)));
            else v.u64 = raw;
        }
        args.push_back(v);
    }
    return args;
}

std::vector<JniVal> FakeJvm::ExtractArgsArray(const std::vector<char>& types, uint64_t jvalues_addr) {
    std::vector<JniVal> args;
    args.reserve(types.size());
    const auto* vals = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(jvalues_addr));
    for (size_t i = 0; i < types.size(); ++i) {
        JniVal v{};
        uint64_t raw = vals ? vals[i] : 0;
        char t = types[i];
        if (t == 'F') {
            uint32_t lo = static_cast<uint32_t>(raw & 0xFFFFFFFFULL);
            std::memcpy(&v.f32, &lo, 4);
            v.f64 = v.f32;
        } else if (t == 'D') {
            std::memcpy(&v.f64, &raw, 8);
            v.f32 = static_cast<float>(v.f64);
        } else {
            v.u64 = raw;
        }
        args.push_back(v);
    }
    return args;
}

void FakeJvm::WriteReturnValue(CpuContext& c, const JniVal& v, char ret_type) {
    switch (ret_type) {
        case 'V': break;
        case 'F': c.SetS(0, v.f32); break;
        case 'D': c.SetD(0, v.f64); break;
        case 'Z': c.SetW(0, v.u64 ? 1u : 0u); break;
        case 'B': c.SetW(0, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v.u64)))); break;
        case 'C': c.SetW(0, static_cast<uint32_t>(v.u64 & 0xFFFF)); break;
        case 'S': c.SetW(0, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v.u64)))); break;
        case 'I': c.SetW(0, static_cast<uint32_t>(v.u64)); break;
        default:  c.SetX(0, v.u64); break; // J, L, [
    }
}

void FakeJvm::DispatchMethodCall(CpuContext& c, uint64_t target, uint64_t mid,
                                 const std::vector<JniVal>& args, char expected_ret) {
    JniMethodFn fn;
    char ret_type = expected_ret;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = method_ids_.find(mid);
        if (it != method_ids_.end()) {
            fn = it->second.fn;
            if (ret_type == '?') ret_type = it->second.ret_type;
        }
    }
    JniVal res{};
    if (fn) {
        res = fn(*this, target, args);
    }
    WriteReturnValue(c, res, ret_type);
}

void FakeJvm::InitTables() {
    auto set_env = [&](int idx, const char* name, SvcHandlerFn handler) {
        env_vtable_[idx] = cpu_.AllocateSvcStub(std::string("JNI::") + name, std::move(handler));
    };
    auto set_vm = [&](int idx, const char* name, SvcHandlerFn handler) {
        vm_vtable_[idx] = cpu_.AllocateSvcStub(std::string("JavaVM::") + name, std::move(handler));
    };

    // Default fallback for every slot so no jump to 0 ever occurs silently
    for (int i = 0; i < 236; ++i) {
        env_vtable_[i] = cpu_.AllocateSvcStub("JNI::unimpl_" + std::to_string(i), [i](CpuContext& c) {
            std::fprintf(stderr, "[JNI FATAL] Unimplemented JNINativeInterface slot %d (LR=0x%llx)\n",
                         i, static_cast<unsigned long long>(c.GetLR()));
            c.SetX(0, 0);
        });
    }
    for (int i = 0; i < 8; ++i) {
        vm_vtable_[i] = 0;
    }

    // ---- JNIInvokeInterface (JavaVM, slots 3..7) ----
    set_vm(3, "DestroyJavaVM", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_vm(4, "AttachCurrentThread", [this](CpuContext& c) {
        auto* out_env = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (out_env) *out_env = jni_env_ptr_;
        c.SetW(0, kJniOk);
    });
    set_vm(5, "DetachCurrentThread", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_vm(6, "GetEnv", [this](CpuContext& c) {
        auto* out_env = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (out_env) *out_env = jni_env_ptr_;
        c.SetW(0, kJniOk);
    });
    set_vm(7, "AttachCurrentThreadAsDaemon", [this](CpuContext& c) {
        auto* out_env = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (out_env) *out_env = jni_env_ptr_;
        c.SetW(0, kJniOk);
    });

    // ---- JNINativeInterface (exact JDK/NDK indices 4..235) ----
    set_env(4, "GetVersion", [](CpuContext& c) { c.SetW(0, kJniVersion16); });
    set_env(5, "DefineClass", [](CpuContext& c) { c.SetX(0, 0); });
    set_env(6, "FindClass", [this](CpuContext& c) {
        const char* raw = reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(1)));
        std::string norm = NormalizeClassName(raw);
        uint64_t h = FindClassHandle(norm);
        if (!h) {
            if (IsVerboseLogging()) {
                std::fprintf(stderr, "[JNI WARN] FindClass('%s') NOT FOUND (caller=0x%llx)\n",
                             norm.c_str(), static_cast<unsigned long long>(c.GetLR()));
            }
            ThrowException("java/lang/NoClassDefFoundError", norm);
        }
        c.SetX(0, h);
    });
    set_env(10, "GetSuperclass", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o) { c.SetX(0, 0); return; }
        auto* def = FindClassDef(o->class_name);
        c.SetX(0, (def && !def->super_name.empty()) ? FindClassHandle(def->super_name) : 0);
    });
    set_env(11, "IsAssignableFrom", [this](CpuContext& c) {
        auto sub = GetJniObject(c.GetX(1));
        auto sup = GetJniObject(c.GetX(2));
        if (!sub || !sup) { c.SetW(0, 0); return; }
        if (sup->class_name == "java/lang/Object" || sub->class_name == sup->class_name) {
            c.SetW(0, 1); return;
        }
        std::string cur = sub->class_name;
        while (!cur.empty()) {
            if (cur == sup->class_name) { c.SetW(0, 1); return; }
            auto* def = FindClassDef(cur);
            cur = def ? def->super_name : "";
        }
        c.SetW(0, 0);
    });

    // Exceptions (13..18, 228)
    set_env(13, "Throw", [](CpuContext& c) { tls_jni.pending_exception = c.GetX(1); c.SetW(0, 0); });
    set_env(14, "ThrowNew", [this](CpuContext& c) {
        auto cls = GetJniObject(c.GetX(1));
        const char* msg = reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(2)));
        ThrowException(cls ? cls->class_name : "java/lang/Exception", msg ? msg : "");
        c.SetW(0, 0);
    });
    set_env(15, "ExceptionOccurred", [](CpuContext& c) { c.SetX(0, tls_jni.pending_exception); });
    set_env(16, "ExceptionDescribe", [this](CpuContext&) {
        if (tls_jni.pending_exception) {
            auto ex = GetJniObject(tls_jni.pending_exception);
            std::string msg = GetStringUtf(GetInstanceField(tls_jni.pending_exception, "message").AsObj());
            std::fprintf(stderr, "[JNI EXCEPTION] %s: %s\n",
                         ex ? ex->class_name.c_str() : "?", msg.c_str());
        }
    });
    set_env(17, "ExceptionClear", [](CpuContext&) { tls_jni.pending_exception = 0; });
    set_env(18, "FatalError", [](CpuContext& c) {
        const char* msg = reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(1)));
        std::fprintf(stderr, "[JNI FATAL ERROR] %s\n", msg ? msg : "(null)");
    });
    set_env(228, "ExceptionCheck", [](CpuContext& c) { c.SetW(0, tls_jni.pending_exception ? 1u : 0u); });

    // Local/Global Refs (19..26, 226..227, 232)
    set_env(19, "PushLocalFrame", [](CpuContext& c) {
        tls_jni.local_frames.emplace_back();
        c.SetW(0, kJniOk);
    });
    set_env(20, "PopLocalFrame", [this](CpuContext& c) {
        uint64_t keep = c.GetX(1);
        if (!tls_jni.local_frames.empty()) {
            auto frame = std::move(tls_jni.local_frames.back());
            tls_jni.local_frames.pop_back();
            std::lock_guard<std::mutex> lk(mtx_);
            for (uint64_t h : frame) {
                if (h == keep) continue;
                auto it = objects_.find(h);
                if (it != objects_.end() && !it->second->pinned && it->second->global_refs == 0) {
                    if (it->second->local_refs > 0) --it->second->local_refs;
                    if (it->second->local_refs == 0) objects_.erase(it);
                }
            }
            if (keep && !tls_jni.local_frames.empty()) {
                tls_jni.local_frames.back().push_back(keep);
            }
        }
        c.SetX(0, keep);
    });
    set_env(21, "NewGlobalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        if (auto o = GetJniObject(h)) {
            std::lock_guard<std::mutex> lk(mtx_);
            ++o->global_refs;
        }
        c.SetX(0, h);
    });
    set_env(22, "DeleteGlobalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = objects_.find(h);
        if (it != objects_.end() && it->second->global_refs > 0) {
            --it->second->global_refs;
            if (!it->second->pinned && it->second->global_refs == 0 && it->second->local_refs == 0) {
                objects_.erase(it);
            }
        }
    });
    set_env(23, "DeleteLocalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = objects_.find(h);
        if (it != objects_.end() && it->second->local_refs > 0) {
            --it->second->local_refs;
            if (!it->second->pinned && it->second->global_refs == 0 && it->second->local_refs == 0) {
                objects_.erase(it);
            }
        }
    });
    set_env(24, "IsSameObject", [](CpuContext& c) { c.SetW(0, c.GetX(1) == c.GetX(2) ? 1u : 0u); });
    set_env(25, "NewLocalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        if (auto o = GetJniObject(h)) {
            std::lock_guard<std::mutex> lk(mtx_);
            ++o->local_refs;
        }
        c.SetX(0, h);
    });
    set_env(26, "EnsureLocalCapacity", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_env(226, "NewWeakGlobalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        if (auto o = GetJniObject(h)) { std::lock_guard<std::mutex> lk(mtx_); ++o->global_refs; }
        c.SetX(0, h);
    });
    set_env(227, "DeleteWeakGlobalRef", [this](CpuContext& c) {
        uint64_t h = c.GetX(1);
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = objects_.find(h);
        if (it != objects_.end() && it->second->global_refs > 0) --it->second->global_refs;
    });
    set_env(232, "GetObjectRefType", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o) { c.SetW(0, 0); return; }
        c.SetW(0, o->global_refs > 0 ? 2u : 1u);
    });

    // Object creation & class inspection (27..33)
    set_env(27, "AllocObject", [this](CpuContext& c) {
        auto cls = GetJniObject(c.GetX(1));
        c.SetX(0, cls ? NewObjectInstance(cls->class_name) : 0);
    });
    auto do_new_object = [this](CpuContext& c, int mode) {
        auto cls = GetJniObject(c.GetX(1));
        uint64_t mid = c.GetX(2);
        if (!cls) { c.SetX(0, 0); return; }
        uint64_t inst = NewObjectInstance(cls->class_name);
        std::vector<char> ptypes;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (auto it = method_ids_.find(mid); it != method_ids_.end()) ptypes = it->second.param_types;
        }
        std::vector<JniVal> args = (mode == 0) ? ExtractArgsVariadic(c, ptypes, 3)
                                 : (mode == 1) ? ExtractArgsVaList(c, ptypes, c.GetX(3))
                                               : ExtractArgsArray(ptypes, c.GetX(3));
        DispatchMethodCall(c, inst, mid, args, 'V');
        c.SetX(0, inst);
    };
    set_env(28, "NewObject", [do_new_object](CpuContext& c) { do_new_object(c, 0); });
    set_env(29, "NewObjectV", [do_new_object](CpuContext& c) { do_new_object(c, 1); });
    set_env(30, "NewObjectA", [do_new_object](CpuContext& c) { do_new_object(c, 2); });
    set_env(31, "GetObjectClass", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o) { c.SetX(0, 0); return; }
        uint64_t ch = FindClassHandle(o->class_name);
        if (!ch) ch = RegisterJniClass(JniClassDef{o->class_name});
        c.SetX(0, ch);
    });
    set_env(32, "IsInstanceOf", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        auto cls = GetJniObject(c.GetX(2));
        if (!o || !cls) { c.SetW(0, 0); return; }
        if (cls->class_name == "java/lang/Object" || o->class_name == cls->class_name) { c.SetW(0, 1); return; }
        std::string cur = o->class_name;
        while (!cur.empty()) {
            if (cur == cls->class_name) { c.SetW(0, 1); return; }
            auto* def = FindClassDef(cur);
            cur = def ? def->super_name : "";
        }
        c.SetW(0, 0);
    });
    set_env(33, "GetMethodID", [this](CpuContext& c) {
        c.SetX(0, ResolveMethodId(c.GetX(1),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(2))),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(3))),
            false, c.GetLR()));
    });

    // Helper for registering Call*Method / CallNonvirtual*Method / CallStatic*Method families
    auto reg_call_triplet = [&](int base_slot, const char* prefix, char ret_type, bool is_nonvirtual) {
        auto make_handler = [this, ret_type, is_nonvirtual](int mode) -> SvcHandlerFn {
            return [this, ret_type, is_nonvirtual, mode](CpuContext& c) {
                uint64_t target = c.GetX(1);
                uint64_t mid = is_nonvirtual ? c.GetX(3) : c.GetX(2);
                int first_gp = is_nonvirtual ? 4 : 3;
                std::vector<char> ptypes;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (auto it = method_ids_.find(mid); it != method_ids_.end()) {
                        ptypes = it->second.param_types;
                    }
                }
                std::vector<JniVal> args = (mode == 0) ? ExtractArgsVariadic(c, ptypes, first_gp)
                                         : (mode == 1) ? ExtractArgsVaList(c, ptypes, c.GetX(first_gp))
                                                       : ExtractArgsArray(ptypes, c.GetX(first_gp));
                DispatchMethodCall(c, target, mid, args, ret_type);
            };
        };
        set_env(base_slot + 0, prefix, make_handler(0));
        set_env(base_slot + 1, (std::string(prefix) + "V").c_str(), make_handler(1));
        set_env(base_slot + 2, (std::string(prefix) + "A").c_str(), make_handler(2));
    };

    // Instance methods (34..63)
    const char kRetTypes[10] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D', 'V'};
    const char* kNames[10] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
    for (int i = 0; i < 10; ++i) {
        reg_call_triplet(34 + i * 3, (std::string("Call") + kNames[i] + "Method").c_str(), kRetTypes[i], false);
        reg_call_triplet(64 + i * 3, (std::string("CallNonvirtual") + kNames[i] + "Method").c_str(), kRetTypes[i], true);
        reg_call_triplet(114 + i * 3, (std::string("CallStatic") + kNames[i] + "Method").c_str(), kRetTypes[i], false);
    }

    // Instance Fields (94..112)
    set_env(94, "GetFieldID", [this](CpuContext& c) {
        c.SetX(0, ResolveFieldId(c.GetX(1),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(2))),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(3))),
            false, c.GetLR()));
    });
    const char kFieldTypes[9] = {'L', 'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D'};
    for (int i = 0; i < 9; ++i) {
        char ft = kFieldTypes[i];
        set_env(95 + i, (std::string("Get") + kNames[i] + "Field").c_str(), [this, ft](CpuContext& c) {
            std::string fname;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (auto it = field_ids_.find(c.GetX(2)); it != field_ids_.end()) fname = it->second.name;
            }
            WriteReturnValue(c, GetInstanceField(c.GetX(1), fname), ft);
        });
        set_env(104 + i, (std::string("Set") + kNames[i] + "Field").c_str(), [this, ft](CpuContext& c) {
            std::string fname;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (auto it = field_ids_.find(c.GetX(2)); it != field_ids_.end()) fname = it->second.name;
            }
            JniVal v{};
            if (ft == 'F') { v.f32 = c.GetS(0); v.f64 = v.f32; }
            else if (ft == 'D') { v.f64 = c.GetD(0); v.f32 = static_cast<float>(v.f64); }
            else { v.u64 = c.GetX(3); }
            SetInstanceField(c.GetX(1), fname, v);
        });
    }

    // Static MethodID & Static Fields (113, 144..162)
    set_env(113, "GetStaticMethodID", [this](CpuContext& c) {
        c.SetX(0, ResolveMethodId(c.GetX(1),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(2))),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(3))),
            true, c.GetLR()));
    });
    set_env(144, "GetStaticFieldID", [this](CpuContext& c) {
        c.SetX(0, ResolveFieldId(c.GetX(1),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(2))),
            reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(3))),
            true, c.GetLR()));
    });
    for (int i = 0; i < 9; ++i) {
        char ft = kFieldTypes[i];
        set_env(145 + i, (std::string("GetStatic") + kNames[i] + "Field").c_str(), [this, ft](CpuContext& c) {
            std::string cname, fname;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (auto it = field_ids_.find(c.GetX(2)); it != field_ids_.end()) {
                    cname = it->second.class_name;
                    fname = it->second.name;
                }
            }
            WriteReturnValue(c, GetStaticField(cname, fname), ft);
        });
        set_env(154 + i, (std::string("SetStatic") + kNames[i] + "Field").c_str(), [this, ft](CpuContext& c) {
            std::string cname, fname;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (auto it = field_ids_.find(c.GetX(2)); it != field_ids_.end()) {
                    cname = it->second.class_name;
                    fname = it->second.name;
                }
            }
            JniVal v{};
            if (ft == 'F') { v.f32 = c.GetS(0); v.f64 = v.f32; }
            else if (ft == 'D') { v.f64 = c.GetD(0); v.f32 = static_cast<float>(v.f64); }
            else { v.u64 = c.GetX(3); }
            SetStaticField(cname, fname, v);
        });
    }

    // ---- Strings (163..170, 220..221, 224..225) ----
    set_env(163, "NewString", [this](CpuContext& c) {
        const auto* u16 = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(c.GetX(1)));
        size_t len = static_cast<size_t>(c.GetW(2));
        c.SetX(0, NewStringUtf(Utf16ToUtf8(u16, len)));
    });
    set_env(164, "GetStringLength", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        c.SetW(0, o ? static_cast<uint32_t>(o->utf16.size()) : 0);
    });
    set_env(165, "GetStringChars", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        auto* is_copy = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(c.GetX(2)));
        if (is_copy) *is_copy = 0;
        if (!o) { c.SetX(0, 0); return; }
        if (o->utf16.empty()) o->utf16.push_back(0);
        c.SetX(0, reinterpret_cast<uint64_t>(o->utf16.data()));
    });
    set_env(166, "ReleaseStringChars", [](CpuContext&) {});
    set_env(167, "NewStringUTF", [this](CpuContext& c) {
        const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(c.GetX(1)));
        c.SetX(0, s ? NewStringUtf(s) : 0);
    });
    set_env(168, "GetStringUTFLength", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        c.SetW(0, o ? static_cast<uint32_t>(o->utf8.size()) : 0);
    });
    set_env(169, "GetStringUTFChars", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        auto* is_copy = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(c.GetX(2)));
        if (is_copy) *is_copy = 0;
        c.SetX(0, o ? reinterpret_cast<uint64_t>(o->utf8.c_str()) : 0);
    });
    set_env(170, "ReleaseStringUTFChars", [](CpuContext&) {});
    set_env(220, "GetStringRegion", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        size_t start = static_cast<size_t>(c.GetW(2));
        size_t len = static_cast<size_t>(c.GetW(3));
        auto* dst = reinterpret_cast<uint16_t*>(static_cast<uintptr_t>(c.GetX(4)));
        if (o && dst && start + len <= o->utf16.size()) {
            std::memcpy(dst, o->utf16.data() + start, len * 2);
        }
    });
    set_env(221, "GetStringUTFRegion", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        size_t start = static_cast<size_t>(c.GetW(2));
        size_t len = static_cast<size_t>(c.GetW(3));
        auto* dst = reinterpret_cast<char*>(static_cast<uintptr_t>(c.GetX(4)));
        if (o && dst && start <= o->utf8.size()) {
            size_t n = std::min(len, o->utf8.size() - start);
            std::memcpy(dst, o->utf8.data() + start, n);
            dst[n] = '\0';
        }
    });
    set_env(224, "GetStringCritical", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        auto* is_copy = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(c.GetX(2)));
        if (is_copy) *is_copy = 0;
        c.SetX(0, (o && !o->utf16.empty()) ? reinterpret_cast<uint64_t>(o->utf16.data()) : 0);
    });
    set_env(225, "ReleaseStringCritical", [](CpuContext&) {});

    // ---- Arrays (171..214, 222..223) ----
    set_env(171, "GetArrayLength", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o) { c.SetW(0, 0); return; }
        if (o->kind == JniObject::Kind::ObjectArray) {
            c.SetW(0, static_cast<uint32_t>(o->obj_elems.size()));
            return;
        }
        size_t es = ElemSizeForArrayKind(o->kind);
        c.SetW(0, es ? static_cast<uint32_t>(o->bytes.size() / es) : 0);
    });
    set_env(172, "NewObjectArray", [this](CpuContext& c) {
        size_t len = static_cast<size_t>(c.GetW(1));
        auto cls = GetJniObject(c.GetX(2));
        uint64_t init = c.GetX(3);
        c.SetX(0, NewObjectArray(len, cls ? cls->class_name : "java/lang/Object", init));
    });
    set_env(173, "GetObjectArrayElement", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        size_t idx = static_cast<size_t>(c.GetW(2));
        c.SetX(0, (o && idx < o->obj_elems.size()) ? o->obj_elems[idx] : 0);
    });
    set_env(174, "SetObjectArrayElement", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        size_t idx = static_cast<size_t>(c.GetW(2));
        if (o && idx < o->obj_elems.size()) o->obj_elems[idx] = c.GetX(3);
    });

    const JniObject::Kind kArrKinds[8] = {
        JniObject::Kind::BoolArray, JniObject::Kind::ByteArray, JniObject::Kind::CharArray,
        JniObject::Kind::ShortArray, JniObject::Kind::IntArray, JniObject::Kind::LongArray,
        JniObject::Kind::FloatArray, JniObject::Kind::DoubleArray
    };
    const char* kArrNames[8] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
    const char* kArrSigs[8] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    for (int i = 0; i < 8; ++i) {
        auto kind = kArrKinds[i];
        size_t es = ElemSizeForArrayKind(kind);
        const char* sig = kArrSigs[i];
        set_env(175 + i, (std::string("New") + kArrNames[i] + "Array").c_str(), [this, kind, es, sig](CpuContext& c) {
            size_t len = static_cast<size_t>(c.GetW(1));
            auto obj = std::make_shared<JniObject>();
            obj->kind = kind;
            obj->class_name = sig;
            obj->bytes.resize(len * es, 0);
            c.SetX(0, AllocHandle(std::move(obj)));
        });
        set_env(183 + i, (std::string("Get") + kArrNames[i] + "ArrayElements").c_str(), [this](CpuContext& c) {
            auto o = GetJniObject(c.GetX(1));
            auto* is_copy = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(c.GetX(2)));
            if (is_copy) *is_copy = 0;
            if (!o) { c.SetX(0, 0); return; }
            if (o->bytes.empty()) o->bytes.reserve(8);
            c.SetX(0, reinterpret_cast<uint64_t>(o->bytes.data()));
        });
        set_env(191 + i, (std::string("Release") + kArrNames[i] + "ArrayElements").c_str(), [](CpuContext&) {});
        set_env(199 + i, (std::string("Get") + kArrNames[i] + "ArrayRegion").c_str(), [this, es](CpuContext& c) {
            auto o = GetJniObject(c.GetX(1));
            size_t start = static_cast<size_t>(c.GetW(2));
            size_t len = static_cast<size_t>(c.GetW(3));
            void* dst = reinterpret_cast<void*>(static_cast<uintptr_t>(c.GetX(4)));
            if (o && dst && (start + len) * es <= o->bytes.size()) {
                std::memcpy(dst, o->bytes.data() + start * es, len * es);
            }
        });
        set_env(207 + i, (std::string("Set") + kArrNames[i] + "ArrayRegion").c_str(), [this, es](CpuContext& c) {
            auto o = GetJniObject(c.GetX(1));
            size_t start = static_cast<size_t>(c.GetW(2));
            size_t len = static_cast<size_t>(c.GetW(3));
            const void* src = reinterpret_cast<const void*>(static_cast<uintptr_t>(c.GetX(4)));
            if (o && src && (start + len) * es <= o->bytes.size()) {
                std::memcpy(o->bytes.data() + start * es, src, len * es);
            }
        });
    }
    set_env(222, "GetPrimitiveArrayCritical", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        auto* is_copy = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(c.GetX(2)));
        if (is_copy) *is_copy = 0;
        c.SetX(0, o ? reinterpret_cast<uint64_t>(o->bytes.data()) : 0);
    });
    set_env(223, "ReleasePrimitiveArrayCritical", [](CpuContext&) {});

    // ---- RegisterNatives / Monitors / GetJavaVM / DirectByteBuffer (215..219, 229..231) ----
    set_env(215, "RegisterNatives", [this](CpuContext& c) {
        auto cls = GetJniObject(c.GetX(1));
        const auto* methods = reinterpret_cast<const GuestJniNativeMethod*>(static_cast<uintptr_t>(c.GetX(2)));
        int n = static_cast<int>(c.GetW(3));
        if (cls && methods && n > 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            for (int i = 0; i < n; ++i) {
                const char* mname = reinterpret_cast<const char*>(static_cast<uintptr_t>(methods[i].name_ptr));
                const char* msig = reinterpret_cast<const char*>(static_cast<uintptr_t>(methods[i].sig_ptr));
                if (mname && msig) {
                    registered_natives_[cls->class_name + ":" + mname + ":" + msig] = methods[i].fn_ptr;
                    registered_natives_[cls->class_name + ":" + mname] = methods[i].fn_ptr;
                }
            }
        }
        c.SetW(0, kJniOk);
    });
    set_env(216, "UnregisterNatives", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_env(217, "MonitorEnter", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_env(218, "MonitorExit", [](CpuContext& c) { c.SetW(0, kJniOk); });
    set_env(219, "GetJavaVM", [this](CpuContext& c) {
        auto* out_vm = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(c.GetX(1)));
        if (out_vm) *out_vm = java_vm_ptr_;
        c.SetW(0, kJniOk);
    });
    set_env(229, "NewDirectByteBuffer", [this](CpuContext& c) {
        void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(c.GetX(1)));
        size_t cap = static_cast<size_t>(c.GetX(2));
        c.SetX(0, NewDirectByteBuffer(addr, cap, false));
    });
    set_env(230, "GetDirectBufferAddress", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o || o->kind != JniObject::Kind::DirectBuffer) { c.SetX(0, 0); return; }
        void* p = o->external_direct_ptr ? o->external_direct_ptr : static_cast<void*>(o->bytes.data());
        c.SetX(0, reinterpret_cast<uint64_t>(p));
    });
    set_env(231, "GetDirectBufferCapacity", [this](CpuContext& c) {
        auto o = GetJniObject(c.GetX(1));
        if (!o || o->kind != JniObject::Kind::DirectBuffer) { c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        c.SetX(0, static_cast<uint64_t>(o->direct_capacity));
    });
}

void FakeJvm::InitBuiltinClasses() {
    // java/lang/Object
    {
        JniClassDef d;
        d.name = "java/lang/Object";
        d.methods["toString"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            return JniVal::Obj(jvm.NewStringUtf(o ? o->class_name : "null"));
        };
        d.methods["hashCode"] = [](FakeJvm&, uint64_t self, const auto&) {
            return JniVal::Int(static_cast<int32_t>(self & 0x7FFFFFFF));
        };
        d.methods["equals"] = [](FakeJvm&, uint64_t self, const auto& a) {
            return JniVal::Bool(!a.empty() && self == a[0].AsObj());
        };
        d.methods["getClass"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            return JniVal::Obj(o ? jvm.FindClassHandle(o->class_name) : 0);
        };
        RegisterJniClass(std::move(d));
    }

    // java/lang/Class
    {
        JniClassDef d;
        d.name = "java/lang/Class";
        d.methods["getName"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            std::string n = o ? o->class_name : "";
            for (char& ch : n) if (ch == '/') ch = '.';
            return JniVal::Obj(jvm.NewStringUtf(n));
        };
        d.methods["getSimpleName"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            std::string n = o ? o->class_name : "";
            auto pos = n.rfind('/');
            if (pos != std::string::npos) n = n.substr(pos + 1);
            return JniVal::Obj(jvm.NewStringUtf(n));
        };
        RegisterJniClass(std::move(d));
    }

    // java/lang/String
    {
        JniClassDef d;
        d.name = "java/lang/String";
        d.methods["length"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            return JniVal::Int(o ? static_cast<int32_t>(o->utf16.size()) : 0);
        };
        d.methods["getBytes"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            std::string s = jvm.GetStringUtf(self);
            return JniVal::Obj(jvm.NewByteArray(s.data(), s.size()));
        };
        d.methods["toString"] = [](FakeJvm&, uint64_t self, const auto&) { return JniVal::Obj(self); };
        d.methods["equals"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (a.empty()) return JniVal::Bool(false);
            return JniVal::Bool(jvm.GetStringUtf(self) == jvm.GetStringUtf(a[0].AsObj()));
        };
        RegisterJniClass(std::move(d));
    }

    // java/lang/Throwable & Exception
    {
        JniClassDef d;
        d.name = "java/lang/Throwable";
        d.methods["getMessage"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto v = jvm.GetInstanceField(self, "message");
            return v.AsObj() ? v : JniVal::Obj(jvm.NewStringUtf("exception"));
        };
        d.methods["toString"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto msg = jvm.GetStringUtf(jvm.GetInstanceField(self, "message").AsObj());
            return JniVal::Obj(jvm.NewStringUtf(msg));
        };
        RegisterJniClass(std::move(d));
    }
    RegisterJniClass(JniClassDef{"java/lang/Exception", "java/lang/Throwable"});
    RegisterJniClass(JniClassDef{"java/lang/RuntimeException", "java/lang/Exception"});
    RegisterJniClass(JniClassDef{"java/lang/NoClassDefFoundError", "java/lang/Throwable"});

    // Primitive wrapper classes
    {
        JniClassDef d;
        d.name = "java/lang/Integer";
        d.methods["<init>:(I)V"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (!a.empty()) jvm.SetInstanceField(self, "value", a[0]);
            return JniVal::Void();
        };
        d.methods["intValue"] = [](FakeJvm& jvm, uint64_t self, const auto&) { return jvm.GetInstanceField(self, "value"); };
        d.static_methods["valueOf"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            uint64_t h = jvm.NewObjectInstance("java/lang/Integer");
            if (!a.empty()) jvm.SetInstanceField(h, "value", a[0]);
            return JniVal::Obj(h);
        };
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/Boolean";
        d.methods["<init>:(Z)V"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (!a.empty()) jvm.SetInstanceField(self, "value", a[0]);
            return JniVal::Void();
        };
        d.methods["booleanValue"] = [](FakeJvm& jvm, uint64_t self, const auto&) { return jvm.GetInstanceField(self, "value"); };
        d.static_methods["valueOf"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            uint64_t h = jvm.NewObjectInstance("java/lang/Boolean");
            if (!a.empty()) jvm.SetInstanceField(h, "value", a[0]);
            return JniVal::Obj(h);
        };
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/Long";
        d.methods["<init>:(J)V"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (!a.empty()) jvm.SetInstanceField(self, "value", a[0]);
            return JniVal::Void();
        };
        d.methods["longValue"] = [](FakeJvm& jvm, uint64_t self, const auto&) { return jvm.GetInstanceField(self, "value"); };
        d.static_methods["valueOf"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            uint64_t h = jvm.NewObjectInstance("java/lang/Long");
            if (!a.empty()) jvm.SetInstanceField(h, "value", a[0]);
            return JniVal::Obj(h);
        };
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/Float";
        d.methods["<init>:(F)V"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (!a.empty()) jvm.SetInstanceField(self, "value", a[0]);
            return JniVal::Void();
        };
        d.methods["floatValue"] = [](FakeJvm& jvm, uint64_t self, const auto&) { return jvm.GetInstanceField(self, "value"); };
        d.static_methods["valueOf"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            uint64_t h = jvm.NewObjectInstance("java/lang/Float");
            if (!a.empty()) jvm.SetInstanceField(h, "value", a[0]);
            return JniVal::Obj(h);
        };
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/Double";
        d.methods["<init>:(D)V"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            if (!a.empty()) jvm.SetInstanceField(self, "value", a[0]);
            return JniVal::Void();
        };
        d.methods["doubleValue"] = [](FakeJvm& jvm, uint64_t self, const auto&) { return jvm.GetInstanceField(self, "value"); };
        d.static_methods["valueOf"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            uint64_t h = jvm.NewObjectInstance("java/lang/Double");
            if (!a.empty()) jvm.SetInstanceField(h, "value", a[0]);
            return JniVal::Obj(h);
        };
        RegisterJniClass(std::move(d));
    }

    // java/nio/ByteBuffer & DirectByteBuffer
    {
        JniClassDef d;
        d.name = "java/nio/ByteBuffer";
        d.static_methods["allocateDirect"] = [](FakeJvm& jvm, uint64_t, const auto& a) {
            size_t cap = a.empty() ? 0 : static_cast<size_t>(std::max(0, a[0].AsInt()));
            return JniVal::Obj(jvm.NewDirectByteBufferOwned(std::vector<uint8_t>(cap, 0)));
        };
        d.methods["capacity"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            return JniVal::Int(o ? static_cast<int32_t>(o->direct_capacity) : 0);
        };
        d.methods["limit"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            return JniVal::Int(o ? static_cast<int32_t>(o->direct_capacity) : 0);
        };
        d.methods["order"] = [](FakeJvm&, uint64_t self, const auto&) { return JniVal::Obj(self); };
        RegisterJniClass(std::move(d));
    }
    RegisterJniClass(JniClassDef{"java/nio/DirectByteBuffer", "java/nio/ByteBuffer"});

    // java/util/ArrayList & HashMap (minimal containers used by Nimble/SDK code)
    {
        JniClassDef d;
        d.name = "java/util/ArrayList";
        d.create_native_state = []() -> std::any { return std::vector<uint64_t>{}; };
        d.methods["size"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            if (!o || !o->native_state.has_value()) return JniVal::Int(0);
            return JniVal::Int(static_cast<int32_t>(std::any_cast<std::vector<uint64_t>&>(o->native_state).size()));
        };
        d.methods["add"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            auto o = jvm.GetJniObject(self);
            if (o && !a.empty()) {
                if (!o->native_state.has_value()) o->native_state = std::vector<uint64_t>{};
                std::any_cast<std::vector<uint64_t>&>(o->native_state).push_back(a[0].AsObj());
            }
            return JniVal::Bool(true);
        };
        d.methods["get"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            auto o = jvm.GetJniObject(self);
            if (!o || !o->native_state.has_value() || a.empty()) return JniVal::Obj(0);
            auto& v = std::any_cast<std::vector<uint64_t>&>(o->native_state);
            size_t idx = static_cast<size_t>(a[0].AsInt());
            return JniVal::Obj(idx < v.size() ? v[idx] : 0);
        };
        RegisterJniClass(std::move(d));
    }
    RegisterJniClass(JniClassDef{"java/util/List", "java/lang/Object"});

    {
        JniClassDef d;
        d.name = "java/util/HashMap";
        d.create_native_state = []() -> std::any { return std::unordered_map<std::string, uint64_t>{}; };
        d.methods["put"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            auto o = jvm.GetJniObject(self);
            if (o && a.size() >= 2) {
                if (!o->native_state.has_value()) o->native_state = std::unordered_map<std::string, uint64_t>{};
                auto& m = std::any_cast<std::unordered_map<std::string, uint64_t>&>(o->native_state);
                std::string k = jvm.GetStringUtf(a[0].AsObj());
                uint64_t prev = m.count(k) ? m[k] : 0;
                m[k] = a[1].AsObj();
                return JniVal::Obj(prev);
            }
            return JniVal::Obj(0);
        };
        d.methods["get"] = [](FakeJvm& jvm, uint64_t self, const auto& a) {
            auto o = jvm.GetJniObject(self);
            if (!o || !o->native_state.has_value() || a.empty()) return JniVal::Obj(0);
            auto& m = std::any_cast<std::unordered_map<std::string, uint64_t>&>(o->native_state);
            auto it = m.find(jvm.GetStringUtf(a[0].AsObj()));
            return JniVal::Obj(it != m.end() ? it->second : 0);
        };
        d.methods["size"] = [](FakeJvm& jvm, uint64_t self, const auto&) {
            auto o = jvm.GetJniObject(self);
            if (!o || !o->native_state.has_value()) return JniVal::Int(0);
            return JniVal::Int(static_cast<int32_t>(
                std::any_cast<std::unordered_map<std::string, uint64_t>&>(o->native_state).size()));
        };
        RegisterJniClass(std::move(d));
    }
    RegisterJniClass(JniClassDef{"java/util/Map", "java/lang/Object"});

    // java/util/Locale & TimeZone
    {
        JniClassDef d;
        d.name = "java/util/Locale";
        d.static_methods["getDefault"] = [](FakeJvm& jvm, uint64_t, const auto&) {
            return JniVal::Obj(jvm.NewObjectInstance("java/util/Locale"));
        };
        d.methods["getLanguage"] = [](FakeJvm& jvm, uint64_t, const auto&) { return JniVal::Obj(jvm.NewStringUtf("en")); };
        d.methods["getCountry"] = [](FakeJvm& jvm, uint64_t, const auto&) { return JniVal::Obj(jvm.NewStringUtf("US")); };
        d.methods["toString"] = [](FakeJvm& jvm, uint64_t, const auto&) { return JniVal::Obj(jvm.NewStringUtf("en_US")); };
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/TimeZone";
        d.static_methods["getDefault"] = [](FakeJvm& jvm, uint64_t, const auto&) {
            return JniVal::Obj(jvm.NewObjectInstance("java/util/TimeZone"));
        };
        d.methods["getID"] = [](FakeJvm& jvm, uint64_t, const auto&) { return JniVal::Obj(jvm.NewStringUtf("UTC")); };
        d.methods["getRawOffset"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(0); };
        RegisterJniClass(std::move(d));
    }

    // android/os/Build & Build$VERSION
    {
        JniClassDef d;
        d.name = "android/os/Build";
        d.static_fields["MODEL"] = JniVal::Obj(NewStringUtf("SM-S918B"));
        d.static_fields["DEVICE"] = JniVal::Obj(NewStringUtf("dm3q"));
        d.static_fields["MANUFACTURER"] = JniVal::Obj(NewStringUtf("samsung"));
        d.static_fields["BRAND"] = JniVal::Obj(NewStringUtf("samsung"));
        d.static_fields["PRODUCT"] = JniVal::Obj(NewStringUtf("dm3qxxx"));
        d.static_fields["HARDWARE"] = JniVal::Obj(NewStringUtf("qcom"));
        d.static_fields["BOARD"] = JniVal::Obj(NewStringUtf("kalama"));
        d.static_fields["FINGERPRINT"] = JniVal::Obj(NewStringUtf("samsung/dm3qxxx/dm3q:13/TP1A.220624.014/S918BXXU1AWC8:user/release-keys"));
        for (auto& [k, v] : d.static_fields) PinHandle(v.AsObj());
        RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/os/Build$VERSION";
        d.static_fields["SDK_INT"] = JniVal::Int(31);
        d.static_fields["RELEASE"] = JniVal::Obj(NewStringUtf("12"));
        PinHandle(d.static_fields["RELEASE"].AsObj());
        RegisterJniClass(std::move(d));
    }
}
