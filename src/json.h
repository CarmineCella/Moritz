// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// json.h — a small JSON reader (for Max patches, which are JSON files)
#pragma once

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace moritz {
namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> items;                              // Array
    std::vector<std::pair<std::string, Value>> members;    // Object, in file order

    const Value* get(const std::string& key) const {
        for (auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
    // convenience accessors with defaults
    std::string getStr(const std::string& key, const std::string& def = "") const {
        const Value* v = get(key);
        return v && v->type == String ? v->str : def;
    }
    double getNum(const std::string& key, double def = 0.0) const {
        const Value* v = get(key);
        return v && v->type == Number ? v->num : v && v->type == Bool ? (v->b ? 1.0 : 0.0) : def;
    }
    bool isObject() const { return type == Object; }
    bool isArray() const { return type == Array; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(Value& out, std::string& err) {
        pos_ = 0;
        if (!value(out, 0)) { err = err_ + " (at byte " + std::to_string(pos_) + ")"; return false; }
        ws();
        if (pos_ != s_.size()) { err = "extra text after the JSON value"; return false; }
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    std::string err_;

    bool fail(const char* what) { err_ = what; return false; }
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s_.compare(pos_, n, w) != 0) return false;
        pos_ += n;
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return false;
        cp = (unsigned)std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16);
        pos_ += 4;
        return true;
    }
    bool string(std::string& o) {
        ++pos_;                                   // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {   // surrogate pair
                        pos_ += 2;
                        unsigned lo;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: o += e;                  // \" \\ \/
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v, int depth) {
        if (depth > 200) return fail("nested too deeply");
        ws();
        if (pos_ >= s_.size()) return fail("unexpected end");
        char c = s_[pos_];
        if (c == '{') {
            v.type = Value::Object;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            for (;;) {
                ws();
                if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
                ++pos_;
                v.members.emplace_back(std::move(key), Value());
                if (!value(v.members.back().second, depth + 1)) return false;
                ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1)) return false;
                ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Value::String; return string(v.str); }
        if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Value::Null; return true; }
        char* end = nullptr;
        double d = std::strtod(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_) return fail("unexpected character");
        v.type = Value::Number;
        v.num = d;
        v.str = s_.substr(pos_, (size_t)(end - (s_.c_str() + pos_)));   // as written: 2 vs 2.
        pos_ = (size_t)(end - s_.c_str());
        return true;
    }
};

}  // namespace json
}  // namespace moritz
