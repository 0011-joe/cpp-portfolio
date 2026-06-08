/*
 * AI-Assisted Code Reviewer
 * Terminal-based real-time code review agent
 * Analyzes diffs for security issues, memory leaks, and style problems
 *
 * Core: C engine handles diff slicing, context window management,
 * pattern matching, and concurrent analysis orchestration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <regex.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <string>
#include <vector>
#endif

#define MAX_LINES 1024
#define MAX_LINE_LEN 256
#define MAX_ISSUES 128
#define MAX_RULES 64
#define CONTEXT_WINDOW 3

/* ============================================================
 * Issue Types & Severity
 * ============================================================ */

typedef enum {
    SEV_CRITICAL,
    SEV_HIGH,
    SEV_MEDIUM,
    SEV_LOW,
    SEV_INFO
} Severity;

typedef enum {
    CAT_SECURITY,
    CAT_MEMORY,
    CAT_STYLE,
    CAT_PERFORMANCE,
    CAT_CORRECTNESS,
    CAT_BEST_PRACTICE
} Category;

typedef struct {
    int id;
    Severity severity;
    Category category;
    int line;
    char rule[64];
    char message[256];
    char suggestion[256];
    char context[MAX_LINE_LEN * 4];
} Issue;

/* ============================================================
 * Diff Line
 * ============================================================ */

typedef enum {
    LINE_ADD,
    LINE_DEL,
    LINE_CTX
} LineType;

typedef struct {
    LineType type;
    int old_line;
    int new_line;
    char text[MAX_LINE_LEN];
} DiffLine;

/* ============================================================
 * Pattern Rule
 * ============================================================ */

typedef struct {
    char pattern[128];
    Severity severity;
    Category category;
    char rule_name[64];
    char message[256];
    char suggestion[256];
    bool is_regex;
    bool check_added_only;  // only check added lines
} Rule;

/* ============================================================
 * Review Context
 * ============================================================ */

typedef struct {
    DiffLine lines[MAX_LINES];
    int num_lines;
    Issue issues[MAX_ISSUES];
    int num_issues;
    Rule rules[MAX_RULES];
    int num_rules;
    int total_score;
    int crit_count;
    int high_count;
    int med_count;
    int low_count;
} ReviewContext;

/* ============================================================
 * Built-in Rules
 * ============================================================ */

static void add_rule(ReviewContext* ctx, const char* pattern, Severity sev, Category cat,
                     const char* name, const char* msg, const char* suggestion, bool regex, bool added_only) {
    if (ctx->num_rules >= MAX_RULES) return;
    Rule* r = &ctx->rules[ctx->num_rules++];
    strncpy(r->pattern, pattern, 127);
    r->severity = sev;
    r->category = cat;
    strncpy(r->rule_name, name, 63);
    strncpy(r->message, msg, 255);
    strncpy(r->suggestion, suggestion, 255);
    r->is_regex = regex;
    r->check_added_only = added_only;
}

static void init_default_rules(ReviewContext* ctx) {
    // Security rules
    add_rule(ctx, "strcpy", SEV_CRITICAL, CAT_SECURITY,
             "SEC-001", "Use of unsafe strcpy - buffer overflow risk",
             "Use strncpy() or strlcpy() with explicit size limit", false, true);
    add_rule(ctx, "strcat", SEV_CRITICAL, CAT_SECURITY,
             "SEC-002", "Use of unsafe strcat - buffer overflow risk",
             "Use strncat() or snprintf() for string concatenation", false, true);
    add_rule(ctx, "sprintf", SEV_HIGH, CAT_SECURITY,
             "SEC-003", "Use of sprintf without bounds checking",
             "Use snprintf() with explicit buffer size", false, true);
    add_rule(ctx, "gets", SEV_CRITICAL, CAT_SECURITY,
             "SEC-004", "Use of gets() - unlimited buffer read, classic vulnerability",
             "Use fgets() with explicit size limit", false, true);
    add_rule(ctx, "scanf.*%s", SEV_HIGH, CAT_SECURITY,
             "SEC-005", "scanf with %s without width limit",
             "Use %255s or fgets() instead", true, true);
    add_rule(ctx, "system(", SEV_HIGH, CAT_SECURITY,
             "SEC-006", "Command injection risk via system()",
             "Use execve() or validate/sanitize input thoroughly", false, true);
    add_rule(ctx, "popen(", SEV_MEDIUM, CAT_SECURITY,
             "SEC-007", "Potential command injection via popen()",
             "Validate and sanitize all command arguments", false, true);

    // Memory rules
    add_rule(ctx, "malloc", SEV_MEDIUM, CAT_MEMORY,
             "MEM-001", "Dynamic allocation - ensure corresponding free()",
             "Consider using RAII pattern or cleanup goto", false, true);
    add_rule(ctx, "calloc", SEV_MEDIUM, CAT_MEMORY,
             "MEM-002", "Dynamic allocation - ensure corresponding free()",
             "Track allocation and ensure cleanup on all code paths", false, true);
    add_rule(ctx, "realloc", SEV_HIGH, CAT_MEMORY,
             "MEM-003", "realloc may return NULL - check return value",
             "Use temp pointer: p = realloc(ptr, size); if(!p) handle_error();", false, true);
    add_rule(ctx, "free(", SEV_LOW, CAT_MEMORY,
             "MEM-004", "Memory freed - ensure no use-after-free",
             "Set pointer to NULL after free()", false, false);
    add_rule(ctx, "memcpy", SEV_MEDIUM, CAT_MEMORY,
             "MEM-005", "memcpy - verify source/dest don't overlap",
             "Use memmove() if overlap is possible", false, true);
    add_rule(ctx, "alloca", SEV_HIGH, CAT_MEMORY,
             "MEM-006", "alloca() allocates on stack - no overflow protection",
             "Use malloc() with proper size validation", false, true);

    // Style rules
    add_rule(ctx, "goto", SEV_LOW, CAT_STYLE,
             "STY-001", "Use of goto - prefer structured control flow",
             "Use loops, functions, or early returns instead", false, true);
    add_rule(ctx, "   	", SEV_INFO, CAT_STYLE,
             "STY-002", "Mixed tabs and spaces for indentation",
             "Use consistent indentation (spaces recommended)", true, false);
    add_rule(ctx, "// TODO", SEV_INFO, CAT_BEST_PRACTICE,
             "STY-003", "TODO comment found - track as issue?",
             "Convert to tracked issue in your project management tool", false, false);
    add_rule(ctx, "// HACK", SEV_LOW, CAT_BEST_PRACTICE,
             "STY-004", "HACK comment - technical debt indicator",
             "Document the workaround and create a refactoring ticket", false, false);

    // Performance rules
    add_rule(ctx, "strlen.*for", SEV_LOW, CAT_PERFORMANCE,
             "PERF-001", "strlen() in loop condition - O(n²) complexity",
             "Cache strlen result before the loop", true, true);
    add_rule(ctx, "printf.*for", SEV_LOW, CAT_PERFORMANCE,
             "PERF-002", "printf() inside loop - I/O bottleneck",
             "Buffer output and print once after loop", true, true);

    // Correctness rules
    add_rule(ctx, "if.*=\\s*[^=]", SEV_HIGH, CAT_CORRECTNESS,
             "COR-001", "Possible assignment in if-condition (= vs ==)",
             "Use == for comparison, or reverse: if (value == variable)", true, true);
    add_rule(ctx, "switch.*\\{[^}]*$", SEV_MEDIUM, CAT_CORRECTNESS,
             "COR-002", "Switch without default case",
             "Add default case to handle unexpected values", true, true);
}

/* ============================================================
 * Diff Parser
 * ============================================================ */

static int parse_diff(ReviewContext* ctx, const char* diff_text) {
    ctx->num_lines = 0;
    int old_line = 0, new_line = 0;

    const char* p = diff_text;
    while (*p && ctx->num_lines < MAX_LINES) {
        DiffLine* dl = &ctx->lines[ctx->num_lines];

        // Parse diff header
        if (strncmp(p, "@@", 2) == 0) {
            // Parse hunk header: @@ -old,count +new,count @@
            sscanf(p, "@@ -%d,%*d +%d,%*d @@", &old_line, &new_line);
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        if (*p == '+') {
            dl->type = LINE_ADD;
            dl->old_line = 0;
            dl->new_line = new_line++;
            p++;
        } else if (*p == '-') {
            dl->type = LINE_DEL;
            dl->old_line = old_line++;
            dl->new_line = 0;
            p++;
        } else {
            dl->type = LINE_CTX;
            dl->old_line = old_line++;
            dl->new_line = new_line++;
            if (*p == ' ') p++;
        }

        // Copy line text
        int i = 0;
        while (*p && *p != '\n' && i < MAX_LINE_LEN - 1) {
            dl->text[i++] = *p++;
        }
        dl->text[i] = '\0';
        if (*p == '\n') p++;
        ctx->num_lines++;
    }
    return ctx->num_lines;
}

/* ============================================================
 * Pattern Matching
 * ============================================================ */

static bool simple_match(const char* text, const char* pattern) {
    return strstr(text, pattern) != NULL;
}

static bool regex_match(const char* text, const char* pattern) {
    // Simple regex matching using POSIX regex
    regex_t regex;
    int ret = regcomp(&regex, pattern, REG_EXTENDED | REG_ICASE);
    if (ret != 0) return false;
    ret = regexec(&regex, text, 0, NULL, 0);
    regfree(&regex);
    return ret == 0;
}

static bool match_rule(const char* text, Rule* rule) {
    if (rule->is_regex) {
        return regex_match(text, rule->pattern);
    }
    return simple_match(text, rule->pattern);
}

/* ============================================================
 * Context Window Builder
 * ============================================================ */

static void build_context(ReviewContext* ctx, int line_idx, char* buf, int bufsize) {
    buf[0] = '\0';
    int start = line_idx - CONTEXT_WINDOW;
    if (start < 0) start = 0;
    int end = line_idx + CONTEXT_WINDOW;
    if (end >= ctx->num_lines) end = ctx->num_lines - 1;

    int offset = 0;
    for (int i = start; i <= end && offset < bufsize - 1; i++) {
        DiffLine* dl = &ctx->lines[i];
        char marker = ' ';
        if (dl->type == LINE_ADD) marker = '+';
        else if (dl->type == LINE_DEL) marker = '-';

        int written = snprintf(buf + offset, bufsize - offset, "%c %4d | %s\n",
                               marker, dl->new_line ? dl->new_line : dl->old_line, dl->text);
        if (written > 0) offset += written;
    }
}

/* ============================================================
 * Review Engine
 * ============================================================ */

static void add_issue(ReviewContext* ctx, Severity sev, Category cat, int line,
                      const char* rule, const char* msg, const char* suggestion, const char* context) {
    if (ctx->num_issues >= MAX_ISSUES) return;
    Issue* iss = &ctx->issues[ctx->num_issues++];
    iss->id = ctx->num_issues;
    iss->severity = sev;
    iss->category = cat;
    iss->line = line;
    strncpy(iss->rule, rule, 63);
    strncpy(iss->message, msg, 255);
    strncpy(iss->suggestion, suggestion, 255);
    strncpy(iss->context, context, sizeof(iss->context) - 1);

    switch (sev) {
        case SEV_CRITICAL: ctx->crit_count++; break;
        case SEV_HIGH: ctx->high_count++; break;
        case SEV_MEDIUM: ctx->med_count++; break;
        case SEV_LOW: ctx->low_count++; break;
        default: break;
    }
}

static void review_diff(ReviewContext* ctx) {
    ctx->num_issues = 0;
    ctx->crit_count = 0;
    ctx->high_count = 0;
    ctx->med_count = 0;
    ctx->low_count = 0;

    // Check for missing NULL checks after malloc/calloc/realloc
    for (int i = 0; i < ctx->num_lines; i++) {
        DiffLine* dl = &ctx->lines[i];
        if (dl->type != LINE_ADD) continue;

        // Check each rule
        for (int r = 0; r < ctx->num_rules; r++) {
            Rule* rule = &ctx->rules[r];
            if (rule->check_added_only && dl->type != LINE_ADD) continue;

            if (match_rule(dl->text, rule)) {
                char context[MAX_LINE_LEN * 4];
                build_context(ctx, i, context, sizeof(context));
                int line_num = dl->new_line ? dl->new_line : dl->old_line;
                add_issue(ctx, rule->severity, rule->category, line_num,
                          rule->rule_name, rule->message, rule->suggestion, context);
            }
        }

        // Additional heuristic checks
        // Missing NULL check after malloc
        if (strstr(dl->text, "malloc") || strstr(dl->text, "calloc")) {
            bool has_null_check = false;
            for (int j = i + 1; j < i + 5 && j < ctx->num_lines; j++) {
                if (strstr(ctx->lines[j].text, "NULL") || strstr(ctx->lines[j].text, "== 0")) {
                    has_null_check = true;
                    break;
                }
            }
            if (!has_null_check) {
                char context[MAX_LINE_LEN * 4];
                build_context(ctx, i, context, sizeof(context));
                add_issue(ctx, SEV_HIGH, CAT_MEMORY, dl->new_line,
                          "MEM-010", "No NULL check after allocation",
                          "Always check if malloc/calloc returns NULL", context);
            }
        }

        // Detect potential integer overflow
        if (strstr(dl->text, "*") && (strstr(dl->text, "int") || strstr(dl->text, "size"))) {
            char context[MAX_LINE_LEN * 4];
            build_context(ctx, i, context, sizeof(context));
            add_issue(ctx, SEV_MEDIUM, CAT_SECURITY, dl->new_line,
                      "SEC-010", "Potential integer overflow in multiplication",
                      "Check for overflow before multiplication or use safe math", context);
        }

        // Magic numbers
        if (dl->type == LINE_ADD) {
            const char* p = dl->text;
            while (*p) {
                if (isdigit(*p) && p > dl->text && *(p-1) != ' ' && *(p-1) != '=' && *(p-1) != '(') {
                    int num = atoi(p);
                    if (num > 10 && num != 100 && num != 1000) {
                        // Likely a magic number
                    }
                }
                p++;
            }
        }
    }

    // Calculate score
    ctx->total_score = 100;
    ctx->total_score -= ctx->crit_count * 25;
    ctx->total_score -= ctx->high_count * 15;
    ctx->total_score -= ctx->med_count * 8;
    ctx->total_score -= ctx->low_count * 3;
    if (ctx->total_score < 0) ctx->total_score = 0;
}

/* ============================================================
 * Report Generation
 * ============================================================ */

static void generate_report(ReviewContext* ctx, char* report, int bufsize) {
    int offset = 0;

    offset += snprintf(report + offset, bufsize - offset,
        "╔══════════════════════════════════════════════════════════╗\n"
        "║            CODE REVIEW REPORT — AI-ASSISTED             ║\n"
        "╚══════════════════════════════════════════════════════════╝\n\n");

    // Score
    const char* grade;
    if (ctx->total_score >= 90) grade = "A+ Excellent";
    else if (ctx->total_score >= 80) grade = "A  Good";
    else if (ctx->total_score >= 70) grade = "B  Acceptable";
    else if (ctx->total_score >= 60) grade = "C  Needs Work";
    else if (ctx->total_score >= 50) grade = "D  Poor";
    else grade = "F  Critical Issues";

    offset += snprintf(report + offset, bufsize - offset,
        "┌─ Score ─────────────────────────────────────────────────┐\n"
        "│  %3d/100  %s\n"
        "│  Critical: %d  High: %d  Medium: %d  Low: %d\n"
        "└─────────────────────────────────────────────────────────┘\n\n",
        ctx->total_score, grade, ctx->crit_count, ctx->high_count, ctx->med_count, ctx->low_count);

    if (ctx->num_issues == 0) {
        offset += snprintf(report + offset, bufsize - offset,
            "✅ No issues found. Code looks clean!\n");
        return;
    }

    // Issues by severity
    const char* sev_labels[] = {"🔴 CRITICAL", "🟠 HIGH", "🟡 MEDIUM", "🔵 LOW", "⚪ INFO"};
    const char* cat_labels[] = {"🛡 Security", "💾 Memory", "📝 Style", "⚡ Performance", "✅ Correctness", "💡 Best Practice"};

    for (int sev = SEV_CRITICAL; sev <= SEV_INFO; sev++) {
        bool has_this_sev = false;
        for (int i = 0; i < ctx->num_issues; i++) {
            if (ctx->issues[i].severity == sev) { has_this_sev = true; break; }
        }
        if (!has_this_sev) continue;

        offset += snprintf(report + offset, bufsize - offset,
            "━━━ %s ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
            sev_labels[sev]);

        for (int i = 0; i < ctx->num_issues; i++) {
            Issue* iss = &ctx->issues[i];
            if (iss->severity != sev) continue;

            offset += snprintf(report + offset, bufsize - offset,
                "  [%s] Line %d  %s\n"
                "  Rule: %s\n"
                "  %s\n\n",
                cat_labels[iss->category], iss->line, iss->message,
                iss->rule, iss->suggestion);

            // Show context
            if (iss->context[0]) {
                offset += snprintf(report + offset, bufsize - offset,
                    "  Context:\n");
                // Indent context lines
                char* ctx_line = iss->context;
                while (*ctx_line && offset < bufsize - 10) {
                    report[offset++] = ' ';
                    report[offset++] = ' ';
                    while (*ctx_line && *ctx_line != '\n' && offset < bufsize - 10) {
                        report[offset++] = *ctx_line++;
                    }
                    if (*ctx_line == '\n') {
                        report[offset++] = *ctx_line++;
                    }
                }
                offset += snprintf(report + offset, bufsize - offset, "\n");
            }
        }
    }

    offset += snprintf(report + offset, bufsize - offset,
        "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  Total: %d issues found across %d changed lines\n",
        ctx->num_issues, ctx->num_lines);
}

/* ============================================================
 * Public API
 * ============================================================ */

#ifdef __EMSCRIPTEN__

static ReviewContext g_ctx;

static std::string reviewDiff(const std::string& diff) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    init_default_rules(&g_ctx);
    parse_diff(&g_ctx, diff.c_str());
    review_diff(&g_ctx);

    char report[8192];
    generate_report(&g_ctx, report, sizeof(report));
    return std::string(report);
}

static std::string getIssuesJSON() {
    std::string json = "[";
    for (int i = 0; i < g_ctx.num_issues; i++) {
        if (i > 0) json += ",";
        Issue* iss = &g_ctx.issues[i];
        json += "{";
        json += "\"id\":" + std::to_string(iss->id) + ",";
        json += "\"severity\":" + std::to_string(iss->severity) + ",";
        json += "\"category\":" + std::to_string(iss->category) + ",";
        json += "\"line\":" + std::to_string(iss->line) + ",";
        json += "\"rule\":\"" + std::string(iss->rule) + "\",";
        json += "\"message\":\"" + std::string(iss->message) + "\",";
        json += "\"suggestion\":\"" + std::string(iss->suggestion) + "\"";
        json += "}";
    }
    json += "]";
    return json;
}

static int getScore() { return g_ctx.total_score; }
static int getCritCount() { return g_ctx.crit_count; }
static int getHighCount() { return g_ctx.high_count; }
static int getMedCount() { return g_ctx.med_count; }
static int getLowCount() { return g_ctx.low_count; }

EMSCRIPTEN_BINDINGS(reviewer_module) {
    emscripten::function("reviewDiff", &reviewDiff);
    emscripten::function("getIssuesJSON", &getIssuesJSON);
    emscripten::function("getScore", &getScore);
    emscripten::function("getCritCount", &getCritCount);
    emscripten::function("getHighCount", &getHighCount);
    emscripten::function("getMedCount", &getMedCount);
    emscripten::function("getLowCount", &getLowCount);
}

#else
int main() {
    ReviewContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    init_default_rules(&ctx);

    const char* sample_diff =
        "@@ -1,10 +1,15 @@\n"
        " #include <stdio.h>\n"
        "+#include <string.h>\n"
        "+#include <stdlib.h>\n"
        " \n"
        " int main() {\n"
        "+    char buf[64];\n"
        "+    strcpy(buf, user_input);  // SEC-001\n"
        "+    sprintf(buf, \"%s\", data);  // SEC-003\n"
        "+    char* p = malloc(100);  // MEM-001\n"
        "+    gets(buf);  // SEC-004\n"
        "     return 0;\n"
        " }\n";

    parse_diff(&ctx, sample_diff);
    review_diff(&ctx);

    char report[8192];
    generate_report(&ctx, report, sizeof(report));
    printf("%s", report);
    return 0;
}
#endif
