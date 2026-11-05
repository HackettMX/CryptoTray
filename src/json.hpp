#pragma once
#include <string>
#include <vector>
#include <map>

// 极简 JSON 解析器（仅覆盖 OKX 返回结构所需：对象/数组/字符串/数字/布尔/null）
// 不依赖任何第三方库，足够稳健地解析已知结构的响应。
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue& operator[](const std::string& key) const {
        static JsonValue nullv;
        if (type == Object) {
            auto it = object.find(key);
            if (it != object.end()) return it->second;
        }
        return nullv;
    }
    const JsonValue& at(size_t i) const {
        static JsonValue nullv;
        if (type == Array && i < array.size()) return array[i];
        return nullv;
    }
    size_t size() const {
        return type == Array ? array.size() : (type == Object ? object.size() : 0);
    }
};

class JsonParser {
    const std::string& s;
    size_t i = 0;
    std::string* err;
    void skipws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        std::string r;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') { out = r; return true; }
            if (c == '\\') {
                if (i >= s.size()) return false;
                char e = s[i++];
                switch (e) {
                case '"': r += '"'; break;
                case '\\': r += '\\'; break;
                case '/': r += '/'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case 'n': r += '\n'; break;
                case 'r': r += '\r'; break;
                case 't': r += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp += h - '0';
                        else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                        else return false;
                    }
                    if (cp < 0x80) r += (char)cp;
                    else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
                    else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: r += e; break;
                }
            } else r += c;
        }
        return false;
    }
    bool parseNumber(JsonValue& v) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i < s.size() && s[i] == '.') { i++; while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++; if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i == start) return false;
        v.type = JsonValue::Number;
        v.number = atof(s.substr(start, i - start).c_str());
        return true;
    }
    bool parseValue(JsonValue& v) {
        skipws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') return parseObject(v);
        if (c == '[') return parseArray(v);
        if (c == '"') { std::string st; if (!parseString(st)) return false; v.type = JsonValue::String; v.str = st; return true; }
        if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; v.type = JsonValue::Bool; v.boolean = true; return true; } return false; }
        if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; v.type = JsonValue::Bool; v.boolean = false; return true; } return false; }
        if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; v.type = JsonValue::Null; return true; } return false; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(v);
        return false;
    }
    bool parseObject(JsonValue& v) {
        v.type = JsonValue::Object; i++;
        skipws();
        if (i < s.size() && s[i] == '}') { i++; return true; }
        while (true) {
            skipws();
            if (i >= s.size() || s[i] != '"') return false;
            std::string key; if (!parseString(key)) return false;
            skipws();
            if (i >= s.size() || s[i] != ':') return false;
            i++;
            JsonValue val; if (!parseValue(val)) return false;
            v.object[key] = val;
            skipws();
            if (i >= s.size()) return false;
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; break; }
            return false;
        }
        return true;
    }
    bool parseArray(JsonValue& v) {
        v.type = JsonValue::Array; i++;
        skipws();
        if (i < s.size() && s[i] == ']') { i++; return true; }
        while (true) {
            JsonValue val; if (!parseValue(val)) return false;
            v.array.push_back(val);
            skipws();
            if (i >= s.size()) return false;
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; break; }
            return false;
        }
        return true;
    }
public:
    JsonParser(const std::string& src, std::string* e = nullptr) : s(src), err(e) {}
    bool parse(JsonValue& root) {
        skipws();
        if (!parseValue(root)) { if (err) *err = "parse error"; return false; }
        skipws();
        if (i < s.size()) { if (err) *err = "trailing chars"; return false; }
        return true;
    }
};
