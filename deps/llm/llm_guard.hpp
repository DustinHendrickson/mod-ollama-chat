#pragma once
#define NOMINMAX

// llm_guard.hpp -- Zero-dependency single-header C++ PII detection and guard.
// Detects/scrubs: Email, Phone, SSN, CreditCard (Luhn), APIKey patterns.
// Prompt injection scoring. Fully offline, no curl required.
//
// USAGE:
//   #define LLM_GUARD_IMPLEMENTATION  (in exactly one .cpp)
//   #include "llm_guard.hpp"

#include <string>
#include <vector>

namespace llm {

enum class PiiType {
    Email,
    Phone,
    SSN,
    CreditCard,
    ApiKey,
};

struct PiiMatch {
    PiiType     type;
    std::string value;    // original matched text
    size_t      offset;   // byte offset in input
    size_t      length;   // byte length
};

struct GuardResult {
    std::string          scrubbed;   // text with PII replaced by placeholders
    std::vector<PiiMatch> matches;
    double               injection_score; // 0.0-1.0; >= 0.7 is suspicious
    bool                 has_pii;
    bool                 injection_detected; // injection_score >= 0.7
};

/// Scan text for PII and prompt injection signals.
GuardResult scan(const std::string& text);

/// Scrub only (returns scrubbed text, no match details).
std::string scrub(const std::string& text);

/// Score prompt injection likelihood (0.0-1.0).
double injection_score(const std::string& text);

} // namespace llm

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
#ifdef LLM_GUARD_IMPLEMENTATION

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace llm {
namespace detail_guard {

// ---------------------------------------------------------------------------
// Luhn check for credit card validation
// ---------------------------------------------------------------------------
static bool luhn_check(const std::string& digits) {
    if (digits.size() < 13 || digits.size() > 19) return false;
    int sum = 0;
    bool alt = false;
    for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
        if (!std::isdigit(static_cast<unsigned char>(digits[i]))) return false;
        int d = digits[i] - '0';
        if (alt) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
        alt = !alt;
    }
    return (sum % 10) == 0;
}

// ---------------------------------------------------------------------------
// Simple pattern matchers (no regex — hand-rolled state machines)
// ---------------------------------------------------------------------------

static bool is_digit(char c)  { return c >= '0' && c <= '9'; }
static bool is_alpha(char c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_alnum(char c)  { return is_digit(c) || is_alpha(c); }
static bool is_hex(char c)    { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Email: local@domain.tld
static bool try_email(const std::string& s, size_t start, size_t& end) {
    size_t i = start;
    if (i >= s.size() || !is_alnum(s[i])) return false;
    while (i < s.size() && (is_alnum(s[i]) || s[i] == '.' || s[i] == '_' || s[i] == '+' || s[i] == '-')) ++i;
    if (i >= s.size() || s[i] != '@') return false;
    ++i;
    if (i >= s.size() || !is_alnum(s[i])) return false;
    while (i < s.size() && (is_alnum(s[i]) || s[i] == '.' || s[i] == '-')) ++i;
    // must have at least one dot after @
    size_t dot = s.rfind('.', i - 1);
    if (dot == std::string::npos || dot <= start) return false;
    // tld must be at least 2 chars
    size_t tld_start = dot + 1;
    if (i - tld_start < 2) return false;
    end = i;
    return true;
}

// Phone: (ddd) ddd-dddd  or  ddd-ddd-dddd  or  ddd.ddd.dddd  or +1dddddddddd
static bool try_phone(const std::string& s, size_t start, size_t& end) {
    size_t i = start;
    int digits = 0;
    if (i < s.size() && s[i] == '+') ++i;
    // collect digit/sep chars up to 15 digits
    size_t run_start = i;
    while (i < s.size() && (is_digit(s[i]) || s[i] == '-' || s[i] == '.' || s[i] == ' '
                             || s[i] == '(' || s[i] == ')')) {
        if (is_digit(s[i])) ++digits;
        ++i;
    }
    if (digits < 10 || digits > 15) return false;
    end = i;
    return true;
}

// SSN: ddd-dd-dddd
static bool try_ssn(const std::string& s, size_t start, size_t& end) {
    if (start + 11 > s.size()) return false;
    auto ok = [&](size_t i, int count) {
        for (int j = 0; j < count; ++j)
            if (!is_digit(s[i + j])) return false;
        return true;
    };
    if (!ok(start, 3)) return false;
    if (s[start + 3] != '-') return false;
    if (!ok(start + 4, 2)) return false;
    if (s[start + 6] != '-') return false;
    if (!ok(start + 7, 4)) return false;
    end = start + 11;
    return true;
}

// Credit card: 13-19 digit sequences (optionally space/dash separated), validated by Luhn
static bool try_creditcard(const std::string& s, size_t start, size_t& end) {
    size_t i = start;
    std::string digits;
    while (i < s.size() && (is_digit(s[i]) || s[i] == ' ' || s[i] == '-')) {
        if (is_digit(s[i])) digits += s[i];
        ++i;
        if (digits.size() > 19) break;
    }
    if (digits.size() < 13) return false;
    // try longest valid Luhn prefix
    while (digits.size() >= 13) {
        if (luhn_check(digits)) {
            // find end offset: count digits+seps
            size_t j = start;
            size_t needed = digits.size();
            size_t found = 0;
            while (j < s.size() && found < needed) {
                if (is_digit(s[j])) ++found;
                else if (s[j] != ' ' && s[j] != '-') break;
                ++j;
            }
            end = j;
            return true;
        }
        digits.pop_back();
    }
    return false;
}

// API key heuristic: 20+ char alphanumeric/dash/underscore token starting with
// known prefixes (sk-, pk-, ghp_, etc.) or long hex strings
static bool try_apikey(const std::string& s, size_t start, size_t& end) {
    // Check known prefixes
    static const char* prefixes[] = {
        "sk-", "pk-", "ghp_", "gho_", "ghs_", "ghu_", "Bearer ", "token_", nullptr
    };
    bool has_prefix = false;
    size_t i = start;
    for (int p = 0; prefixes[p]; ++p) {
        size_t plen = strlen(prefixes[p]);
        if (start + plen <= s.size() &&
            s.substr(start, plen) == prefixes[p]) {
            i = start + plen;
            has_prefix = true;
            break;
        }
    }
    if (!has_prefix) return false;
    size_t key_start = i;
    while (i < s.size() && (is_alnum(s[i]) || s[i] == '-' || s[i] == '_')) ++i;
    if (i - key_start < 16) return false;
    end = i;
    return true;
}

// ---------------------------------------------------------------------------
// Prompt injection scoring
// ---------------------------------------------------------------------------
static double compute_injection_score(const std::string& text) {
    std::string lower = text;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const char* signals[] = {
        "ignore previous instructions",
        "ignore all instructions",
        "disregard the above",
        "forget your instructions",
        "you are now",
        "new personality",
        "jailbreak",
        "dan mode",
        "act as if",
        "pretend you are",
        "bypass",
        "override",
        "system prompt",
        "your instructions are",
        nullptr
    };

    int hits = 0;
    for (int i = 0; signals[i]; ++i)
        if (lower.find(signals[i]) != std::string::npos) ++hits;

    // each hit contributes; cap at 1.0
    double score = std::min(1.0, hits * 0.25);
    return score;
}

} // namespace detail_guard

// ---------------------------------------------------------------------------

GuardResult scan(const std::string& text) {
    GuardResult result;
    result.scrubbed       = text;
    result.injection_score = detail_guard::compute_injection_score(text);
    result.injection_detected = result.injection_score >= 0.7;

    // We'll build the scrubbed text by replacing matches with placeholders.
    // Collect all matches first, then apply non-overlapping replacements.
    std::vector<PiiMatch> raw;

    const std::string& s = text;
    for (size_t i = 0; i < s.size(); ++i) {
        size_t end = 0;

        // SSN (check before phone to avoid partial match)
        if (detail_guard::try_ssn(s, i, end)) {
            raw.push_back({PiiType::SSN, s.substr(i, end - i), i, end - i});
            i = end - 1;
            continue;
        }
        // Credit card
        if (detail_guard::try_creditcard(s, i, end)) {
            raw.push_back({PiiType::CreditCard, s.substr(i, end - i), i, end - i});
            i = end - 1;
            continue;
        }
        // Email
        if (detail_guard::try_email(s, i, end)) {
            raw.push_back({PiiType::Email, s.substr(i, end - i), i, end - i});
            i = end - 1;
            continue;
        }
        // API key
        if (detail_guard::try_apikey(s, i, end)) {
            raw.push_back({PiiType::ApiKey, s.substr(i, end - i), i, end - i});
            i = end - 1;
            continue;
        }
        // Phone (only if starts with digit or +)
        if ((s[i] == '+' || detail_guard::is_digit(s[i])) &&
            detail_guard::try_phone(s, i, end)) {
            raw.push_back({PiiType::Phone, s.substr(i, end - i), i, end - i});
            i = end - 1;
            continue;
        }
    }

    result.matches = raw;
    result.has_pii = !raw.empty();

    // Build scrubbed string (replace from back to front to preserve offsets)
    static const char* labels[] = {
        "[EMAIL]", "[PHONE]", "[SSN]", "[CREDIT_CARD]", "[API_KEY]"
    };
    std::string scrubbed = text;
    // sort by offset descending
    std::sort(raw.begin(), raw.end(),
              [](const PiiMatch& a, const PiiMatch& b){ return a.offset > b.offset; });
    for (const auto& m : raw) {
        const char* label = labels[static_cast<int>(m.type)];
        scrubbed.replace(m.offset, m.length, label);
    }
    result.scrubbed = scrubbed;

    return result;
}

std::string scrub(const std::string& text) {
    return scan(text).scrubbed;
}

double injection_score(const std::string& text) {
    return detail_guard::compute_injection_score(text);
}

} // namespace llm
#endif // LLM_GUARD_IMPLEMENTATION
