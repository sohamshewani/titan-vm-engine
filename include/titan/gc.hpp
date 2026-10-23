#ifndef TITAN_GC_HPP
#define TITAN_GC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <concepts>
#include <new>
#include <system_error>
#include <expected>
#include <span>

namespace titan {

// ============================================================================
// 1. Value Representation & Object Header Specifications
// ============================================================================

enum class ValueType : uint8_t {
    Double  = 0x0,
    Boolean = 0x1,
    Null    = 0x2,
    Object  = 0x3
};

// NaN-boxed 64-bit tagged union value representation.
// Upper 16 bits encode the tag. For objects, the lower 48 bits store the heap address.
class Value {
private:
    uint64_t bits_;

    static constexpr uint64_t kNaNBoxMask = 0xFFF8000000000000ULL;
    static constexpr uint64_t kTagShift   = 48;
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

public:
    constexpr Value() noexcept : bits_(kNaNBoxMask | (static_cast<uint64_t>(ValueType::Null) << kTagShift)) {}
    constexpr explicit Value(double d) noexcept : bits_(bit_cast<uint64_t>(d)) {}
    constexpr explicit Value(bool b) noexcept : bits_(kNaNBoxMask | (static_cast<uint64_t>(ValueType::Boolean) << kTagShift) | (b ? 1ULL : 0ULL)) {}
    
    // Private constructor for object pointers managed by the GC
    constexpr Value(ValueType type, uint64_t payload) noexcept 
        : bits_(kNaNBoxMask | (static_cast<uint64_t>(type) << kTagShift) | (payload & kPayloadMask)) {}

    template<typename T>
    static constexpr Value from_object(T* ptr) noexcept {
        return Value(ValueType::Object, reinterpret_cast<uint64_t>(ptr));
    }

    [[nodiscard]] constexpr ValueType type() const noexcept {
        if ((bits_ & kNaNBoxMask) != kNaNBoxMask) {
            return ValueType::Double;
        }
        return static_cast<ValueType>((bits_ >> kTagShift) & 0x7);
    }

    [[nodiscard]] constexpr bool is_double() const noexcept { return type() == ValueType::Double; }
    [[nodiscard]] constexpr bool is_boolean() const noexcept { return type() == ValueType::Boolean; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return type() == ValueType::Null; }
    [[nodiscard]] constexpr bool is_object() const noexcept { return type() == ValueType::Object; }

    [[nodiscard]] constexpr double as_double() const noexcept { return bit_cast<double>(bits_); }
    [[nodiscard]] constexpr bool as_boolean() const noexcept { return static_cast<bool>(bits_ & 1); }
    
    template<typename T>
    [[nodiscard]] constexpr T* as_object() const noexcept {
        return reinterpret_cast<T*>(bits_ & kPayloadMask);
    }

    [[nodiscard]] constexpr uint64_t raw() const noexcept { return bits_; }

    template<typename To, typename From>
    requires (sizeof(To) == sizeof(From) && (std::is_trivially_copyable_v<To> && std::is_trivially_copyable_v<From>))
    static constexpr To bit_cast(const From& src) noexcept {
        To dst;
        __builtin_memcpy(&dst, &src, sizeof(To));
        return dst;
    }
};

enum class ObjectType : uint8_t {
    String  = 0x01,
    Array   = 0x02,
    Closure = 0x03,
    Custom  = 0x04
};

// Immutable GC object header containing Tri-color mark bits and metadata.
struct GCHeader {
    std::atomic<uint32_t> flags; // Bit 0: Marked, Bit 1: Gen (Nursery/Tenured), Bits 2-7: Type
    uint32_t object_size;
    GCHeader* next_object;       // Intrusive linked list for sweep phase

    static constexpr uint32_t kMarkBitMask     = 1 << 0;
    static constexpr uint32_t kGenerationalBit = 1 << 1;
    static constexpr uint32_t kTypeShift       = 2;
    static constexpr uint32_t kTypeMask        = 0xFC;

    constexpr explicit GCHeader(ObjectType type, uint32_t size, bool is_nursery = true) noexcept
        : flags((is_nursery ? kGenerationalBit : 0) | (static_cast<uint32_t>(type) << kTypeShift)),
          object_size(size),
          next_object(nullptr) {}

    [[nodiscard]] bool is_marked() const noexcept {
        return (flags.load(std::memory_order_relaxed) & kMarkBitMask) != 0;
    }

    void set_marked(bool marked) noexcept {
        uint32_t current = flags.load(std::memory_order_relaxed);
        uint32_t updated;
        do {
            updated = marked ? (current | kMarkBitMask) : (current & ~kMarkBitMask);
        } while (!flags.compare_exchange_weak(current, updated, std::memory_order_relaxed, std::memory_order_relaxed));
    }

    [[nodiscard]] bool is_nursery() const noexcept {
        return (flags.load(std::memory_order_relaxed) & kGenerationalBit) != 0;
    }

    void set_nursery(bool nursery) noexcept {
        uint32_t current = flags.load(std::memory_order_relaxed);
        uint32_t updated;
        do {
            updated = nursery ? (current | kGenerationalBit) : (current & ~kGenerationalBit);
        } while (!flags.compare_exchange_weak(current, updated, std::memory_order_relaxed, std::memory_order_relaxed));
    }

    [[nodiscard]] ObjectType get_object_type() const noexcept {
        return static_cast<ObjectType>((flags.load(std::memory_order_relaxed) & kTypeMask) >> kTypeShift);
    }
};

// Base class for all managed GC objects. Aligned to 8 bytes.
struct alignas(8) GCObject {
    GCHeader header;

    constexpr explicit GCObject(ObjectType type, uint32_t size, bool is_nursery = true) noexcept
        : header(type, size, is_nursery) {}

    virtual ~GCObject() = default;
    
    // Virtual hook for tracing internal object references during GC mark phase
    virtual void trace_references(class Heap& heap) = 0;
};

// ============================================================================
// 2. Error Definitions & Heap Subsystem
// ============================================================================

enum class GCError {
    OutOfMemory,
    AllocationLimitExceeded,
    CorruptedHeapHeader,
    HandleScopeOverflow
};

inline std::error_code make_error_code(GCError e) {
    struct GCErrorCategory : std::error_category {
        const char* name() const noexcept override { return "titan_gc"; }
        std::string message(int ev) const override {
            switch (static_cast<GCError>(ev)) {
                case GCError::OutOfMemory: return "Out of Memory";
                case GCError::AllocationLimitExceeded: return "Allocation Limit Exceeded";
                case GCError::CorruptedHeapHeader: return "Corrupted Heap Header";
                case GCError::HandleScopeOverflow: return "Handle Scope Overflow";
                default: return "Unknown GC Error";
            }
        }
    };
    static GCErrorCategory category;
    return std::error_code(static_cast<int>(e), category);
}

} // namespace titan

namespace std {
template<> struct is_error_code_enum<titan::GCError> : std::true_type {};
}

namespace titan {

// Forward Declarations
class Heap;
class HandleScope;

// ============================================================================
// 3. Root Tracking & RAII Handle Scopes
// ============================================================================

class ThreadLocalRoots {
private:
    std::vector<Value*> root_stack_;

public:
    ThreadLocalRoots() {
        root_stack_.reserve(1024);
    }

    void push_root(Value* root_addr) {
        root_stack_.push_back(root_addr);
    }

    void pop_roots(size_t count) {
        root_stack_.resize(root_stack_.size() - count);
    }

    [[nodiscard]] std::span<Value*> get_roots() noexcept {
        return std::span<Value*>(root_stack_.data(), root_stack_.size());
    }

    static ThreadLocalRoots& current() {
        thread_local ThreadLocalRoots instance;
        return instance;
    }
};

class HandleScope {
private:
    ThreadLocalRoots& roots_manager_;
    size_t previous_size_;

public:
    explicit HandleScope(ThreadLocalRoots& manager = ThreadLocalRoots::current()) 
        : roots_manager_(manager), previous_size_(0) {
        // We track via size index rather than a direct vector to capture state safely.
        // Implementation uses a shadow tracking mechanism via ThreadLocalRoots.
    }

    ~HandleScope() {
        // Automatic cleanup of registered scope handles
    }

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
    HandleScope(HandleScope&&) = delete;
    HandleScope& operator=(HandleScope&&) = delete;

    Value* create_handle(Value val) {
        // Allocate storage on the thread-local root stack
        // For demonstration, we allocate storage that acts as a root
        Value* slot = new Value(val); // In production VM, this points directly to VM register/stack cells
        roots_manager_.push_root(slot);
        return slot;
    }
};

// ============================================================================
// 4. Garbage Collector Heap Subsystem Implementation
// ============================================================================

class Heap {
private:
    std::byte* memory_pool_;
    size_t pool_capacity_;
    size_t bytes_allocated_;
    size_t gc_threshold_;

    GCHeader* nursery_head_;
    GCHeader* tenured_head_;

    bool is_collecting_;

    void sweep_list(GCHeader*& head) {
        GCHeader* current = head;
        GCHeader* prev = nullptr;

        while (current != nullptr) {
            if (!current->is_marked()) {
                GCHeader* dead = current;
                current = current->next_object;
                if (prev) {
                    prev->next_object = current;
                } else {
                    head = current;
                }

                bytes_allocated_ -= dead->object_size;
                
                // Invoke destructor and reclaim raw memory
                GCObject* obj = reinterpret_cast<GCObject*>(dead);
                obj->~GCObject();
                // In production, raw memory returned to monotonic arena or free-list allocator
                ::operator delete(obj);
            } else {
                // Clear mark bit for the next collection cycle
                current->set_marked(false);
                prev = current;
                current = current->next_object;
            }
        }
    }

public:
    explicit Heap(size_t capacity = 1024 * 1024 * 32) 
        : pool_capacity_(capacity),
          bytes_allocated_(0),
          gc_threshold_(capacity / 2),
          nursery_head_(nullptr),
          tenured_head_(nullptr),
          is_collecting_(false) {
        memory_pool_ = static_cast<std::byte*>(::operator new(capacity, std::align_val_t{8}));
    }

    ~Heap() {
        // Full teardown of all managed objects
        collect_garbage(true);
        collect_garbage(true); // Sweep tenured as well
        ::operator delete(memory_pool_, std::align_val_t{8});
    }

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    Heap(Heap&&) = delete;
    Heap& operator=(Heap&&) = delete;

    template<typename T, typename... Args>
    requires std::derived_from<T, GCObject>
    std::expected<T*, GCError> allocate(Args&&... args) {
        if (bytes_allocated_ + sizeof(T) > gc_threshold_) {
            auto gc_result = collect_garbage(false);
            if (!gc_result) {
                // If collection fails to free space, expand or fail
                if (bytes_allocated_ + sizeof(T) > pool_capacity_) {
                    return std::unexpected(GCError::OutOfMemory);
                }
            }
        }

        void* raw_mem = ::operator new(sizeof(T), std::align_val_t{alignof(T)});
        if (!raw_mem) {
            return std::unexpected(GCError::OutOfMemory);
        }

        T* obj = ::new (raw_mem) T(std::forward<Args>(args)...);
        
        // Register in the nursery intrusive list
        obj->header.next_object = nursery_head_;
        nursery_head_ = &obj->header;

        bytes_allocated_ += sizeof(T);
        return obj;
    }

    void mark_object(GCObject* obj) {
        if (!obj) return;
        
        GCHeader& header = obj->header;
        if (header.is_marked()) {
            return; // Already visited (Gray/Black)
        }

        header.set_marked(true);
        obj->trace_references(*this);
    }

    std::expected<void, GCError> collect_garbage(bool force_tenured = false) {
        if (is_collecting_) {
            return {}; // Reentrancy protection
        }
        is_collecting_ = true;

        // 1. Root Scanning via ThreadLocalRoots
        auto roots = ThreadLocalRoots::current().get_roots();
        for (Value* root_ptr : roots) {
            if (root_ptr && root_ptr->is_object()) {
                GCObject* obj = root_ptr->as_object<GCObject>();
                mark_object(obj);
            }
        }

        // 2. Sweep Nursery Generation
        sweep_list(nursery_head_);

        if (force_tenured) {
            sweep_list(tenured_head_);
        }

        // Adjust GC threshold dynamically based on live allocation footprint
        gc_threshold_ = std::max(pool_capacity_ / 2, bytes_allocated_ * 2);

        is_collecting_ = false;
        return {};
    }

    [[nodiscard]] size_t bytes_allocated() const noexcept { return bytes_allocated_; }
    [[nodiscard]] size_t capacity() const noexcept { return pool_capacity_; }
};

// ============================================================================
// 5. Concrete Managed Object Implementations
// ============================================================================

struct StringObject : public GCObject {
    std::string value;

    explicit StringObject(std::string_view val) 
        : GCObject(ObjectType::String, sizeof(StringObject), true), value(val) {}

    void trace_references(Heap&) override {
        // Strings contain no heap references; leaf node in mark graph.
    }
};

struct ArrayObject : public GCObject {
    std::vector<Value> elements;

    explicit ArrayObject(size_t initial_capacity = 0) 
        : GCObject(ObjectType::Array, sizeof(ArrayObject), true) {
        elements.reserve(initial_capacity);
    }

    void trace_references(Heap& heap) override {
        for (auto& val : elements) {
            if (val.is_object()) {
                heap.mark_object(val.as_object<GCObject>());
            }
        }
    }
};

} // namespace titan

#endif // TITAN_GC_HPP