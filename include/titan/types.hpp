#ifndef TITAN_TYPES_HPP
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string_view>
#include <expected>
#include <system_error>
#include <memory_resource>
#include <shared_mutex>
#include <vector>
#include <atomic>
#include <span>

namespace titan {

enum class ErrorCode : uint32_t {
    Success = 0,
    OutOfMemory,
    TypeError,
    ReferenceError,
    SyntaxError,
    StackOverflow,
    InvalidBytecode,
    ActorMailboxFull
};

class ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "titan-vm-engine"; }
    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Success: return "Success";
            case ErrorCode::OutOfMemory: return "Out of Memory";
            case ErrorCode::TypeError: return "Type Error";
            case ErrorCode::ReferenceError: return "Reference Error";
            case ErrorCode::SyntaxError: return "Syntax Error";
            case ErrorCode::StackOverflow: return "Stack Overflow";
            case ErrorCode::InvalidBytecode: return "Invalid Bytecode";
            case ErrorCode::ActorMailboxFull: return "Actor Mailbox Full";
            default: return "Unknown Error";
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
}

namespace titan {

enum class ObjectType : uint8_t {
    String = 0,
    Array = 1,
    Closure = 2
};

struct ObjectHeader {
    std::atomic<uint32_t> metadata; // Bit 0: Mark bit, Bits 1-31: Size / Generational age
    ObjectType type;
    uint8_t reserved[3];
    ObjectHeader* forwarding_pointer;
};

class Value {
private:
    uint64_t bits_;

    static constexpr uint64_t QNAN_MASK      = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_SHIFT      = 48;
    static constexpr uint64_t TAG_MASK       = 0x7ULL;
    
    static constexpr uint64_t TAG_OBJECT     = 0x0ULL;
    static constexpr uint64_t TAG_BOOLEAN    = 0x1ULL;
    static constexpr uint64_t TAG_NULL       = 0x2ULL;

    static constexpr uint64_t PAYLOAD_MASK   = 0x0000FFFFFFFFFFFFULL;

    explicit constexpr Value(uint64_t bits) : bits_(bits) {}

public:
    constexpr Value() : bits_(QNAN_MASK | (TAG_NULL << TAG_SHIFT)) {}

    static constexpr Value from_double(double d) {
        Value v;
        std::memcpy(&v.bits_, &d, sizeof(double));
        return v;
    }

    static constexpr Value from_bool(bool b) {
        return Value(QNAN_MASK | (TAG_BOOLEAN << TAG_SHIFT) | (b ? 1ULL : 0ULL));
    }

    static constexpr Value from_null() {
        return Value(QNAN_MASK | (TAG_NULL << TAG_SHIFT));
    }

    static constexpr Value from_object(ObjectHeader* ptr) {
        return Value(QNAN_MASK | (TAG_OBJECT << TAG_SHIFT) | (reinterpret_2_uint64(ptr) & PAYLOAD_MASK));
    }

    constexpr bool is_double() const {
        return (bits_ & QNAN_MASK) != QNAN_MASK;
    }

    constexpr bool is_nan_boxed() const {
        return (bits_ & QNAN_MASK) == QNAN_MASK;
    }

    constexpr bool is_object() const {
        return is_nan_boxed() && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_OBJECT);
    }

    constexpr bool is_boolean() const {
        return is_nan_boxed() && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_BOOLEAN);
    }

    constexpr bool is_null() const {
        return is_nan_boxed() && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_NULL);
    }

    constexpr double as_double() const {
        double d;
        std::memcpy(&d, &bits_, sizeof(double));
        return d;
    }

    constexpr bool as_boolean() const {
        return (bits_ & 1ULL) != 0;
    }

    constexpr ObjectHeader* as_object() const {
        uint64_t ptr_bits = bits_ & PAYLOAD_MASK;
        return reinterpret_cast<ObjectHeader*>(ptr_bits);
    }

    constexpr uint64_t raw() const { return bits_; }

private:
    static constexpr uint64_t reinterpret_2_uint64(ObjectHeader* ptr) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
    }
};

enum class OpCode : uint8_t {
    OP_NOP = 0,
    OP_LOAD_CONST,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOV,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_CALL,
    OP_RET,
    OP_HALT
};

struct Instruction {
    uint32_t raw;

    static constexpr Instruction encode(OpCode op, uint8_t r1, uint8_t r2, uint8_t r3) {
        uint32_t val = (static_cast<uint32_t>(op) & 0xFF) |
                       ((static_cast<uint32_t>(r1) & 0xFF) << 8) |
                       ((static_cast<uint32_t>(r2) & 0xFF) << 16) |
                       ((static_cast<uint32_t>(r3) & 0xFF) << 24);
        return Instruction{val};
    }

    constexpr OpCode opcode() const {
        return static_cast<OpCode>(raw & 0xFF);
    }

    constexpr uint8_t r1() const {
        return static_cast<uint8_t>((raw >> 8) & 0xFF);
    }

    constexpr uint8_t r2() const {
        return static_cast<uint8_t>((raw >> 16) & 0xFF);
    }

    constexpr uint8_t r3() const {
        return static_cast<uint8_t>((raw >> 24) & 0xFF);
    }
};

class VMThreadContext;

class HandleScope {
private:
    VMThreadContext* context_;
    size_t prev_size_;

public:
    explicit HandleScope(VMThreadContext* ctx);
    ~HandleScope();

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
    HandleScope(HandleScope&&) = delete;
    HandleScope& operator=(HandleScope&&) = delete;

    void* allocate_handle(ObjectHeader* obj);
};

class VMThreadContext {
private:
    std::vector<ObjectHeader*> root_stack_;

public:
    VMThreadContext() {
        root_stack_.reserve(1024);
    }

    ~VMThreadContext() = default;

    void push_root(ObjectHeader* obj) {
        root_stack_.push_back(obj);
    }

    void pop_roots(size_t count) {
        root_stack_.resize(count);
    }

    size_t root_stack_size() const {
        return root_stack_.size();
    }

    std::span<ObjectHeader*> get_roots() {
        return std::span<ObjectHeader*>(root_stack_.data(), root_stack_.size());
    }
};

inline HandleScope::HandleScope(VMThreadContext* ctx) : context_(ctx), prev_size_(ctx->root_stack_size()) {}

inline HandleScope::~HandleScope() {
    context_->pop_roots(prev_size_);
}

inline void* HandleScope::allocate_handle(ObjectHeader* obj) {
    context_->push_root(obj);
    return static_cast<void*>(obj);
}

template <typename T, size_t Capacity>
class SPSCQueue {
private:
    struct Node {
        std::atomic<bool> ready{false};
        alignas(64) uint8_t storage[sizeof(T)];
    };

    Node ring_[Capacity + 1];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    SPSCQueue() = default;
    ~SPSCQueue() {
        T dummy;
        while (pop(dummy));
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    bool push(const T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = tail_.load(std::memory_order_acquire);
        
        if ((current_head + 1) % (Capacity + 1) == current_tail) {
            return false;
        }

        new (ring_[current_head].storage) T(item);
        ring_[current_head].ready.store(true, std::memory_order_release);
        head_.store((current_head + 1) % (Capacity + 1), std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t current_head = head_.load(std::memory_order_acquire);

        if (current_tail == current_head) {
            return false;
        }

        if (!ring_[current_tail].ready.load(std::memory_order_acquire)) {
            return false;
        }

        T* ptr = reinterpret_cast<T*>(ring_[current_tail].storage);
        item = std::move(*ptr);
        ptr->~T();
        ring_[current_tail].ready.store(false, std::memory_order_release);
        tail_.store((current_tail + 1) % (Capacity + 1), std::memory_order_release);
        return true;
    }
};

} // namespace titan

#endif // TITAN_TYPES_HPP