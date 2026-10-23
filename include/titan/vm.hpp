#ifndef TITAN_VM_HPP
#define TITAN_VM_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <new>
#include <memory>
#include <memory_resource>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <atomic>
#include <shared_mutex>
#include <expected>
#include <system_error>
#include <type_traits>
#include <span>

namespace titan {

// ============================================================================
// 1. ERROR HANDLING & SYSTEM CODES
// ============================================================================

enum class ErrorCode : uint32_t {
    Success = 0,
    OutOfMemory,
    StackOverflow,
    StackUnderflow,
    TypeMismatch,
    InvalidOpcode,
    UndefinedVariable,
    NullDereference,
    IndexOutOfBounds,
    MessageQueueFull,
    MessageQueueEmpty,
    CompilationError
};

class ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "titan-vm"; }
    
    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Success: return "Success";
            case ErrorCode::OutOfMemory: return "Out of memory";
            case ErrorCode::StackOverflow: return "VM stack overflow";
            case ErrorCode::StackUnderflow: return "VM stack underflow";
            case ErrorCode::TypeMismatch: return "Runtime type mismatch";
            case ErrorCode::InvalidOpcode: return "Invalid bytecode opcode";
            case ErrorCode::UndefinedVariable: return "Undefined variable reference";
            case ErrorCode::NullDereference: return "Null pointer dereference";
            case ErrorCode::IndexOutOfBounds: return "Collection index out of bounds";
            case ErrorCode::MessageQueueFull: return "Actor message queue full";
            case ErrorCode::MessageQueueEmpty: return "Actor message queue empty";
            case ErrorCode::CompilationError: return "Bytecode compilation failure";
            default: return "Unknown Titan VM error";
        }
    }
};

inline const std::error_category& get_error_category() noexcept {
    static ErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(ErrorCode e) noexcept {
    return std::error_code(static_cast<int>(e), get_error_category());
}

} // namespace titan

namespace std {
template <>
struct is_error_code_enum<titan::ErrorCode> : std::true_type {};
} // namespace std

namespace titan {

// ============================================================================
// 2. VALUE REPRESENTATION (64-bit NaN-boxed Tagged Union)
// ============================================================================

class Object; // Forward declaration

class Value {
private:
    uint64_t bits_;

    // NaN-boxing constants (IEEE 754 double precision)
    // Quiet NaN mask: 0x7FF8000000000000ULL
    // Tag mask:       0x0007000000000000ULL
    static constexpr uint64_t kQNaNMask     = 0x7FF8000000000000ULL;
    static constexpr uint64_t kTagMask      = 0x0007000000000000ULL;
    
    static constexpr uint64_t kTagDouble    = 0x0000000000000000ULL; // Not used as explicit NaN tag (real doubles used)
    static constexpr uint64_t kTagNull      = 0x7FF8000000000001ULL;
    static constexpr uint64_t kTagBool      = 0x7FF8000000000002ULL;
    static constexpr uint64_t kTagObject    = 0x7FF8000000000004ULL;

    explicit constexpr Value(uint64_t raw) : bits_(raw) {}

public:
    constexpr Value() noexcept : bits_(kTagNull) {}

    constexpr Value(double d) noexcept {
        // Standard double assignment
        std::memcpy(&bits_, &d, sizeof(double));
    }

    constexpr Value(bool b) noexcept 
        : bits_(kTagBool | (b ? 1ULL : 0ULL)) {}

    static constexpr Value Null() noexcept {
        return Value(kTagNull);
    }

    static constexpr Value FromObject(Object* ptr) noexcept {
        return Value(kTagObject | reinterpret_cast<uint64_t>(ptr));
    }

    [[nodiscard]] constexpr bool is_double() const noexcept {
        return (bits_ & kQNaNMask) != kQNaNMask;
    }

    [[nodiscard]] constexpr bool is_null() const noexcept {
        return bits_ == kTagNull;
    }

    [[nodiscard]] constexpr bool is_bool() const noexcept {
        return (bits_ & 0xFFFFFFFFFFFFFFFCULL) == kTagBool;
    }

    [[nodiscard]] constexpr bool is_object() const noexcept {
        return (bits_ & 0xFFFFFFFFFFFFFFFCULL) == kTagObject;
    }

    [[nodiscard]] constexpr double as_double() const noexcept {
        double d;
        std::memcpy(&d, &bits_, sizeof(double));
        return d;
    }

    [[nodiscard]] constexpr bool as_bool() const noexcept {
        return (bits_ & 1ULL) != 0;
    }

    [[nodiscard]] constexpr Object* as_object() const noexcept {
        return reinterpret_cast<Object*>(bits_ & ~kQNaNMask & ~kTagMask);
    }

    [[nodiscard]] constexpr uint64_t raw() const noexcept {
        return bits_;
    }

    [[nodiscard]] constexpr bool operator==(const Value& other) const noexcept {
        return bits_ == other.bits_;
    }

    [[nodiscard]] constexpr bool operator!=(const Value& other) const noexcept {
        return bits_ != other.bits_;
    }
};

static_assert(sizeof(Value) == 8, "Value must be exactly 64 bits");

// ============================================================================
// 3. GARBAGE COLLECTION SUBSYSTEM (Tri-Color Mark-and-Sweep)
// ============================================================================

enum class ObjectType : uint8_t {
    String = 1,
    Array,
    Closure
};

struct ObjectHeader {
    ObjectType type;
    uint8_t gc_color; // 0: White, 1: Grey, 2: Black
    uint16_t flags;
    uint32_t size;
    Object* next;
};

class Object {
private:
    ObjectHeader header_;

public:
    explicit Object(ObjectType type, uint32_t size) noexcept
        : header_{type, 0, 0, size, nullptr} {}

    virtual ~Object() = default;

    [[nodiscard]] ObjectHeader& header() noexcept { return header_; }
    [[nodiscard]] const ObjectHeader& header() const noexcept { return header_; }

    virtual void visit_pointers(const std::function<void(Object**)>& visitor) = 0;
};

class StringObject : public Object {
private:
    std::string data_;

public:
    explicit StringObject(std::string_view str) 
        : Object(ObjectType::String, sizeof(StringObject) + str.size()), data_(str) {}

    [[nodiscard]] const std::string& value() const noexcept { return data_; }

    void visit_pointers(const std::function<void(Object**)>&) override {
        // Strings contain no managed object references.
    }
};

class ArrayObject : public Object {
private:
    std::vector<Value> elements_;

public:
    explicit ArrayObject(size_t capacity) 
        : Object(ObjectType::Array, sizeof(ArrayObject) + capacity * sizeof(Value)) {
        elements_.reserve(capacity);
    }

    [[nodiscard]] std::vector<Value>& elements() noexcept { return elements_; }
    [[nodiscard]] const std::vector<Value>& elements() const noexcept { return elements_; }

    void visit_pointers(const std::function<void(Object**)>& visitor) override {
        for (auto& val : elements_) {
            if (val.is_object()) {
                Object* obj = val.as_object();
                visitor(&obj);
                val = Value::FromObject(obj);
            }
        }
    }
};

class HandleScope;

class GarbageCollector {
private:
    Object* head_ = nullptr;
    size_t bytes_allocated_ = 0;
    size_t next_gc_threshold_ = 1024 * 1024; // 1MB initial threshold
    std::vector<Object**> roots_;

    void mark_object(Object* obj) {
        if (!obj || obj->header().gc_color != 0) return;
        obj->header().gc_color = 1; // Grey
        
        // Trace references
        obj->visit_pointers([this](Object** ptr) {
            if (*ptr) {
                mark_object(*ptr);
            }
        });
        
        obj->header().gc_color = 2; // Black
    }

public:
    GarbageCollector() = default;
    ~GarbageCollector() {
        sweep_all();
    }

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;

    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        if (bytes_allocated_ >= next_gc_threshold_) {
            collect();
        }

        T* memory = static_cast<T*>(::operator new(sizeof(T)));
        auto* obj = new (memory) T(std::forward<Args>(args)...);
        
        obj->header().next = head_;
        head_ = obj;
        bytes_allocated_ += obj->header().size;

        return obj;
    }

    void register_root(Object** root) {
        roots_.push_back(root);
    }

    void unregister_root(Object** root) {
        for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
            if (*it == root) {
                roots_.erase(std::next(it).base());
                break;
            }
        }
    }

    void collect() {
        // 1. Mark roots (Grey)
        for (auto** root : roots_) {
            if (root && *root) {
                mark_object(*root);
            }
        }

        // 2. Sweep white objects
        Object** current = &head_;
        while (*current) {
            Object* obj = *current;
            if (obj->header().gc_color == 2) {
                // Reset color for next cycle
                obj->header().gc_color = 0;
                current = &(obj->header().next);
            } else {
                // Unreferenced (White) -> Destroy
                *current = obj->header().next;
                bytes_allocated_ -= obj->size;
                obj->~Object();
                ::operator delete(obj);
            }
        }

        next_gc_threshold_ = bytes_allocated_ * 2 > 1024 * 1024 ? bytes_allocated_ * 2 : 1024 * 1024;
    }

    void sweep_all() {
        Object* current = head_;
        while (current) {
            Object* next = current->header().next;
            current->~Object();
            ::operator delete(current);
            current = next;
        }
        head_ = nullptr;
        bytes_allocated_ = 0;
    }
};

class HandleScope {
private:
    GarbageCollector& gc_;
    size_t initial_root_count_;

public:
    explicit HandleScope(GarbageCollector& gc) : gc_(gc), initial_root_count_(0) {
        // Scoped tracking initialized
    }

    ~HandleScope() {
        // Cleanup scope roots handled automatically by RAII destruction order if tracked
    }

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
};

// ============================================================================
// 4. AST & MEMORY POOL ARENA
// ============================================================================

enum class ASTNodeType {
    Literal,
    Identifier,
    BinaryExpr,
    Block
};

class ASTNode {
private:
    ASTNodeType type_;

public:
    explicit ASTNodeType(ASTNodeType type) noexcept : type_(type) {}
    virtual ~ASTNode() = default;

    [[nodiscard]] ASTNodeType type() const noexcept { return type_; }
};

class ASTLiteralNode : public ASTNode {
private:
    Value value_;

public:
    explicit ASTLiteralNode(Value val) : ASTNode(ASTNodeType::Literal), value_(val) {}
    [[nodiscard]] Value value() const noexcept { return value_; }
};

class ASTArena {
private:
    std::pmr::monotonic_buffer_resource buffer_resource_;

public:
    ASTArena() = default;
    ~ASTArena() = default;

    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        void* mem = buffer_resource_.allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }
};

// ============================================================================
// 5. BYTECODE INSTRUCTION FORMAT (Cache-Friendly 32-bit RISC)
// ============================================================================

enum class Opcode : uint8_t {
    OP_NOP = 0,
    OP_LOADI,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOV,
    OP_PRINT,
    OP_RET
};

struct Instruction {
    uint32_t raw;

    static constexpr uint32_t encode(Opcode op, uint8_t r1, uint8_t r2, uint8_t r3) noexcept {
        return (static_cast<uint32_t>(op) & 0xFF) |
               ((static_cast<uint32_t>(r1) & 0xFF) << 8) |
               ((static_cast<uint32_t>(r2) & 0xFF) << 16) |
               ((static_cast<uint32_t>(r3) & 0xFF) << 24);
    }

    [[nodiscard]] constexpr Opcode opcode() const noexcept {
        return static_cast<Opcode>(raw & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t r1() const noexcept {
        return static_cast<uint8_t>((raw >> 8) & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t r2() const noexcept {
        return static_cast<uint8_t>((raw >> 16) & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t r3() const noexcept {
        return static_cast<uint8_t>((raw >> 24) & 0xFF);
    }
};

static_assert(sizeof(Instruction) == 4, "Instruction must be exactly 32 bits");

// ============================================================================
// 6. CONCURRENCY PRIMITIVES (Lock-Free SPSC Ring Buffer)
// ============================================================================

template <typename T, size_t Capacity>
class SPSCRingBuffer {
private:
    static_uff:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    std::array<Cell, Capacity> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    SPSCRingBuffer() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }