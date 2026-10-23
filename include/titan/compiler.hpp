#ifndef TITAN_COMPILER_HPP
#define TITAN_COMPILER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <memory_resource>
#include <expected>
#include <system_error>
#include <variant>
#include <unordered_map>
#include <array>
#include <shared_mutex>
#include <concepts>
#include <type_traits>

namespace titan {

// ============================================================================
// 1. Error Handling & Monadic Types
// ============================================================================

enum class ErrorCode : uint32_t {
    Success = 0,
    // Lexer errors
    UnexpectedCharacter,
    UnterminatedString,
    InvalidNumberLiteral,
    // Parser errors
    UnexpectedToken,
    ExpectedExpression,
    ExpectedIdentifier,
    ExpectedSemicolon,
    ExpectedClosingParen,
    ExpectedClosingBrace,
    // Compiler/Codegen errors
    SymbolNotFound,
    SymbolRedefinition,
    TooManyRegisters,
    TooManyConstants,
    JumpTargetTooFar,
    AllocationFailure
};

class ErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "titan.compiler"; }
    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::Success: return "Success";
            case ErrorCode::UnexpectedCharacter: return "Unexpected character in source";
            case ErrorCode::UnterminatedString: return "Unterminated string literal";
            case ErrorCode::InvalidNumberLiteral: return "Invalid numeric literal format";
            case ErrorCode::UnexpectedToken: return "Unexpected token encountered";
            case ErrorCode::ExpectedExpression: return "Expected an expression";
            case ErrorCode::ExpectedIdentifier: return "Expected an identifier";
            case ErrorCode::ExpectedSemicolon: return "Expected ';' after statement";
            case ErrorCode::ExpectedClosingParen: return "Expected ')'";
            case ErrorCode::ExpectedClosingBrace: return "Expected '}'";
            case ErrorCode::SymbolNotFound: return "Undeclared symbol reference";
            case ErrorCode::SymbolRedefinition: return "Redefinition of existing symbol";
            case ErrorCode::TooManyRegisters: return "Exhausted available virtual registers in frame";
            case ErrorCode::TooManyConstants: return "Exhausted constant pool capacity (max 65535)";
            case ErrorCode::JumpTargetTooFar: return "Branch offset exceeds instruction range";
            case ErrorCode::AllocationFailure: return "Memory allocation failed in arena or heap";
        }
        return "Unknown compilation error";
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

struct SourceLocation {
    uint32_t line = 1;
    uint32_t column = 1;
    uint32_t offset = 0;
};

struct CompileError {
    ErrorCode code;
    SourceLocation location;
    std::string message;

    CompileError(ErrorCode c, SourceLocation loc, std::string msg)
        : code(c), location(loc), message(std::move(msg)) {}
};

template <typename T>
using CompResult = std::expected<T, CompileError>;

// ============================================================================
// 2. Data Structures & Memory Layout (Values & Bytecode)
// ============================================================================

enum class ValueType : uint8_t {
    Double  = 0,
    Boolean = 1,
    Null    = 2,
    Object  = 3
};

class Value {
private:
    uint64_t bits_;

    static constexpr uint64_t QNAN      = 0x7FFC000000000000ULL;
    static constexpr uint64_t TAG_SHIFT = 48;
    static constexpr uint64_t TAG_MASK  = 0x3ULL;
    static constexpr uint64_t PAYLOAD_MASK = 0x0000FFFFFFFFFFFFULL;

    static constexpr uint64_t TAG_BOOL  = 0x1ULL;
    static constexpr uint64_t TAG_NULL  = 0x2ULL;
    static constexpr uint64_t TAG_OBJ   = 0x3ULL;

public:
    constexpr Value() noexcept : bits_(QNAN | (TAG_NULL << TAG_SHIFT)) {}
    constexpr explicit Value(double d) noexcept {
        __builtin_memcpy(&bits_, &d, sizeof(double));
    }
    constexpr explicit Value(bool b) noexcept 
        : bits_(QNAN | (TAG_BOOL << TAG_SHIFT) | (b ? 1ULL : 0ULL)) {}
    
    static constexpr Value Null() noexcept {
        return Value(QNAN | (TAG_NULL << TAG_SHIFT));
    }

    static constexpr Value FromObject(void* ptr) noexcept {
        return Value(QNAN | (TAG_OBJ << TAG_SHIFT) | (reinterpret_cast<uint64_t>(ptr) & PAYLOAD_MASK));
    }

    [[nodiscard]] constexpr ValueType GetType() const noexcept {
        if ((bits_ & QNAN) != QNAN) {
            return ValueType::Double;
        }
        uint64_t tag = (bits_ >> TAG_SHIFT) & TAG_MASK;
        switch (tag) {
            case TAG_BOOL: return ValueType::Boolean;
            case TAG_NULL: return ValueType::Null;
            case TAG_OBJ:  return ValueType::Object;
            default:       return ValueType::Double; // Canonical NaN
        }
    }

    [[nodiscard]] constexpr double AsDouble() const noexcept {
        double d;
        __builtin_memcpy(&d, &bits_, sizeof(double));
        return d;
    }

    [[nodiscard]] constexpr bool AsBoolean() const noexcept {
        return (bits_ & 1ULL) != 0;
    }

    [[nodiscard]] constexpr void* AsObject() const noexcept {
        return reinterpret_cast<void*>(bits_ & PAYLOAD_MASK);
    }

    [[nodiscard]] constexpr uint64_t Raw() const noexcept { return bits_; }

private:
    constexpr explicit Value(uint64_t raw) noexcept : bits_(raw) {}
};

enum class Opcode : uint8_t {
    OP_NOP = 0,
    OP_LOAD_CONST,
    OP_LOAD_NULL,
    OP_LOAD_BOOL,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_EQ,
    OP_LT,
    OP_LE,
    OP_JMP,
    OP_JMP_TRUE,
    OP_JMP_FALSE,
    OP_CALL,
    OP_RET,
    OP_PRINT
};

struct Instruction {
    uint32_t raw;

    static constexpr Instruction Encode(Opcode op, uint8_t r1, uint8_t r2, uint8_t r3) noexcept {
        uint32_t val = (static_cast<uint32_t>(op) & 0xFF) |
                       ((static_cast<uint32_t>(r1) & 0xFF) << 8) |
                       ((static_cast<uint32_t>(r2) & 0xFF) << 16) |
                       ((static_cast<uint32_t>(r3) & 0xFF) << 24);
        return Instruction{val};
    }

    static constexpr Instruction EncodeAB(Opcode op, uint8_t r1, uint16_t imm16) noexcept {
        uint32_t val = (static_cast<uint32_t>(op) & 0xFF) |
                       ((static_cast<uint32_t>(r1) & 0xFF) << 8) |
                       ((static_cast<uint32_t>(imm16) & 0xFFFF) << 16);
        return Instruction{val};
    }

    [[nodiscard]] constexpr Opcode GetOpcode() const noexcept {
        return static_cast<Opcode>(raw & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t GetR1() const noexcept {
        return static_cast<uint8_t>((raw >> 8) & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t GetR2() const noexcept {
        return static_cast<uint8_t>((raw >> 16) & 0xFF);
    }

    [[nodiscard]] constexpr uint8_t GetR3() const noexcept {
        return static_cast<uint8_t>((raw >> 24) & 0xFF);
    }

    [[nodiscard]] constexpr uint16_t GetImm16() const noexcept {
        return static_cast<uint16_t>((raw >> 16) & 0xFFFF);
    }
} __attribute__((aligned(4)));

static_assert(sizeof(Instruction) == 4, "Instruction must be exactly 32 bits");

// ============================================================================
// 3. AST Subsystem (Arena Allocated)
// ============================================================================

enum class ASTNodeType {
    Program,
    VarDecl,
    Assignment,
    BinaryExpr,
    UnaryExpr,
    Literal,
    Identifier,
    IfStmt,
    WhileStmt,
    Block,
    ReturnStmt,
    PrintStmt
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Eq, Ne, Lt, Le, Gt, Ge, And, Or
};

enum class UnaryOp {
    Negate, Not
};

class ASTNode {
public:
    ASTNodeType type;
    SourceLocation location;

    explicit ASTNode(ASTNodeType t, SourceLocation loc) noexcept : type(t), location(loc) {}
    virtual ~ASTNode() = default;

    static void* operator new(size_t size, std::pmr::memory_resource* mr) {
        return mr->allocate(size, alignof(ASTNode));
    }
    
    // Disable global delete operator matching placement new requirements
    static void operator delete(void*, size_t) noexcept = delete;
    static void operator delete(void*, std::pmr::memory_resource*) noexcept = delete;
};

class ProgramNode : public ASTNode {
public:
    std::pmr::vector<ASTNode*> statements;

    ProgramNode(std::pmr::memory_resource* mr, SourceLocation loc)
        : ASTNode(ASTNodeType::Program, loc), statements(mr) {}
};

class VarDeclNode : public ASTNode {
public:
    std::string_view name;
    ASTNode* initializer;

    VarDeclNode(std::string_view n, ASTNode* init, SourceLocation loc)
        : ASTNode(ASTNodeType::VarDecl, loc), name(n), initializer(init) {}
};

class LiteralNode : public ASTNode {
public:
    Value value;

    LiteralNode(Value val, SourceLocation loc)
        : ASTNode(ASTNodeType::Literal, loc), value(val) {}
};

class IdentifierNode : public ASTNode {
public:
    std::string_view name;

    IdentifierNode(std::string_view n, SourceLocation loc)
        : ASTNode(ASTNodeType::Identifier, loc), name(n) {}
};

class BinaryExprNode : public ASTNode {
public:
    BinaryOp op;
    ASTNode* left;
    ASTNode* right;

    BinaryExprNode(BinaryOp o, ASTNode* l, ASTNode* r, SourceLocation loc)
        : ASTNode(ASTNodeType::BinaryExpr, loc), op(o), left(l), right(r) {}
};

class PrintStmtNode : public ASTNode {
public:
    ASTNode* expression;

    PrintStmtNode(ASTNode* expr, SourceLocation loc)
        : ASTNode(ASTNodeType::PrintStmt, loc), expression(expr) {}
};

// ============================================================================
// 4. Lexer Subsystem
// ============================================================================

enum class TokenType {
    Eof = 0,
    // Keywords
    Let, If, Else, While, Return, Print, True, False, Null,
    // Literals
    Identifier, Number, String,
    // Operators & Punctuation
    Plus, Minus, Star, Slash,
    Equal, EqualEqual, BangEqual, Less, LessEqual, Greater, GreaterEqual,
    LeftParen, RightParen, LeftBrace, RightBrace, Semicolon
};

struct Token {
    TokenType type;
    std::string_view lexeme;
    SourceLocation location;
};

class Lexer {
private:
    std::string_view source_;
    size_t cursor_ = 0;
    SourceLocation loc_;

public:
    explicit Lexer(std::string_view source) noexcept : source_(source) {}

    CompResult<Token> NextToken() noexcept {
        SkipWhitespaceAndComments();

        if (cursor_ >= source_.size()) {
            return Token{TokenType::Eof, "", loc_};
        }

        SourceLocation start_loc = loc_;
        char c = Advance();

        if (IsAlpha(c)) {
            return ScanIdentifierOrKeyword(start_loc);
        }
        if (IsDigit(c)) {
            return ScanNumber(start_loc);
        }

        switch (c) {
            case '+': return Token{TokenType::Plus, source_.substr(cursor_ - 1, 1), start_loc};
            case '-': return Token{TokenType::Minus, source_.substr(cursor_ - 1, 1), start_loc};
            case '*': return Token{TokenType::Star, source_.substr(cursor_ - 1, 1), start_loc};
            case '/': return Token{TokenType::Slash, source_.substr(cursor_ - 1, 1), start_loc};
            case ';': return Token{TokenType::Semicolon, source_.substr(cursor_ - 1, 1), start_loc};
            case '(': return Token{TokenType::LeftParen, source_.substr(cursor_ - 1, 1), start_loc};
            case ')': return Token{TokenType::RightParen, source_.substr(cursor_ - 1, 1), start_loc};
            case '{': return Token{TokenType::LeftBrace, source_.substr(cursor_ - 1, 1), start_loc};
            case '}': return Token{TokenType::RightBrace, source_.substr(cursor_ - 1, 1), start_loc};
            case '=':
                if (Match('=')) {
                    return Token{TokenType::EqualEqual, source_.substr(cursor_ - 2, 2), start_loc};
                }
                return Token{TokenType::Equal, source_.substr(cursor_ - 1, 1), start_loc};
            case '!':
                if (Match('=')) {
                    return Token{TokenType::BangEqual, source_.substr(cursor_ - 2, 2), start_loc};
                }
                return std::unexpected(CompileError(ErrorCode::UnexpectedCharacter, start_loc, "Expected '=' after '!'"));
            case '<':
                if (Match('=')) {
                    return Token{TokenType::LessEqual, source_.substr(cursor_ - 2, 2), start_loc};
                }
                return Token{TokenType::Less, source_.substr(cursor_ - 1, 1), start_loc};
            case '>':
                if (Match('=')) {
                    return Token{TokenType::GreaterEqual, source_.substr(cursor_ - 2, 2), start_loc};
                }
                return Token{TokenType::Greater, source_.substr(cursor_ - 1, 1), start_loc};
            default:
                return std::unexpected(CompileError(ErrorCode::UnexpectedCharacter, start_loc, std::string("Unexpected character: ") + c));
        }
    }

private:
    char Peek() const noexcept {
        if (cursor_ >= source_.size()) return '\0';
        return source_[cursor_];
    }

    char Advance() noexcept {
        char c = source_[cursor_++];
        if (c == '\n') {
            loc_.line++;
            loc_.