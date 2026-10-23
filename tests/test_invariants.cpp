#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <algorithm>

// ============================================================================
// TITAN-VM-ENGINE: Implementation Mock & Interface Specification
// ============================================================================
namespace TitanVM {

using Address = uint64_t;
using Word = uint64_t;

enum class ErrorCode {
    Success = 0,
    OutOfBounds,
    DivisionByZero,
    InvalidOpcode,
    StackUnderflow,
    StackOverflow,
    MutexPoisoned,
    ConcurrentFault
};

enum class Opcode : uint8_t {
    NOP = 0x00,
    PUSH = 0x01,
    POP = 0x02,
    ADD = 0x03,
    SUB = 0x04,
    DIV = 0x05,
    JMP = 0x06,
    HALT = 0xFF
};

struct Instruction {
    Opcode op;
    Word arg;
};

class VMState {
public:
    std::atomic<bool> halted{false};
    std::atomic<Word> pc{0};
    std::atomic<Word> sp{0};
    std::atomic<uint32_t> error_code{static_cast<uint32_t>(ErrorCode::Success)};
};

class MemoryBus {
private:
    size_t size_;
    std::unique_ptr<Word[]> memory_;
    mutable std::shared_mutex mutex_; // Simplified to mutex simulation for single-header portability

public:
    explicit MemoryBus(size_t size) : size_(size), memory_(std::make_unique<Word[]>(size)) {
        std::memset(memory_.get(), 0, size * sizeof(Word));
    }

    ErrorCode read(Address addr, Word& out) const {
        if (addr >= size_) return ErrorCode::OutOfBounds;
        out = memory_[addr];
        return ErrorCode::Success;
    }

    ErrorCode write(Address addr, Word val) {
        if (addr >= size_) return ErrorCode::OutOfBounds;
        memory_[addr] = val;
        return ErrorCode::Success;
    }

    size_t size() const { return size_; }
    void clear() { std::memset(memory_.get(), 0, size_ * sizeof(Word)); }
};

class Engine {
private:
    VMState state_;
    MemoryBus memory_;
    std::vector<Word> stack_;
    size_t stack_capacity_;
    mutable std::mutex engine_mutex_;

public:
    Engine(size_t mem_size, size_t stack_cap) 
        : memory_(mem_size), stack_capacity_(stack_cap) {
        stack_.reserve(stack_cap);
    }

    ErrorCode execute_instruction(const Instruction& inst) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        if (state_.halted.load()) return ErrorCode::Success;

        switch (inst.op) {
            case Opcode::NOP:
                break;

            case Opcode::PUSH:
                if (stack_.size() >= stack_capacity_) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::StackOverflow));
                    state_.halted.store(true);
                    return ErrorCode::StackOverflow;
                }
                stack_.push_back(inst.arg);
                state_.sp.store(stack_.size());
                break;

            case Opcode::POP:
                if (stack_.empty()) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::StackUnderflow));
                    state_.halted.store(true);
                    return ErrorCode::StackUnderflow;
                }
                stack_.pop_back();
                state_.sp.store(stack_.size());
                break;

            case Opcode::ADD: {
                if (stack_.size() < 2) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::StackUnderflow));
                    state_.halted.store(true);
                    return ErrorCode::StackUnderflow;
                }
                Word b = stack_.back(); stack_.pop_back();
                Word a = stack_.back(); stack_.pop_back();
                stack_.push_back(a + b);
                state_.sp.store(stack_.size());
                break;
            }

            case Opcode::SUB: {
                if (stack_.size() < 2) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::StackUnderflow));
                    state_.halted.store(true);
                    return ErrorCode::StackUnderflow;
                }
                Word b = stack_.back(); stack_.pop_back();
                Word a = stack_.back(); stack_.pop_back();
                stack_.push_back(a - b);
                state_.sp.store(stack_.size());
                break;
            }

            case Opcode::DIV: {
                if (stack_.size() < 2) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::StackUnderflow));
                    state_.halted.store(true);
                    return ErrorCode::StackUnderflow;
                }
                Word b = stack_.back(); stack_.pop_back();
                Word a = stack_.back(); stack_.pop_back();
                if (b == 0) {
                    state_.error_code.store(static_cast<uint32_t>(ErrorCode::DivisionByZero));
                    state_.halted.store(true);
                    return ErrorCode::DivisionByZero;
                }
                stack_.push_back(a / b);
                state_.sp.store(stack_.size());
                break;
            }

            case Opcode::JMP:
                state_.pc.store(inst.arg);
                break;

            case Opcode::HALT:
                state_.halted.store(true);
                break;

            default:
                state_.error_code.store(static_cast<uint32_t>(ErrorCode::InvalidOpcode));
                state_.halted.store(true);
                return ErrorCode::InvalidOpcode;
        }

        state_.pc.fetch_add(1);
        return ErrorCode::Success;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        state_.halted.store(false);
        state_.pc.store(0);
        state_.sp.store(0);
        state_.error_code.store(static_cast<uint32_t>(ErrorCode::Success));
        stack_.clear();
        memory_.clear();
    }

    VMState& get_state() { return state_; }
    MemoryBus& get_memory() { return memory_; }
    
    size_t get_stack_size() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        return stack_.size();
    }
};

} // namespace TitanVM

// ============================================================================
// TEST SUITE: titan-vm-engine
// ============================================================================
namespace TitanVMTests {

class TestRunner {
private:
    int passed_{0};
    int failed_{0};

public:
    void assert_true(bool condition, const std::string& name) {
        if (condition) {
            passed_++;
            std::cout << "[PASS] " << name << "\n";
        } else {
            failed_++;
            std::cerr << "[FAIL] " << name << "\n";
            std::abort();
        }
    }

    void report() const {
        std::cout << "\n============================================\n";
        std::cout << "Test Suites Completed.\n";
        std::cout << "Passed: " << passed_ << "\n";
        std::cout << "Failed: " << failed_ << "\n";
        std::cout << "============================================\n";
    }
};

// 1. Boundary and Edge-Case Assertions
void run_unit_tests(TestRunner& runner) {
    std::cout << "\n--- Running Unit & Boundary Tests ---\n";
    
    TitanVM::Engine vm(1024, 16);

    // Initial state check
    runner.assert_true(vm.get_state().halted.load() == false, "Initial state unhalted");
    runner.assert_true(vm.get_state().pc.load() == 0, "Initial PC is zero");
    runner.assert_true(vm.get_state().sp.load() == 0, "Initial SP is zero");

    // Memory bounds checking
    TitanVM::Word val = 0;
    runner.assert_true(vm.get_memory().read(0, val) == TitanVM::ErrorCode::Success, "Memory read valid lower bound");
    runner.assert_true(vm.get_memory().read(1023, val) == TitanVM::ErrorCode::Success, "Memory read valid upper bound");
    runner.assert_true(vm.get_memory().read(1024, val) == TitanVM::ErrorCode::OutOfBounds, "Memory read out of bounds check");
    runner.assert_true(vm.get_memory().write(1024, 42) == TitanVM::ErrorCode::OutOfBounds, "Memory write out of bounds check");

    // Stack Underflow
    TitanVM::Instruction pop_inst{TitanVM::Opcode::POP, 0};
    runner.assert_true(vm.execute_instruction(pop_inst) == TitanVM::ErrorCode::StackUnderflow, "Stack underflow caught");
    runner.assert_true(vm.get_state().halted.load() == true, "VM halts on underflow");

    // Reset recovery verification
    vm.reset();
    runner.assert_true(vm.get_state().halted.load() == false, "VM recovers after reset (unhalted)");
    runner.assert_true(vm.get_state().error_code.load() == static_cast<uint32_t>(TitanVM::ErrorCode::Success), "Error code cleared on reset");

    // Stack Overflow
    for (int i = 0; i < 16; ++i) {
        vm.execute_instruction({TitanVM::Opcode::PUSH, static_cast<TitanVM::Word>(i)});
    }
    runner.assert_true(vm.get_stack_size() == 16, "Stack filled to capacity");
    runner.assert_true(vm.execute_instruction({TitanVM::Opcode::PUSH, 99}) == TitanVM::ErrorCode::StackOverflow, "Stack overflow caught");

    // Arithmetic: Division by zero
    vm.reset();
    vm.execute_instruction({TitanVM::Opcode::PUSH, 10});
    vm.execute_instruction({TitanVM::Opcode::PUSH, 0});
    runner.assert_true(vm.execute_instruction({TitanVM::Opcode::DIV, 0}) == TitanVM::ErrorCode::DivisionByZero, "Division by zero caught");
}

// 2. Invariant Verification Tests
void run_invariant_tests(TestRunner& runner) {
    std::cout << "\n--- Running Invariant Verification Tests ---\n";

    TitanVM::Engine vm(512, 32);

    // Invariant: Stack pointer always matches internal container size
    for (int i = 0; i < 10; ++i) {
        vm.execute_instruction({TitanVM::Opcode::PUSH, static_cast<TitanVM::Word>(i)});
        runner.assert_true(vm.get_state().sp.load() == vm.get_stack_size(), "Invariant: SP matches stack size on PUSH");
    }

    for (int i = 0; i < 10; ++i) {
        vm.execute_instruction({TitanVM::Opcode::POP, 0});
        runner.assert_true(vm.get_state().sp.load() == vm.get_stack_size(), "Invariant: SP matches stack size on POP");
    }

    // Invariant: Arithmetic validity (A + B - B = A)
    vm.reset();
    vm.execute_instruction({TitanVM::Opcode::PUSH, 42});
    vm.execute_instruction({TitanVM::Opcode::PUSH, 8});
    vm.execute_instruction({TitanVM::Opcode::ADD, 0}); // 50
    vm.execute_instruction({TitanVM::Opcode::