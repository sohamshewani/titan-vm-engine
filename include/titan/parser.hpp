#ifndef TITAN_PARSER_HPP
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <memory_resource>
#include <expected>
#include <cstdint>
#include <variant>
#include <array>

namespace titan {

    enum class ErrorCode : uint32_t {
        Success = 0,
        UnexpectedToken,
        ExpectedExpression,
        ExpectedIdentifier,
        ExpectedSemicolon,
        UnterminatedString,
        Overflow,
        OutOfMemory
    };

    enum class NodeType : uint8_t {
        LiteralNumber,
        LiteralString,
        LiteralBoolean,
        LiteralNull,
        Identifier,
        BinaryExpr,
        UnaryExpr,
        AssignmentExpr,
        BlockStmt,
        IfStmt,
        WhileStmt,
        ReturnStmt,
        FunctionDecl,
        VarDecl
    };

    enum class OpType : uint8_t {
        Add, Sub, Mul, Div,
        Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
        LogicalAnd, LogicalOr,
        Negate, Not
    };

    struct ASTNode;

    struct Token {
        enum class Type : uint8_t {
            Number, String, Identifier,
            KeywordIf, KeywordWhile, KeywordReturn, KeywordFn, KeywordVar,
            KeywordTrue, KeywordFalse, KeywordNull,
            Plus, Minus, Star, Slash,
            Equal, EqualEqual, Bang, BangEqual,
            Less, LessEqual, Greater, GreaterEqual,
            AndAnd, OrOr,
            LeftParen, RightParen, LeftBrace, RightBrace,
            Semicolon, Comma, Eof, Unknown
        };

        Type type;
        std::string_view lexeme;
        size_t line;
        size_t column;
    };

    struct LiteralNumberNode { double value; };
    struct LiteralStringNode { std::string_view value; };
    struct LiteralBooleanNode { bool value; };
    struct IdentifierNode { std::string_view name; };
    struct BinaryExprNode { OpType op; ASTNode* left; ASTNode* right; };
    struct UnaryExprNode { OpType op; ASTNode* operand; };
    struct AssignmentExprNode { std::string_view name; ASTNode* value; };
    struct BlockStmtNode { std::vector<ASTNode*, std::pmr::polymorphic_allocator<ASTNode*>> statements; };
    struct IfExprNode { ASTNode* condition; ASTNode* thenBranch; ASTNode* elseBranch; };
    struct WhileStmtNode { ASTNode* condition; ASTNode* body; };
    struct ReturnStmtNode { ASTNode* value; };
    struct FunctionDeclNode { std::string_view name; std::vector<std::string_view, std::pmr::polymorphic_allocator<std::string_view>> params; ASTNode* body; };
    struct VarDeclNode { std::string_view name; ASTNode* initializer; };

    struct ASTNode {
        NodeType type;
        size_t line;
        size_t column;
        
        std::variant<
            LiteralNumberNode,
            LiteralStringNode,
            LiteralBooleanNode,
            IdentifierNode,
            BinaryExprNode,
            UnaryExprNode,
            AssignmentExprNode,
            BlockStmtNode,
            IfExprNode,
            WhileStmtNode,
            ReturnStmtNode,
            FunctionDeclNode,
            VarDeclNode
        > data;
    };

    class Lexer {
    public:
        explicit Lexer(std::string_view source) noexcept
            : source_(source), cursor_(0), line_(1), column_(1) {}

        Token nextToken() noexcept {
            skipWhitespace();
            if (cursor_ >= source_.size()) {
                return Token{Token::Type::Eof, "", line_, column_};
            }

            size_t startLine = line_;
            size_t startCol = column_;
            char c = advance();

            if (isAlpha(c)) {
                return identifier(startLine, startCol);
            }
            if (isDigit(c)) {
                return number(startLine, startCol);
            }

            switch (c) {
                returnToken(Token::Type::Plus, "+", startLine, startCol);
                case '-': return makeToken(Token::Type::Minus, "-", startLine, startCol);
                case '*': return makeToken(Token::Type::Star, "*", startLine, startCol);
                case '/': return makeToken(Token::Type::Slash, "/", startLine, startCol);
                case '(': return makeToken(Token::Type::LeftParen, "(", startLine, startCol);
                case ')': return makeToken(Token::Type::RightParen, ")", startLine, startCol);
                case '{': return makeToken(Token::Type::LeftBrace, "{", startLine, startCol);
                case '}': return makeToken(Token::Type::RightBrace, "}", startLine, startCol);
                case ';': return makeToken(Token::Type::Semicolon, ";", startLine, startCol);
                case ',': return makeToken(Token::Type::Comma, ",", startLine, startCol);
                case '=':
                    if (match('=')) return makeToken(Token::Type::EqualEqual, "==", startLine, startCol);
                    return makeToken(Token::Type::Equal, "=", startLine, startCol);
                case '!':
                    if (match('=')) return makeToken(Token::Type::BangEqual, "!=", startLine, startCol);
                    return makeToken(Token::Type::Bang, "!", startLine, startCol);
                case '<':
                    if (match('=')) return makeToken(Token::Type::LessEqual, "<=", startLine, startCol);
                    return makeToken(Token::Type::Less, "<", startLine, startCol);
                case '>':
                    if (match('=')) return makeToken(Token::Type::GreaterEqual, ">=", startLine, startCol);
                    return makeToken(Token::Type::Greater, ">", startLine, startCol);
                case '&':
                    if (match('&')) return makeToken(Token::Type::AndAnd, "&&", startLine, startCol);
                    break;
                case '|':
                    if (match('|')) return makeToken(Token::Type::OrOr, "||", startLine, startCol);
                    break;
                case '"':
                    return string(startLine, startCol);
                default:
                    break;
            }

            return makeToken(Token::Type::Unknown, std::string_view(&source_[cursor_ - 1], 1), startLine, startCol);
        }

    private:
        std::string_view source_;
        size_t cursor_;
        size_t line_;
        size_t column_;

        bool isAlpha(char c) const noexcept {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool isDigit(char c) const noexcept {
            return c >= '0' && c <= '9';
        }

        char advance() noexcept {
            char c = source_[cursor_++];
            if (c == '\n') {
                line_++;
                column_ = 1;
            } else {
                column_++;
            }
            return c;
        }

        bool match(char expected) noexcept {
            if (cursor_ >= source_.size()) return false;
            if (source_[cursor_] != expected) return false;
            advance();
            return true;
        }

        void skipWhitespace() noexcept {
            while (cursor_ < source_.size()) {
                char c = source_[cursor_];
                switch (c) {
                    case ' ':
                    case '\r':
                    case '\t':
                    case '\n':
                        advance();
                        break;
                    case '/':
                        if (cursor_ + 1 < source_.size() && source_[cursor_ + 1] == '/') {
                            while (cursor_ < source_.size() && source_[cursor_] != '\n') {
                                advance();
                            }
                        } else {
                            return;
                        }
                        break;
                    default:
                        return;
                }
            }
        }

        Token makeToken(Token::Type type, std::string_view lexeme, size_t line, size_t col) const noexcept {
            return Token{type, lexeme, line, col};
        }

        Token identifier(size_t line, size_t col) noexcept {
            size_t start = cursor_ - 1;
            while (cursor_ < source_.size() && (isAlpha(source_[cursor_]) || isDigit(source_[cursor_]))) {
                advance();
            }
            std::string_view lexeme = source_.substr(start, cursor_ - start);
            
            Token::Type type = Token::Type::Identifier;
            if (lexeme == "if") type = Token::Type::KeywordIf;
            else if (lexeme == "while") type = Token::Type::KeywordWhile;
            else if (lexeme == "return") type = Token::Type::KeywordReturn;
            else if (lexeme == "fn") type = Token::Type::KeywordFn;
            else if (lexeme == "var") type = Token::Type::KeywordVar;
            else if (lexeme == "true") type = Token::Type::KeywordTrue;
            else if (lexeme == "false") type = Token::Type::KeywordFalse;
            else if (lexeme == "null") type = Token::Type::KeywordNull;

            return makeToken(type, lexeme, line, col);
        }

        Token number(size_t line, size_t col) noexcept {
            size_t start = cursor_ - 1;
            while (cursor_ < source_.size() && isDigit(source_[cursor_])) {
                advance();
            }
            if (cursor_ < source_.size() && source_[cursor_] == '.' && cursor_ + 1 < source_.size() && isDigit(source_[cursor_ + 1])) {
                advance();
                while (cursor_ < source_.size() && isDigit(source_[cursor_])) {
                    advance();
                }
            }
            return makeToken(Token::Type::Number, source_.substr(start, cursor_ - start), line, col);
        }

        Token string(size_t line, size_t col) noexcept {
            size_t start = cursor_;
            while (cursor_ < source_.size() && source_[cursor_] != '"') {
                advance();
            }
            if (cursor_ >= source_.size()) {
                return makeToken(Token::Type::Unknown, source_.substr(start - 1, cursor_ - start + 1), line, col);
            }
            std::string_view lexeme = source_.substr(start, cursor_ - start);
            advance(); // Consume closing quote
            return makeToken(Token::Type::String, lexeme, line, col);
        }

        Token returnToken(Token::Type type, std::string_view lexeme, size_t line, size_t col) noexcept {
            return makeToken(type, lexeme, line, col);
        }
    };

    class Parser {
    public:
        explicit Parser(std::string_view source, std::pmr::memory_resource* memoryResource = std::pmr::get_default_resource()) noexcept
            : lexer_(source), arena_(memoryResource), currentToken_{Token::Type::Eof, "", 0, 0}, peekToken_{Token::Type::Eof, "", 0, 0}, hadError_(false) {
            advanceLexer();
            advanceLexer();
        }

        std::expected<ASTNode*, ErrorCode> parse() noexcept {
            auto block = arena_.new_object<ASTNode>();
            if (!block) return std::unexpected(ErrorCode::OutOfMemory);
            
            block->type = NodeType::BlockStmt;
            block->line = currentToken_.line;
            block->column = currentToken_.column;
            
            BlockStmtNode blockNode{std::pmr::polymorphic_allocator<ASTNode*>(&arena_)};

            while (currentToken_.type != Token::Type::Eof) {
                auto stmtResult = parseDeclaration();
                if (!stmtResult) {
                    return std::unexpected(stmtResult.error());
                }
                blockNode.statements.push_back(*stmtResult);
            }

            block->data = std::move(blockNode);
            if (hadError_) {
                return std::unexpected(ErrorCode::UnexpectedToken);
            }
            return block;
        }

    private:
        Lexer lexer_;
        std::pmr::monotonic_buffer_resource arena_;
        Token currentToken_;
        Token peekToken_;
        bool hadError_;

        void advanceLexer() noexcept {
            currentToken_ = peekToken_;
            peekToken_ = lexer_.nextToken();
        }

        bool match(Token::Type type) noexcept {
            if (check(type)) {
                advanceLexer();
                return true;
            }
            return false;
        }

        bool check(Token::Type type) const noexcept {
            return currentToken_.type == type;
        }

        std::expected<Token, ErrorCode> consume(Token::Type type, ErrorCode err) noexcept {
            if (check(type)) {
                Token t = currentToken_;
                advanceLexer();
                return t;
            }
            hadError_ = true;
            return std::unexpected(err);
        }

        std::expected<ASTNode*, ErrorCode> parseDeclaration() noexcept {
            if (match(Token::Type::KeywordVar)) {
                return parseVarDecl();
            }
            if (match(Token::Type::KeywordFn)) {
                return parseFunctionDecl();
            }
            return parseStatement();
        }

        std::expected<ASTNode*, ErrorCode> parseVarDecl() noexcept {
            auto nameToken = consume(Token::Type::Identifier, ErrorCode::ExpectedIdentifier);
            if (!nameToken) return std::unexpected(nameToken.error());

            ASTNode* initializer = nullptr;
            if (match(Token::Type::Equal)) {
                auto expr = parseExpression();
                if (!expr) return std::unexpected(expr.error());
                initializer = *expr;
            }

            auto semi = consume(Token::Type::Semicolon, ErrorCode::ExpectedSemicolon);
            if (!semi) return std::unexpected(semi.error());

            auto node = arena_.new_object<ASTNode>();
            if (!node) return std::unexpected(ErrorCode::OutOfMemory);

            node->type = NodeType::VarDecl;
            node->line = nameToken->line;
            node->column = nameToken->column;
            node->data = VarDeclNode{nameToken->lexeme, initializer};
            return node;
        }

        std::expected<ASTNode*, ErrorCode> parseFunctionDecl() noexcept {
            auto nameToken = consume(Token::Type::Identifier, ErrorCode::ExpectedIdentifier);
            if (!nameToken) return std::unexpected(nameToken.error());

            auto openParen = consume(Token::Type::LeftParen, ErrorCode::UnexpectedToken);
            if (!openParen) return std::unexpected(openParen.error());

            std::vector<std::string_view, std::pmr::polymorphic_allocator<std::string_view>> params(std::pmr::polymorphic_allocator<std::string_view>(&arena_));
            if (!check(Token::Type::RightParen)) {
                do {
                    auto paramToken = consume(Token::Type::Identifier, ErrorCode::ExpectedIdentifier);
                    if (!paramToken) return std::unexpected(paramToken.error());
                    params.push_back(paramToken->lexeme);
                } while (match(Token::Type::Comma));
            }

            auto closeParen = consume(Token::Type::RightParen, ErrorCode::UnexpectedToken);
            if (!closeParen) return std::unexpected(closeParen.error());

            auto body = parseBlock();
            if (!body) return std::unexpected(body.error());

            auto node = arena_.new_object<ASTNode>();
            if (!node) return std::unexpected(ErrorCode::OutOfMemory);

            node->type = NodeType::FunctionDecl;
            node->line = nameToken->line;
            node->column = nameToken->column;
            node->data = FunctionDeclNode{nameToken->lexeme, std::move(params), *body};
            return node;
        }

        std::expected<ASTNode*, ErrorCode> parseStatement() noexcept {
            if (match(Token::Type::KeywordIf)) {
                return parseIfStatement();
            }
            if (match(Token::Type::KeywordWhile)) {
                return parseWhileStatement();
            }
            if (match(Token::Type::KeywordReturn)) {
                return