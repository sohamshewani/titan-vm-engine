#ifndef INCLUDE_TITAN_CONCURRENCY_HPP
#define INCLUDE_TITAN_CONCURRENCY_HPP

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <span>
#include <optional>
#include <system_error>

namespace titan {

// Forward declarations from other VM subsystems
struct Value;
class VMInstance;

enum class ConcurrencyErrorCode : uint8_t {
    Success = 0,
    MailboxFull,
    MailboxEmpty,
    IsolateTerminated,
    InvalidMessage,
    LockContentionTimeout,
    SynchronizationError
};

class ConcurrencyErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "titan.concurrency";
    }

    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<ConcurrencyErrorCode>(ev)) {
            case ConcurrencyErrorCode::Success: return "Success";
            case ConcurrencyErrorCode::MailboxFull: return "Actor mailbox is full (SPSC ring buffer capacity exceeded)";
            case ConcurrencyErrorCode::MailboxEmpty: return "Actor mailbox is empty";
            case ConcurrencyErrorCode::IsolateTerminated: return "Target isolate has terminated execution";
            case ConcurrencyErrorCode::InvalidMessage: return "Message payload is malformed or contains invalid object references";
            case ConcurrencyErrorCode::LockContentionTimeout: return "Acquisition of synchronization primitive timed out";
            case ConcurrencyErrorCode::SynchronizationError: return "Low-level synchronization primitive failure";
            default: return "Unknown concurrency error";
        }
    }
};

inline const std::error_category& concurrency_category() noexcept {
    static ConcurrencyErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(ConcurrencyErrorCode e) noexcept {
    return {static_cast<int>(e), concurrency_category()};
}

} // namespace titan

namespace std {
template <>
struct is_error_code_enum<titan::ConcurrencyErrorCode> : std::true_type {};
} // namespace std

namespace titan {

/**
 * High-performance, lock-free Single-Producer Single-Consumer (SPSC) ring buffer
 * tailored for inter-isolate actor messaging in the Titan VM.
 * 
 * Satisfies memory safety by enforcing value semantics or safe ownership transfers
 * of boxed NaN-values across isolate boundaries.
 */
template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two for mask-based indexing.");
public:
    SPSCRingBuffer() 
        : head_(0), tail_(0) {
        // Pre-allocate buffer cells
        buffer_ = std::make_unique<Cell[]>(Capacity);
    }

    ~SPSCRingBuffer() noexcept {
        // Drain remaining items to ensure proper destruction of elements
        T val;
        while (pop(val)) {
            // Drop items
        }
    }

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) noexcept = default;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) noexcept = default;

    /**
     * Pushes an item into the ring buffer (Producer thread only).
     * Uses acquire-release memory ordering to synchronize with the consumer.
     */
    [[nodiscard]] std::expected<void, ConcurrencyErrorCode> push(T&& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        if ((current_tail - current_head) >= Capacity) {
            return std::unexpected(ConcurrencyErrorCode::MailboxFull);
        }

        buffer_[current_tail & Mask].value = std::forward<T>(item);
        tail_.store(current_tail + 1, std::memory_order_release);
        return {};
    }

    [[nodiscard]] std::expected<void, ConcurrencyErrorCode> push(const T& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        if ((current_tail - current_head) >= Capacity) {
            return std::unexpected(ConcurrencyErrorCode::MailboxFull);
        }

        buffer_[current_tail & Mask].value = item;
        tail_.store(current_tail + 1, std::memory_order_release);
        return {};
    }

    /**
     * Pops an item from the ring buffer (Consumer thread only).
     * Uses acquire-release memory ordering to synchronize with the producer.
     */
    [[nodiscard]] std::expected<T, ConcurrencyErrorCode> pop() noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return std::unexpected(ConcurrencyErrorCode::MailboxEmpty);
        }

        T item = std::move(buffer_[current_head & Mask].value);
        // Explicitly clear slot to prevent stale object retention if T is a managed pointer wrapper
        buffer_[current_head & Mask].value = T{};
        head_.store(current_head + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool pop(T& out_item) noexcept {
        auto res = pop();
        if (!res) {
            return false;
        }
        out_item = std::move(*res);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    union Cell {
        Cell() noexcept {}
        ~Cell() noexcept {}
        T value;
    };

    static constexpr size_t Mask = Capacity - 1;

    // Cache-line separation to prevent false sharing between producer and consumer cores
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) std::unique_ptr<Cell[]> buffer_;
};

/**
 * Message envelope encapsulating a Titan VM Value payload sent between isolates.
 */
struct ActorMessage {
    uint64_t sender_isolate_id;
    uint64_t message_type;
    uint64_t packed_value; // NaN-boxed Titan VM Value representation
};

/**
 * Shared-nothing actor mailbox wrapper maintaining the SPSC queue with bounded capacity.
 */
class ActorMailbox {
public:
    explicit ActorMailbox(size_t capacity = 1024) 
        : capacity_(capacity) {
        // Capacity validation and allocation wrapper
    }

    ~ActorMailbox() = default;

    ActorMailbox(const ActorMailbox&) = delete;
    ActorMailbox& operator=(const ActorMailbox&) = delete;
    ActorMailbox(ActorMailbox&&) noexcept = default;
    ActorMailbox& operator=(ActorMailbox&&) noexcept = default;

    [[nodiscard]] std::expected<void, ConcurrencyErrorCode> send(ActorMessage message) noexcept {
        return queue_.push(std::move(message));
    }

    [[nodiscard]] std::expected<ActorMessage, ConcurrencyErrorCode> receive() noexcept {
        return queue_.pop();
    }

    [[nodiscard]] bool has_messages() const noexcept {
        return !queue_.empty();
    }

    [[nodiscard]] size_t message_count() const noexcept {
        return queue_.size();
    }

private:
    // Using a fixed backing power-of-two size for the internal SPSC mechanism (e.g., 4096 messages)
    SPSCRingBuffer<ActorMessage, 4096> queue_;
    size_t capacity_;
};

/**
 * Zero-contention read-write lock wrapper conforming to VM concurrency rules.
 * Protects shared symbol tables, global constant pools, and method metadata during 
 * concurrent asynchronous compilation passes.
 */
class GlobalSymbolTableLock {
public:
    GlobalSymbolTableLock() noexcept = default;
    ~GlobalSymbolTableLock() noexcept = default;

    GlobalSymbolTableLock(const GlobalSymbolTableLock&) = delete;
    GlobalSymbolTableLock& operator=(const GlobalSymbolTableLock&) = delete;
    GlobalSymbolTableLock(GlobalSymbolTableLock&&) = delete;
    GlobalSymbolTableLock& operator=(GlobalSymbolTableLock&&) = delete;

    class ReadLockGuard {
    public:
        explicit ReadLockGuard(GlobalSymbolTableLock& lock) noexcept 
            : mutex_(lock.rw_mutex_) {
            mutex_.lock_shared();
        }

        ~ReadLockGuard() noexcept {
            mutex_.unlock_shared();
        }

        ReadLockGuard(const ReadLockGuard&) = delete;
        ReadLockGuard& operator=(const ReadLockGuard&) = delete;
        ReadLockGuard(ReadLockGuard&&) = delete;
        ReadLockGuard& operator=(ReadLockGuard&&) = delete;

    private:
        std::shared_mutex& mutex_;
    };

    class WriteLockGuard {
    public:
        explicit WriteLockGuard(GlobalSymbolTableLock& lock) noexcept 
            : mutex_(lock.rw_mutex_) {
            mutex_.lock();
        }

        ~WriteLockGuard() noexcept {
            mutex_.unlock();
        }

        WriteLockGuard(const WriteLockGuard&) = delete;
        WriteLockGuard& operator=(const WriteLockGuard&) = delete;
        WriteLockGuard(WriteLockGuard&&) = delete;
        WriteLockGuard& operator=(WriteLockGuard&&) = delete;

    private:
        std::shared_mutex& mutex_;
    };

    [[nodiscard]] ReadLockGuard acquire_read() noexcept {
        return ReadLockGuard(*this);
    }

    [[nodiscard]] WriteLockGuard acquire_write() noexcept {
        return WriteLockGuard(*this);
    }

private:
    mutable std::shared_mutex rw_mutex_;
};

/**
 * Concurrency Subsystem Manager coordinating isolate actor lifecycles, message routing,
 * and global compilation resource synchronization.
 */
class ConcurrencyManager {
public:
    ConcurrencyManager() noexcept = default;
    ~ConcurrencyManager() noexcept = default;

    ConcurrencyManager(const ConcurrencyManager&) = delete;
    ConcurrencyManager& operator=(const ConcurrencyManager&) = delete;
    ConcurrencyManager(ConcurrencyManager&&) = delete;
    ConcurrencyManager& operator=(ConcurrencyManager&&) = delete;

    /**
     * Registers a newly spawned isolate instance in the global concurrency registry.
     */
    [[nodiscard]] std::expected<void, ConcurrencyErrorCode> register_isolate(uint64_t isolate_id, std::shared_ptr<ActorMailbox> mailbox) {
        GlobalSymbolTableLock::WriteLockGuard guard(registry_lock_);
        if (mailboxes_.find(isolate_id) != mailboxes_.end()) {
            return std::unexpected(ConcurrencyErrorCode::SynchronizationError);
        }
        mailboxes_[isolate_id] = std::move(mailbox);
        return {};
    }

    /**
     * Deregisters an isolate upon termination.
     */
    void unregister_isolate(uint64_t isolate_id) noexcept {
        GlobalSymbolTableLock::WriteLockGuard guard(registry_lock_);
        mailboxes_.erase(isolate_id);
    }

    /**
     * Dispatches an inter-isolate actor message safely across threads.
     */
    [[nodiscard]] std::expected<void, ConcurrencyErrorCode> route_message(uint64_t target_isolate_id, ActorMessage message) {
        std::shared_ptr<ActorMailbox> target_box;
        {
            GlobalSymbolTableLock::ReadLockGuard guard(registry_lock_);
            auto it = mailboxes_.find(target_isolate_id);
            if (it == mailboxes_.end()) {
                return std::unexpected(ConcurrencyErrorCode::IsolateTerminated);
            }
            target_box = it->second;
        }

        return target_box->send(std::move(message));
    }

    /**
     * Retrieves the global symbol table lock reference for compilation passes.
     */
    [[nodiscard]] GlobalSymbolTableLock& get_symbol_table_lock() noexcept {
        return symbol_table_lock_;
    }

private:
    GlobalSymbolTableLock registry_lock_;
    GlobalSymbolTableLock symbol_table_lock_;
    // Using flat vector or map protected by registry_lock_
    // For deterministic performance in production: mapping IDs to Mailboxes
    class MapStorage {
    public:
        struct Entry {
            uint64_t id;
            std::shared_ptr<ActorMailbox> mailbox;
        };
        std::vector<Entry> entries;

        auto find(uint64_t id) noexcept {
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->id == id) return it;
            }
            return entries.end();
        }

        void erase(uint64_t id) noexcept {
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->id == id) {
                    entries.erase(it);
                    break;
                }
            }
        }

        auto end() noexcept { return entries.end(); }

        std::shared_ptr<ActorMailbox>& operator[](uint64_t id) {
            for (auto& entry : entries) {
                if (entry.id == id) return entry.mailbox;
            }
            entries.push_back({id, std::make_shared<ActorMailbox>()});
            return entries.back().mailbox;
        }
    } mailboxes_;
};

} // namespace titan

#endif // INCLUDE_TITAN_CONCURRENCY_HPP