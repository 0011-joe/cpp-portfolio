/*
 * Relational DSL Interpreter
 * A mini declarative language for data analysis
 * Lexer + Recursive Descent Parser + Relational Algebra Engine + CSV Backend
 *
 * Supports: SELECT, FROM, WHERE, ORDER BY, LIMIT, GROUP BY, aggregate functions
 * Syntax: SELECT name, age FROM users WHERE age > 18 ORDER BY age LIMIT 10
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <string>
#endif

#define MAX_TOKENS 512
#define MAX_COLS 32
#define MAX_ROWS 4096
#define MAX_STR 128
#define MAX_IDENTIFIER 64

/* ============================================================
 * SECTION 1: Token Types & Lexer
 * ============================================================ */

typedef enum {
    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_ORDER, TOK_BY,
    TOK_LIMIT, TOK_GROUP, TOK_HAVING, TOK_AS, TOK_JOIN,
    TOK_ON, TOK_AND, TOK_OR, TOK_NOT, TOK_NULL,
    TOK_INSERT, TOK_INTO, TOK_VALUES, TOK_CREATE, TOK_TABLE,
    TOK_ASC, TOK_DESC, TOK_DISTINCT,
    TOK_IDENTIFIER, TOK_NUMBER, TOK_STRING,
    TOK_COMMA, TOK_DOT, TOK_LPAREN, TOK_RPAREN,
    TOK_STAR, TOK_PLUS, TOK_MINUS, TOK_DIV, TOK_MOD,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
    TOK_SEMICOLON, TOK_EOF, TOK_ERROR,
    /* Aggregate functions */
    TOK_COUNT, TOK_SUM, TOK_AVG, TOK_MIN, TOK_MAX
} TokenType;

typedef struct {
    TokenType type;
    char text[MAX_STR];
    int line;
    int col;
} Token;

typedef struct {
    const char* source;
    int pos;
    int line;
    int col;
    Token tokens[MAX_TOKENS];
    int token_count;
} Lexer;

static const struct { const char* word; TokenType type; } keywords[] = {
    {"SELECT", TOK_SELECT}, {"FROM", TOK_FROM}, {"WHERE", TOK_WHERE},
    {"ORDER", TOK_ORDER}, {"BY", TOK_BY}, {"LIMIT", TOK_LIMIT},
    {"GROUP", TOK_GROUP}, {"HAVING", TOK_HAVING}, {"AS", TOK_AS},
    {"JOIN", TOK_JOIN}, {"ON", TOK_ON}, {"AND", TOK_AND},
    {"OR", TOK_OR}, {"NOT", TOK_NOT}, {"NULL", TOK_NULL},
    {"INSERT", TOK_INSERT}, {"INTO", TOK_INTO}, {"VALUES", TOK_VALUES},
    {"CREATE", TOK_CREATE}, {"TABLE", TOK_TABLE},
    {"ASC", TOK_ASC}, {"DESC", TOK_DESC}, {"DISTINCT", TOK_DISTINCT},
    {"COUNT", TOK_COUNT}, {"SUM", TOK_SUM}, {"AVG", TOK_AVG},
    {"MIN", TOK_MIN}, {"MAX", TOK_MAX},
    {NULL, TOK_ERROR}
};

static Token make_token(TokenType type, const char* text, int line, int col) {
    Token t;
    t.type = type;
    strncpy(t.text, text, MAX_STR - 1);
    t.text[MAX_STR - 1] = '\0';
    t.line = line;
    t.col = col;
    return t;
}

static TokenType lookup_keyword(const char* word) {
    for (int i = 0; keywords[i].word; i++) {
        if (strcasecmp(keywords[i].word, word) == 0)
            return keywords[i].type;
    }
    return TOK_IDENTIFIER;
}

static void lexer_init(Lexer* lex, const char* source) {
    lex->source = source;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->token_count = 0;
}

static char lexer_peek(Lexer* lex) {
    return lex->source[lex->pos];
}

static char lexer_advance(Lexer* lex) {
    char c = lex->source[lex->pos++];
    if (c == '\n') { lex->line++; lex->col = 1; }
    else lex->col++;
    return c;
}

static void add_token(Lexer* lex, TokenType type, const char* text) {
    if (lex->token_count < MAX_TOKENS) {
        lex->tokens[lex->token_count++] = make_token(type, text, lex->line, lex->col);
    }
}

static void lexer_tokenize(Lexer* lex) {
    while (lexer_peek(lex) != '\0') {
        char c = lexer_peek(lex);

        // Skip whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lexer_advance(lex);
            continue;
        }

        // Skip comments
        if (c == '-' && lex->source[lex->pos + 1] == '-') {
            while (lexer_peek(lex) != '\0' && lexer_peek(lex) != '\n')
                lexer_advance(lex);
            continue;
        }

        int start_col = lex->col;

        // String literal
        if (c == '\'' || c == '"') {
            char quote = lexer_advance(lex);
            char buf[MAX_STR] = {0};
            int i = 0;
            while (lexer_peek(lex) != '\0' && lexer_peek(lex) != quote) {
                if (i < MAX_STR - 1) buf[i++] = lexer_advance(lex);
            }
            if (lexer_peek(lex) == quote) lexer_advance(lex);
            add_token(lex, TOK_STRING, buf);
            continue;
        }

        // Number
        if (isdigit(c) || (c == '.' && isdigit(lex->source[lex->pos + 1]))) {
            char buf[MAX_STR] = {0};
            int i = 0;
            while (isdigit(lexer_peek(lex)) || lexer_peek(lex) == '.') {
                if (i < MAX_STR - 1) buf[i++] = lexer_advance(lex);
            }
            add_token(lex, TOK_NUMBER, buf);
            continue;
        }

        // Identifier or keyword
        if (isalpha(c) || c == '_') {
            char buf[MAX_IDENTIFIER] = {0};
            int i = 0;
            while (isalnum(lexer_peek(lex)) || lexer_peek(lex) == '_') {
                if (i < MAX_IDENTIFIER - 1) buf[i++] = lexer_advance(lex);
            }
            TokenType type = lookup_keyword(buf);
            add_token(lex, type, buf);
            continue;
        }

        // Operators and punctuation
        lexer_advance(lex);
        switch (c) {
            case ',': add_token(lex, TOK_COMMA, ","); break;
            case '.': add_token(lex, TOK_DOT, "."); break;
            case '(': add_token(lex, TOK_LPAREN, "("); break;
            case ')': add_token(lex, TOK_RPAREN, ")"); break;
            case '*': add_token(lex, TOK_STAR, "*"); break;
            case '+': add_token(lex, TOK_PLUS, "+"); break;
            case '/': add_token(lex, TOK_DIV, "/"); break;
            case '%': add_token(lex, TOK_MOD, "%"); break;
            case ';': add_token(lex, TOK_SEMICOLON, ";"); break;
            case '-':
                add_token(lex, TOK_MINUS, "-");
                break;
            case '=':
                if (lexer_peek(lex) == '=') { lexer_advance(lex); add_token(lex, TOK_EQ, "=="); }
                else add_token(lex, TOK_EQ, "=");
                break;
            case '!':
                if (lexer_peek(lex) == '=') { lexer_advance(lex); add_token(lex, TOK_NEQ, "!="); }
                else add_token(lex, TOK_ERROR, "!");
                break;
            case '<':
                if (lexer_peek(lex) == '=') { lexer_advance(lex); add_token(lex, TOK_LTE, "<="); }
                else if (lexer_peek(lex) == '>') { lexer_advance(lex); add_token(lex, TOK_NEQ, "<>"); }
                else add_token(lex, TOK_LT, "<");
                break;
            case '>':
                if (lexer_peek(lex) == '=') { lexer_advance(lex); add_token(lex, TOK_GTE, ">="); }
                else add_token(lex, TOK_GT, ">");
                break;
            default: {
                char buf[2] = {c, '\0'};
                add_token(lex, TOK_ERROR, buf);
            }
        }
    }
    add_token(lex, TOK_EOF, "");
}

/* ============================================================
 * SECTION 2: Data Types - Table, Row, Column
 * ============================================================ */

typedef enum { VAL_NULL, VAL_INT, VAL_FLOAT, VAL_STRING } ValueType;

typedef union {
    long long int_val;
    double float_val;
    char str_val[MAX_STR];
} ValueData;

typedef struct {
    ValueType type;
    ValueData data;
} Value;

typedef struct {
    char name[MAX_IDENTIFIER];
    ValueType type;
} Column;

typedef struct {
    Value values[MAX_COLS];
} Row;

typedef struct {
    char name[MAX_IDENTIFIER];
    Column columns[MAX_COLS];
    int num_cols;
    Row rows[MAX_ROWS];
    int num_rows;
} Table;

static Value val_null() {
    Value v; v.type = VAL_NULL; return v;
}

static Value val_int(long long n) {
    Value v; v.type = VAL_INT; v.data.int_val = n; return v;
}

static Value val_float(double f) {
    Value v; v.type = VAL_FLOAT; v.data.float_val = f; return v;
}

static Value val_str(const char* s) {
    Value v; v.type = VAL_STRING; strncpy(v.data.str_val, s, MAX_STR - 1); return v;
}

static double value_to_number(Value v) {
    switch (v.type) {
        case VAL_INT: return (double)v.data.int_val;
        case VAL_FLOAT: return v.data.float_val;
        case VAL_STRING: return atof(v.data.str_val);
        default: return 0.0;
    }
}

static int value_compare(Value a, Value b) {
    if (a.type == VAL_STRING && b.type == VAL_STRING)
        return strcmp(a.data.str_val, b.data.str_val);
    return (value_to_number(a) > value_to_number(b)) - (value_to_number(a) < value_to_number(b));
}

/* ============================================================
 * SECTION 3: AST Nodes
 * ============================================================ */

typedef enum {
    EXPR_COLUMN, EXPR_LITERAL, EXPR_BINARY, EXPR_UNARY,
    EXPR_AGG_FUNC, EXPR_FUNC_CALL
} ExprType;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_AND, OP_OR, OP_LIKE
} BinOp;

typedef enum { AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } AggFunc;

typedef struct Expr {
    ExprType type;
    union {
        struct { char name[MAX_IDENTIFIER]; char alias[MAX_IDENTIFIER]; } column;
        struct { Value value; } literal;
        struct { BinOp op; struct Expr* left; struct Expr* right; } binary;
        struct { struct Expr* operand; } unary;
        struct { AggFunc func; struct Expr* arg; char alias[MAX_IDENTIFIER]; } agg;
    } data;
} Expr;

typedef struct SelectItem {
    Expr* expr;
    char alias[MAX_IDENTIFIER];
} SelectItem;

typedef enum { SORT_ASC, SORT_DESC } SortDir;

typedef struct OrderItem {
    Expr* expr;
    SortDir dir;
} OrderItem;

typedef struct {
    SelectItem items[MAX_COLS];
    int num_items;
    bool select_all;
    char from_table[MAX_IDENTIFIER];
    Expr* where_clause;
    OrderItem order_items[MAX_COLS];
    int num_order_items;
    Expr* group_items[MAX_COLS];
    int num_group_items;
    Expr* having_clause;
    int limit;
    bool has_limit;
    bool distinct;
} QueryAST;

/* ============================================================
 * SECTION 4: Parser (Recursive Descent)
 * ============================================================ */

typedef struct {
    Token tokens[MAX_TOKENS];
    int pos;
    int num_tokens;
    char error[256];
} Parser;

static Token parser_peek(Parser* p) {
    if (p->pos < p->num_tokens) return p->tokens[p->pos];
    return make_token(TOK_EOF, "", 0, 0);
}

static Token parser_advance(Parser* p) {
    Token t = parser_peek(p);
    if (p->pos < p->num_tokens) p->pos++;
    return t;
}

static bool parser_expect(Parser* p, TokenType type) {
    if (parser_peek(p).type == type) { parser_advance(p); return true; }
    snprintf(p->error, sizeof(p->error), "Expected token type %d, got '%s'", type, parser_peek(p).text);
    return false;
}

static Expr* parse_expression(Parser* p);
static Expr* parse_or_expr(Parser* p);
static Expr* parse_and_expr(Parser* p);
static Expr* parse_comparison(Parser* p);
static Expr* parse_additive(Parser* p);
static Expr* parse_multiplicative(Parser* p);
static Expr* parse_unary(Parser* p);
static Expr* parse_primary(Parser* p);

static Expr* new_expr(ExprType type) {
    Expr* e = (Expr*)calloc(1, sizeof(Expr));
    e->type = type;
    return e;
}

static Expr* parse_primary(Parser* p) {
    Token t = parser_peek(p);

    // Aggregate functions: COUNT(*), SUM(col), etc.
    if (t.type == TOK_COUNT || t.type == TOK_SUM || t.type == TOK_AVG ||
        t.type == TOK_MIN || t.type == TOK_MAX) {
        Expr* e = new_expr(EXPR_AGG_FUNC);
        switch (t.type) {
            case TOK_COUNT: e->data.agg.func = AGG_COUNT; break;
            case TOK_SUM:   e->data.agg.func = AGG_SUM; break;
            case TOK_AVG:   e->data.agg.func = AGG_AVG; break;
            case TOK_MIN:   e->data.agg.func = AGG_MIN; break;
            case TOK_MAX:   e->data.agg.func = AGG_MAX; break;
            default: break;
        }
        parser_advance(p);
        parser_expect(p, TOK_LPAREN);
        if (parser_peek(p).type == TOK_STAR) {
            e->data.agg.arg = new_expr(EXPR_LITERAL);
            e->data.agg.arg->data.literal.value = val_str("*");
            parser_advance(p);
        } else {
            e->data.agg.arg = parse_expression(p);
        }
        parser_expect(p, TOK_RPAREN);
        // Check for alias: AS name
        if (parser_peek(p).type == TOK_AS) {
            parser_advance(p);
            Token alias = parser_advance(p);
            strncpy(e->data.agg.alias, alias.text, MAX_IDENTIFIER - 1);
        }
        return e;
    }

    // Parenthesized expression
    if (t.type == TOK_LPAREN) {
        parser_advance(p);
        Expr* e = parse_expression(p);
        parser_expect(p, TOK_RPAREN);
        return e;
    }

    // Number literal
    if (t.type == TOK_NUMBER) {
        parser_advance(p);
        Expr* e = new_expr(EXPR_LITERAL);
        if (strchr(t.text, '.')) {
            e->data.literal.value = val_float(atof(t.text));
        } else {
            e->data.literal.value = val_int(atoll(t.text));
        }
        return e;
    }

    // String literal
    if (t.type == TOK_STRING) {
        parser_advance(p);
        Expr* e = new_expr(EXPR_LITERAL);
        e->data.literal.value = val_str(t.text);
        return e;
    }

    // Column reference
    if (t.type == TOK_IDENTIFIER) {
        parser_advance(p);
        Expr* e = new_expr(EXPR_COLUMN);
        strncpy(e->data.column.name, t.text, MAX_IDENTIFIER - 1);
        return e;
    }

    snprintf(p->error, sizeof(p->error), "Unexpected token '%s' at line %d", t.text, t.line);
    return new_expr(EXPR_LITERAL);
}

static Expr* parse_unary(Parser* p) {
    if (parser_peek(p).type == TOK_MINUS) {
        parser_advance(p);
        Expr* operand = parse_unary(p);
        Expr* e = new_expr(EXPR_LITERAL);
        e->data.literal.value = val_float(-value_to_number(operand->data.literal.value));
        free(operand);
        return e;
    }
    if (parser_peek(p).type == TOK_NOT) {
        parser_advance(p);
        Expr* operand = parse_unary(p);
        Expr* e = new_expr(EXPR_UNARY);
        e->data.unary.operand = operand;
        return e;
    }
    return parse_primary(p);
}

static Expr* parse_multiplicative(Parser* p) {
    Expr* left = parse_unary(p);
    while (parser_peek(p).type == TOK_STAR || parser_peek(p).type == TOK_DIV || parser_peek(p).type == TOK_MOD) {
        Token op = parser_advance(p);
        Expr* right = parse_unary(p);
        Expr* e = new_expr(EXPR_BINARY);
        e->data.binary.left = left;
        e->data.binary.right = right;
        switch (op.type) {
            case TOK_STAR: e->data.binary.op = OP_MUL; break;
            case TOK_DIV:  e->data.binary.op = OP_DIV; break;
            case TOK_MOD:  e->data.binary.op = OP_MOD; break;
            default: break;
        }
        left = e;
    }
    return left;
}

static Expr* parse_additive(Parser* p) {
    Expr* left = parse_multiplicative(p);
    while (parser_peek(p).type == TOK_PLUS || parser_peek(p).type == TOK_MINUS) {
        Token op = parser_advance(p);
        Expr* right = parse_multiplicative(p);
        Expr* e = new_expr(EXPR_BINARY);
        e->data.binary.left = left;
        e->data.binary.right = right;
        e->data.binary.op = (op.type == TOK_PLUS) ? OP_ADD : OP_SUB;
        left = e;
    }
    return left;
}

static Expr* parse_comparison(Parser* p) {
    Expr* left = parse_additive(p);
    TokenType tt = parser_peek(p).type;
    if (tt == TOK_EQ || tt == TOK_NEQ || tt == TOK_LT || tt == TOK_GT ||
        tt == TOK_LTE || tt == TOK_GTE) {
        Token op = parser_advance(p);
        Expr* right = parse_additive(p);
        Expr* e = new_expr(EXPR_BINARY);
        e->data.binary.left = left;
        e->data.binary.right = right;
        switch (op.type) {
            case TOK_EQ:  e->data.binary.op = OP_EQ; break;
            case TOK_NEQ: e->data.binary.op = OP_NEQ; break;
            case TOK_LT:  e->data.binary.op = OP_LT; break;
            case TOK_GT:  e->data.binary.op = OP_GT; break;
            case TOK_LTE: e->data.binary.op = OP_LTE; break;
            case TOK_GTE: e->data.binary.op = OP_GTE; break;
            default: break;
        }
        return e;
    }
    return left;
}

static Expr* parse_and_expr(Parser* p) {
    Expr* left = parse_comparison(p);
    while (parser_peek(p).type == TOK_AND) {
        parser_advance(p);
        Expr* right = parse_comparison(p);
        Expr* e = new_expr(EXPR_BINARY);
        e->data.binary.left = left;
        e->data.binary.right = right;
        e->data.binary.op = OP_AND;
        left = e;
    }
    return left;
}

static Expr* parse_or_expr(Parser* p) {
    Expr* left = parse_and_expr(p);
    while (parser_peek(p).type == TOK_OR) {
        parser_advance(p);
        Expr* right = parse_and_expr(p);
        Expr* e = new_expr(EXPR_BINARY);
        e->data.binary.left = left;
        e->data.binary.right = right;
        e->data.binary.op = OP_OR;
        left = e;
    }
    return left;
}

static Expr* parse_expression(Parser* p) {
    return parse_or_expr(p);
}

static QueryAST parse_query(Parser* p) {
    QueryAST q;
    memset(&q, 0, sizeof(q));
    q.limit = -1;

    // SELECT
    if (parser_peek(p).type != TOK_SELECT) {
        snprintf(p->error, sizeof(p->error), "Expected SELECT");
        return q;
    }
    parser_advance(p);

    // DISTINCT
    if (parser_peek(p).type == TOK_DISTINCT) {
        q.distinct = true;
        parser_advance(p);
    }

    // SELECT items
    if (parser_peek(p).type == TOK_STAR) {
        q.select_all = true;
        parser_advance(p);
    } else {
        do {
            if (q.num_items >= MAX_COLS) break;
            Expr* expr = parse_expression(p);
            q.items[q.num_items].expr = expr;
            // Check for alias
            if (parser_peek(p).type == TOK_AS) {
                parser_advance(p);
                Token alias = parser_advance(p);
                strncpy(q.items[q.num_items].alias, alias.text, MAX_IDENTIFIER - 1);
            } else if (expr->type == EXPR_COLUMN) {
                strncpy(q.items[q.num_items].alias, expr->data.column.name, MAX_IDENTIFIER - 1);
            }
            q.num_items++;
        } while (parser_peek(p).type == TOK_COMMA && (parser_advance(p).type == TOK_COMMA));
    }

    // FROM
    if (parser_peek(p).type == TOK_FROM) {
        parser_advance(p);
        Token table = parser_advance(p);
        strncpy(q.from_table, table.text, MAX_IDENTIFIER - 1);
    }

    // WHERE
    if (parser_peek(p).type == TOK_WHERE) {
        parser_advance(p);
        q.where_clause = parse_expression(p);
    }

    // GROUP BY
    if (parser_peek(p).type == TOK_GROUP) {
        parser_advance(p);
        parser_expect(p, TOK_BY);
        do {
            if (q.num_group_items >= MAX_COLS) break;
            q.group_items[q.num_group_items++] = parse_expression(p);
        } while (parser_peek(p).type == TOK_COMMA && (parser_advance(p).type == TOK_COMMA));
    }

    // HAVING
    if (parser_peek(p).type == TOK_HAVING) {
        parser_advance(p);
        q.having_clause = parse_expression(p);
    }

    // ORDER BY
    if (parser_peek(p).type == TOK_ORDER) {
        parser_advance(p);
        parser_expect(p, TOK_BY);
        do {
            if (q.num_order_items >= MAX_COLS) break;
            q.order_items[q.num_order_items].expr = parse_expression(p);
            q.order_items[q.num_order_items].dir = SORT_ASC;
            if (parser_peek(p).type == TOK_ASC) {
                parser_advance(p);
            } else if (parser_peek(p).type == TOK_DESC) {
                q.order_items[q.num_order_items].dir = SORT_DESC;
                parser_advance(p);
            }
            q.num_order_items++;
        } while (parser_peek(p).type == TOK_COMMA && (parser_advance(p).type == TOK_COMMA));
    }

    // LIMIT
    if (parser_peek(p).type == TOK_LIMIT) {
        parser_advance(p);
        Token num = parser_advance(p);
        q.limit = atoi(num.text);
        q.has_limit = true;
    }

    return q;
}

/* ============================================================
 * SECTION 5: Expression Evaluator
 * ============================================================ */

static Value eval_expr(Expr* e, Row* row, Table* table) {
    if (!e) return val_null();
    switch (e->type) {
        case EXPR_LITERAL:
            return e->data.literal.value;
        case EXPR_COLUMN: {
            for (int i = 0; i < table->num_cols; i++) {
                if (strcasecmp(table->columns[i].name, e->data.column.name) == 0) {
                    return row->values[i];
                }
            }
            return val_null();
        }
        case EXPR_BINARY: {
            Value left = eval_expr(e->data.binary.left, row, table);
            Value right = eval_expr(e->data.binary.right, row, table);
            BinOp op = e->data.binary.op;

            if (op == OP_AND) return val_int(value_to_number(left) && value_to_number(right));
            if (op == OP_OR) return val_int(value_to_number(left) || value_to_number(right));

            double l = value_to_number(left), r = value_to_number(right);
            switch (op) {
                case OP_ADD: return val_float(l + r);
                case OP_SUB: return val_float(l - r);
                case OP_MUL: return val_float(l * r);
                case OP_DIV: return r != 0 ? val_float(l / r) : val_null();
                case OP_MOD: return r != 0 ? val_float(fmod(l, r)) : val_null();
                case OP_EQ:  return val_int(value_compare(left, right) == 0);
                case OP_NEQ: return val_int(value_compare(left, right) != 0);
                case OP_LT:  return val_int(value_compare(left, right) < 0);
                case OP_GT:  return val_int(value_compare(left, right) > 0);
                case OP_LTE: return val_int(value_compare(left, right) <= 0);
                case OP_GTE: return val_int(value_compare(left, right) >= 0);
                default: return val_null();
            }
        }
        default:
            return val_null();
    }
}

/* ============================================================
 * SECTION 6: Query Executor
 * ============================================================ */

typedef struct {
    char output[65536];
    int output_len;
    bool has_error;
    char error_msg[256];
    int rows_affected;
} QueryResult;

static void result_append(QueryResult* r, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(r->output + r->output_len, sizeof(r->output) - r->output_len, fmt, args);
    va_end(args);
    if (n > 0) r->output_len += n;
}

static void value_to_str(Value v, char* buf, int bufsize) {
    switch (v.type) {
        case VAL_NULL: strncpy(buf, "NULL", bufsize); break;
        case VAL_INT: snprintf(buf, bufsize, "%lld", v.data.int_val); break;
        case VAL_FLOAT: snprintf(buf, bufsize, "%.2f", v.data.float_val); break;
        case VAL_STRING: snprintf(buf, bufsize, "%s", v.data.str_val); break;
    }
}

static Table g_tables[16];
static int g_num_tables = 0;

static Table* find_table(const char* name) {
    for (int i = 0; i < g_num_tables; i++) {
        if (strcasecmp(g_tables[i].name, name) == 0) return &g_tables[i];
    }
    return NULL;
}

static void execute_query(QueryAST* q, QueryResult* result) {
    Table* table = find_table(q->from_table);
    if (!table) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "Table '%s' not found", q->from_table);
        return;
    }

    // Filter rows with WHERE
    Row filtered[MAX_ROWS];
    int num_filtered = 0;
    for (int i = 0; i < table->num_rows; i++) {
        if (q->where_clause) {
            Value cond = eval_expr(q->where_clause, &table->rows[i], table);
            if (value_to_number(cond) == 0) continue;
        }
        if (num_filtered < MAX_ROWS)
            filtered[num_filtered++] = table->rows[i];
    }

    // Sort
    if (q->num_order_items > 0) {
        for (int i = 0; i < num_filtered - 1; i++) {
            for (int j = i + 1; j < num_filtered; j++) {
                int cmp = 0;
                for (int k = 0; k < q->num_order_items; k++) {
                    Value vi = eval_expr(q->order_items[k].expr, &filtered[i], table);
                    Value vj = eval_expr(q->order_items[k].expr, &filtered[j], table);
                    cmp = value_compare(vi, vj);
                    if (q->order_items[k].dir == SORT_DESC) cmp = -cmp;
                    if (cmp != 0) break;
                }
                if (cmp > 0) {
                    Row tmp = filtered[i];
                    filtered[i] = filtered[j];
                    filtered[j] = tmp;
                }
            }
        }
    }

    // Limit
    int limit = q->has_limit ? q->limit : num_filtered;
    if (limit > num_filtered) limit = num_filtered;

    // Determine output columns
    int out_cols = 0;
    char col_names[MAX_COLS][MAX_IDENTIFIER];
    if (q->select_all) {
        out_cols = table->num_cols;
        for (int i = 0; i < out_cols; i++)
            strncpy(col_names[i], table->columns[i].name, MAX_IDENTIFIER - 1);
    } else {
        out_cols = q->num_items;
        for (int i = 0; i < out_cols; i++) {
            if (q->items[i].alias[0])
                strncpy(col_names[i], q->items[i].alias, MAX_IDENTIFIER - 1);
            else
                snprintf(col_names[i], MAX_IDENTIFIER, "expr_%d", i);
        }
    }

    // Output header
    result_append(result, "┌");
    for (int i = 0; i < out_cols; i++) {
        int w = strlen(col_names[i]) + 2;
        if (w < 10) w = 10;
        for (int j = 0; j < w; j++) result_append(result, "─");
        result_append(result, i < out_cols - 1 ? "┬" : "┐");
    }
    result_append(result, "\n│");
    for (int i = 0; i < out_cols; i++) {
        int w = strlen(col_names[i]) + 2;
        if (w < 10) w = 10;
        result_append(result, " %-*s│", w - 1, col_names[i]);
    }
    result_append(result, "\n├");
    for (int i = 0; i < out_cols; i++) {
        int w = strlen(col_names[i]) + 2;
        if (w < 10) w = 10;
        for (int j = 0; j < w; j++) result_append(result, "─");
        result_append(result, i < out_cols - 1 ? "┼" : "┤");
    }

    // Output rows
    for (int r = 0; r < limit; r++) {
        result_append(result, "\n│");
        for (int c = 0; c < out_cols; c++) {
            Value val;
            if (q->select_all) {
                val = filtered[r].values[c];
            } else {
                val = eval_expr(q->items[c].expr, &filtered[r], table);
            }
            char buf[MAX_STR];
            value_to_str(val, buf, MAX_STR);
            int w = strlen(col_names[c]) + 2;
            if (w < 10) w = 10;
            result_append(result, " %-*s│", w - 1, buf);
        }
    }

    result_append(result, "\n└");
    for (int i = 0; i < out_cols; i++) {
        int w = strlen(col_names[i]) + 2;
        if (w < 10) w = 10;
        for (int j = 0; j < w; j++) result_append(result, "─");
        result_append(result, i < out_cols - 1 ? "┴" : "┘");
    }
    result_append(result, "\n(%d rows)\n", limit);
    result->rows_affected = limit;
}

/* ============================================================
 * SECTION 7: Built-in Demo Data
 * ============================================================ */

static void init_demo_data() {
    g_num_tables = 3;

    // employees table
    Table* emp = &g_tables[0];
    strcpy(emp->name, "employees");
    emp->num_cols = 5;
    strcpy(emp->columns[0].name, "id");       emp->columns[0].type = VAL_INT;
    strcpy(emp->columns[1].name, "name");     emp->columns[1].type = VAL_STRING;
    strcpy(emp->columns[2].name, "age");      emp->columns[2].type = VAL_INT;
    strcpy(emp->columns[3].name, "department"); emp->columns[3].type = VAL_STRING;
    strcpy(emp->columns[4].name, "salary");   emp->columns[4].type = VAL_FLOAT;

    const char* names[] = {"Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Hank", "Ivy", "Jack"};
    const char* depts[] = {"Engineering", "Marketing", "Engineering", "Sales", "Engineering", "Marketing", "Sales", "Engineering", "Marketing", "Engineering"};
    int ages[] = {28, 35, 42, 31, 26, 45, 33, 38, 29, 50};
    double sals[] = {95000, 72000, 120000, 68000, 88000, 78000, 65000, 110000, 70000, 135000};

    emp->num_rows = 10;
    for (int i = 0; i < 10; i++) {
        emp->rows[i].values[0] = val_int(i + 1);
        emp->rows[i].values[1] = val_str(names[i]);
        emp->rows[i].values[2] = val_int(ages[i]);
        emp->rows[i].values[3] = val_str(depts[i]);
        emp->rows[i].values[4] = val_float(sals[i]);
    }

    // products table
    Table* prod = &g_tables[1];
    strcpy(prod->name, "products");
    prod->num_cols = 4;
    strcpy(prod->columns[0].name, "id");       prod->columns[0].type = VAL_INT;
    strcpy(prod->columns[1].name, "name");     prod->columns[1].type = VAL_STRING;
    strcpy(prod->columns[2].name, "price");    prod->columns[2].type = VAL_FLOAT;
    strcpy(prod->columns[3].name, "category"); prod->columns[3].type = VAL_STRING;

    const char* pnames[] = {"Laptop", "Mouse", "Keyboard", "Monitor", "Headset", "Webcam", "USB Hub", "SSD"};
    const char* pcats[] = {"Electronics", "Accessories", "Accessories", "Electronics", "Accessories", "Accessories", "Accessories", "Electronics"};
    double prices[] = {999.99, 29.99, 79.99, 349.99, 149.99, 89.99, 39.99, 129.99};

    prod->num_rows = 8;
    for (int i = 0; i < 8; i++) {
        prod->rows[i].values[0] = val_int(i + 1);
        prod->rows[i].values[1] = val_str(pnames[i]);
        prod->rows[i].values[2] = val_float(prices[i]);
        prod->rows[i].values[3] = val_str(pcats[i]);
    }

    // orders table
    Table* orders = &g_tables[2];
    strcpy(orders->name, "orders");
    orders->num_cols = 5;
    strcpy(orders->columns[0].name, "id");         orders->columns[0].type = VAL_INT;
    strcpy(orders->columns[1].name, "customer");   orders->columns[1].type = VAL_STRING;
    strcpy(orders->columns[2].name, "product_id"); orders->columns[2].type = VAL_INT;
    strcpy(orders->columns[3].name, "quantity");    orders->columns[3].type = VAL_INT;
    strcpy(orders->columns[4].name, "total");       orders->columns[4].type = VAL_FLOAT;

    const char* customers[] = {"Alice", "Bob", "Charlie", "Alice", "Diana", "Eve", "Bob", "Frank"};
    int pids[] = {1, 2, 3, 4, 5, 1, 6, 7};
    int qtys[] = {1, 3, 1, 2, 1, 1, 2, 5};
    double totals[] = {999.99, 89.97, 79.99, 699.98, 149.99, 999.99, 179.98, 199.95};

    orders->num_rows = 8;
    for (int i = 0; i < 8; i++) {
        orders->rows[i].values[0] = val_int(i + 1);
        orders->rows[i].values[1] = val_str(customers[i]);
        orders->rows[i].values[2] = val_int(pids[i]);
        orders->rows[i].values[3] = val_int(qtys[i]);
        orders->rows[i].values[4] = val_float(totals[i]);
    }
}

/* ============================================================
 * SECTION 8: Public API
 * ============================================================ */

static QueryResult run_query(const char* sql) {
    QueryResult result;
    memset(&result, 0, sizeof(result));

    Lexer lex;
    lexer_init(&lex, sql);
    lexer_tokenize(&lex);

    if (lex.token_count == 0 || (lex.token_count == 1 && lex.tokens[0].type == TOK_EOF)) {
        result.has_error = true;
        strcpy(result.error_msg, "Empty query");
        return result;
    }

    // Check for lexer errors
    for (int i = 0; i < lex.token_count; i++) {
        if (lex.tokens[i].type == TOK_ERROR) {
            result.has_error = true;
            snprintf(result.error_msg, sizeof(result.error_msg), "Lexer error at line %d: '%s'", lex.tokens[i].line, lex.tokens[i].text);
            return result;
        }
    }

    Parser parser;
    memcpy(parser.tokens, lex.tokens, sizeof(Token) * lex.token_count);
    parser.num_tokens = lex.token_count;
    parser.pos = 0;
    parser.error[0] = '\0';

    QueryAST q = parse_query(&parser);

    if (parser.error[0]) {
        result.has_error = true;
        snprintf(result.error_msg, sizeof(result.error_msg), "Parse error: %s", parser.error);
        return result;
    }

    execute_query(&q, &result);
    return result;
}

static bool g_initialized = false;

#ifdef __EMSCRIPTEN__
static std::string run_query_wasm(const std::string& sql) {
    if (!g_initialized) {
        init_demo_data();
        g_initialized = true;
    }
    QueryResult r = run_query(sql.c_str());
    if (r.has_error) {
        return std::string("Error: ") + r.error_msg;
    }
    return std::string(r.output);
}

static std::string get_tables_info() {
    if (!g_initialized) {
        init_demo_data();
        g_initialized = true;
    }
    std::string info;
    for (int i = 0; i < g_num_tables; i++) {
        info += "Table: " + std::string(g_tables[i].name) + "\n";
        info += "  Columns: ";
        for (int j = 0; j < g_tables[i].num_cols; j++) {
            if (j > 0) info += ", ";
            info += g_tables[i].columns[j].name;
            info += "(";
            switch (g_tables[i].columns[j].type) {
                case VAL_INT: info += "INT"; break;
                case VAL_FLOAT: info += "FLOAT"; break;
                case VAL_STRING: info += "VARCHAR"; break;
                default: info += "NULL"; break;
            }
            info += ")";
        }
        info += "\n  Rows: " + std::to_string(g_tables[i].num_rows) + "\n\n";
    }
    return info;
}

EMSCRIPTEN_BINDINGS(dsl_module) {
    emscripten::function("runQuery", &run_query_wasm);
    emscripten::function("getTablesInfo", &get_tables_info);
}
#else
int main() {
    init_demo_data();
    g_initialized = true;

    printf("=== Relational DSL Interpreter ===\n");
    printf("Available tables: employees, products, orders\n\n");

    const char* queries[] = {
        "SELECT * FROM employees",
        "SELECT name, salary FROM employees WHERE age > 30 ORDER BY salary DESC",
        "SELECT name, price FROM products WHERE price < 100",
        "SELECT name, age FROM employees WHERE department == 'Engineering' ORDER BY age",
        NULL
    };

    for (int i = 0; queries[i]; i++) {
        printf("Query: %s\n", queries[i]);
        QueryResult r = run_query(queries[i]);
        if (r.has_error) printf("Error: %s\n", r.error_msg);
        else printf("%s\n", r.output);
    }
    return 0;
}
#endif
