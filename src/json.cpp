// 极简 JSON：够读 config.json / cdn-cache.json / DoH 响应，够写回去。
// 不支持：注释、NaN/Inf 字面量、\u 代理对之外的转义（够用即可，多了是负担）。
#include "svpn.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace svpn {
namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }
    bool parse_value(Json& out, int depth);
    bool parse_string(std::string& out);
    bool parse_number(Json& out);
};

bool Parser::parse_string(std::string& out) {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    out.clear();
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p++);
        if (c == '"') return true;
        if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
        if (p >= end) return fail("truncated escape");
        char e = *p++;
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (end - p < 4) return fail("bad \\u escape");
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return fail("bad hex in \\u");
                }
                // UTF-8 编码（不处理代理对：配置里不会出现 emoji）
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return fail("unknown escape");
        }
    }
    return fail("unterminated string");
}

bool Parser::parse_number(Json& out) {
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    bool any = false;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                       *p == '+' || *p == '-')) {
        ++p;
        any = true;
    }
    if (!any) return fail("expected number");
    std::string text(start, p);
    char* endp = nullptr;
    double v = std::strtod(text.c_str(), &endp);
    if (endp == text.c_str()) return fail("bad number");
    out.type = JType::Num;
    out.num = v;
    return true;
}

bool Parser::parse_value(Json& out, int depth) {
    if (depth > 64) return fail("nesting too deep");
    skip_ws();
    if (p >= end) return fail("unexpected end");
    switch (*p) {
        case '{': {
            out = Json::make_obj();
            ++p;
            skip_ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (true) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!parse_value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        case '[': {
            out = Json::make_arr();
            ++p;
            skip_ws();
            if (p < end && *p == ']') { ++p; return true; }
            while (true) {
                Json v;
                if (!parse_value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        case '"': {
            out.type = JType::Str;
            return parse_string(out.str);
        }
        case 't':
            if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
                p += 4; out.type = JType::Bool; out.b = true; return true;
            }
            return fail("bad literal");
        case 'f':
            if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
                p += 5; out.type = JType::Bool; out.b = false; return true;
            }
            return fail("bad literal");
        case 'n':
            if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
                p += 4; out.type = JType::Null; return true;
            }
            return fail("bad literal");
        default:
            return parse_number(out);
    }
}

void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void dump_number(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "0"; return; }
    if (v == static_cast<double>(static_cast<long long>(v)) &&
        std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    out += buf;
}

void dump_value(std::string& out, const Json& j, int indent, int level) {
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent * (level + 1)), ' ') : "";
    const std::string pad_end = pretty ? std::string(static_cast<std::size_t>(indent * level), ' ') : "";
    switch (j.type) {
        case JType::Null: out += "null"; break;
        case JType::Bool: out += j.b ? "true" : "false"; break;
        case JType::Num: dump_number(out, j.num); break;
        case JType::Str: dump_string(out, j.str); break;
        case JType::Arr: {
            if (j.arr.empty()) { out += "[]"; break; }
            out += "[";
            for (std::size_t i = 0; i < j.arr.size(); ++i) {
                if (i) out += ",";
                if (pretty) { out += "\n"; out += pad; }
                dump_value(out, j.arr[i], indent, level + 1);
            }
            if (pretty) { out += "\n"; out += pad_end; }
            out += "]";
            break;
        }
        case JType::Obj: {
            if (j.obj.empty()) { out += "{}"; break; }
            out += "{";
            for (std::size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ",";
                if (pretty) { out += "\n"; out += pad; }
                dump_string(out, j.obj[i].first);
                out += pretty ? ": " : ":";
                dump_value(out, j.obj[i].second, indent, level + 1);
            }
            if (pretty) { out += "\n"; out += pad_end; }
            out += "}";
            break;
        }
    }
}

}  // namespace

Json Json::parse(const std::string& text, std::string* err) {
    Parser ps{text.c_str(), text.c_str() + text.size(), {}};
    Json out;
    // 跳过 UTF-8 BOM（Windows 记事本存的文件常带）
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        ps.p += 3;
    }
    if (!ps.parse_value(out, 0)) {
        if (err) *err = ps.err;
        return Json{};
    }
    if (err) err->clear();
    return out;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_value(out, *this, indent, 0);
    return out;
}

bool Json::has(const std::string& k) const { return get(k) != nullptr; }

const Json* Json::get(const std::string& k) const {
    if (type != JType::Obj) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == k) return &kv.second;
    }
    return nullptr;
}

bool Json::as_bool(bool def) const {
    switch (type) {
        case JType::Bool: return b;
        case JType::Num: return num != 0;
        case JType::Str: {
            std::string s = lower(str);
            if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
            if (s == "false" || s == "0" || s == "no" || s == "off") return false;
            return def;
        }
        default: return def;
    }
}

double Json::as_num(double def) const {
    switch (type) {
        case JType::Num: return std::isfinite(num) ? num : def;
        case JType::Bool: return b ? 1 : 0;
        case JType::Str: {
            char* endp = nullptr;
            double v = std::strtod(str.c_str(), &endp);
            if (endp == str.c_str() || !std::isfinite(v)) return def;
            return v;
        }
        default: return def;
    }
}

std::string Json::as_str(const std::string& def) const {
    switch (type) {
        case JType::Str: return str;
        case JType::Num: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.10g", num);
            return buf;
        }
        case JType::Bool: return b ? "true" : "false";
        default: return def;
    }
}

const std::vector<Json>* Json::as_arr() const { return type == JType::Arr ? &arr : nullptr; }

Json Json::make_obj() { Json j; j.type = JType::Obj; return j; }
Json Json::make_arr() { Json j; j.type = JType::Arr; return j; }
Json Json::make_str(const std::string& s) { Json j; j.type = JType::Str; j.str = s; return j; }
Json Json::make_num(double v) { Json j; j.type = JType::Num; j.num = v; return j; }
Json Json::make_bool(bool v) { Json j; j.type = JType::Bool; j.b = v; return j; }

Json& Json::set(const std::string& k, Json v) {
    for (auto& kv : obj) {
        if (kv.first == k) { kv.second = std::move(v); return *this; }
    }
    obj.emplace_back(k, std::move(v));
    return *this;
}

Json& Json::push(Json v) {
    arr.push_back(std::move(v));
    return *this;
}

}  // namespace svpn
