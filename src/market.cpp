#include "market.hpp"
#include "http.hpp"
#include "json.hpp"
#include "util.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

// ==================== 数据源表 ====================
// 顺序即是设置页里的排列顺序；下标同时是配置里保存的序号（配置存的是 id 字符串）
enum {
    PF_OKX = 0, PF_BINANCE, PF_BYBIT, PF_GATE,
    PF_MEXC, PF_KUCOIN, PF_HTX, PF_BITGET, PF_COUNT
};

struct PfInfo {
    const char* id;       // 配置文件里的值
    const wchar_t* label; // 设置页按钮文字
    const wchar_t* host;  // 行情接口域名
};

static const PfInfo kPf[PF_COUNT] = {
    { "okx",     L"OKX",     L"www.okx.com"     },
    { "binance", L"Binance", L"api.binance.com" },
    { "bybit",   L"Bybit",   L"api.bybit.com"   },
    { "gate",    L"Gate",    L"api.gateio.ws"   },
    { "mexc",    L"MEXC",    L"api.mexc.com"    },
    { "kucoin",  L"KuCoin",  L"api.kucoin.com"  },
    { "htx",     L"HTX",     L"api.huobi.pro"   },
    { "bitget",  L"Bitget",  L"api.bitget.com"  },
};

int market_platform_count() { return PF_COUNT; }
const wchar_t* market_platform_label(int i) {
    return (i >= 0 && i < PF_COUNT) ? kPf[i].label : L"";
}
const char* market_platform_id(int i) {
    return (i >= 0 && i < PF_COUNT) ? kPf[i].id : "okx";
}
int market_platform_index(const std::string& id) {
    for (int i = 0; i < PF_COUNT; i++) if (id == kPf[i].id) return i;
    return PF_OKX;
}

// ==================== 周期映射 ====================
// 各平台对同一周期的写法差异很大（1m / 1 / 1min / 1hour ...），逐一映射。
// key 是内部统一值（沿用 OKX 写法，配置文件里的 kline_bar 保持不变）。
struct BarMap {
    const char* key;
    const char* v[PF_COUNT];   // 顺序与 kPf 一致
};

static const BarMap kBars[] = {
    // key    okx     binance  bybit  gate   mexc   kucoin   htx      bitget
    { "1m", { "1m",  "1m",    "1",    "1m",  "1m",  "1min",  "1min",  "1min"  } },
    { "5m", { "5m",  "5m",    "5",    "5m",  "5m",  "5min",  "5min",  "5min"  } },
    { "15m",{ "15m", "15m",   "15",   "15m", "15m", "15min", "15min", "15min" } },
    { "1H", { "1H",  "1h",    "60",   "1h",  "60m", "1hour", "60min", "1h"    } },
    { "4H", { "4H",  "4h",    "240",  "4h",  "4h",  "4hour", "4hour", "4h"    } },
    { "1D", { "1D",  "1d",    "D",    "1d",  "1d",  "1day",  "1day",  "1day"  } },
    { "1W", { "1W",  "1w",    "W",    "7d",  "1W",  "1week", "1week", "1week" } },
    { "1M", { "1M",  "1M",    "M",    "30d", "1M",  "1month","1mon",  "1M"    } },
    // 部分平台没有季度线，用月线兜底（走势形状接近，不影响使用）
    { "3M", { "3M",  "1M",    "M",    "30d", "1M",  "1month","1mon",  "1M"    } },
};
static const int kBarsCount = (int)(sizeof(kBars) / sizeof(kBars[0]));

static const BarMap& bar_map(const std::string& key) {
    for (int i = 0; i < kBarsCount; i++) if (key == kBars[i].key) return kBars[i];
    return kBars[0];
}

static std::string bar_for(int pf, const std::string& key) {
    const BarMap& m = bar_map(key);
    return (pf >= 0 && pf < PF_COUNT) ? m.v[pf] : m.v[PF_OKX];
}

// 一根 K 线覆盖的毫秒数（用于缓存时长与 KuCoin 的起始时间）
static unsigned long long bar_ms(const std::string& bar) {
    if (bar.empty()) return 60000ULL;
    char unit = bar.back();
    int n = atoi(bar.c_str());
    if (n < 1) n = 1;
    unsigned long long ms = 60000ULL;
    switch (unit) {
    case 'm': ms = 60000ULL; break;
    case 'H': ms = 3600000ULL; break;
    case 'D': ms = 86400000ULL; break;
    case 'W': ms = 604800000ULL; break;
    case 'M': ms = 2592000000ULL; break;
    default:  ms = 60000ULL; break;
    }
    return ms * (unsigned long long)n;
}

static unsigned long long bar_ttl_ms(const std::string& bar) {
    unsigned long long ttl = bar_ms(bar) / 8;
    if (ttl < 60000ULL) ttl = 60000ULL;
    if (ttl > 21600000ULL) ttl = 21600000ULL;
    return ttl;
}

// ==================== 币种符号 ====================
static const char* kQuotes[] = { "USDT", "USDC", "FDUSD", "TUSD", "BUSD", "DAI", "USD", "BTC", "ETH" };

std::string market_norm_symbol(const std::string& raw) {
    // 1) 按分隔符拆词，段内转大写（SNDK-USDT-SWAP -> ["SNDK","USDT","SWAP"]）
    std::vector<std::string> seg;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { seg.push_back(cur); cur.clear(); } };
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '/' || c == '\\') flush();
        else cur += (char)toupper((unsigned char)c);
    }
    flush();
    if (seg.empty()) return "";

    // 2) 识别尾部合约类型后缀（永续/交割）。OKX 的 instId 写作 BASE-QUOTE-SWAP，
    //    原逻辑会把 -SWAP 连进计价币导致请求非法；这里单独剥出来保留。
    static const char* kContracts[] = { "SWAP", "FUTURES", "PERP", "LINEAR", "INVERSE", nullptr };
    std::string contract;
    if (seg.size() >= 2) {
        const std::string& last = seg.back();
        for (int i = 0; kContracts[i]; ++i) {
            if (last == kContracts[i]) { contract = "-" + last; seg.pop_back(); break; }
        }
    }

    // 3) 剩余段拼成 BASE-QUOTE（最后一段是已知计价币时规范插入分隔符）
    std::string s;
    for (size_t i = 0; i < seg.size(); ++i) {
        if (i) s += "-";
        s += seg[i];
    }
    if (s.find('-') == std::string::npos && s.size() > 2) {
        for (const char* q : kQuotes) {
            size_t n = strlen(q);
            if (s.size() > n && s.compare(s.size() - n, n, q) == 0) {
                s = s.substr(0, s.size() - n) + "-" + q;
                break;
            }
        }
    }
    return s + contract;
}

// 统一符号 -> 指定平台的写法
static std::string sym_for(int pf, const std::string& norm) {
    std::string bare;
    bare.reserve(norm.size());
    for (char c : norm) if (c != '-') bare += c;

    switch (pf) {
    case PF_OKX:
    case PF_KUCOIN:
        return norm;                       // BTC-USDT
    case PF_HTX: {                         // btcusdt
        std::string l;
        l.reserve(bare.size());
        for (char c : bare) l += (char)tolower((unsigned char)c);
        return l;
    }
    case PF_GATE: {                        // BTC_USDT
        std::string u = bare;
        for (size_t i = 0; i < u.size(); i++) {
            // 在下划线处把基币和计价币分开：找已知后缀的起点
            for (const char* q : kQuotes) {
                size_t n = strlen(q);
                if (u.size() - i == n && u.compare(i, n, q) == 0)
                    return u.substr(0, i) + "_" + u.substr(i);
            }
        }
        return u;
    }
    default:
        return bare;                       // BTCUSDT
    }
}

// ==================== 请求地址 ====================
static std::wstring ticker_path(int pf, const std::string& norm) {
    const std::string s = sym_for(pf, norm);
    switch (pf) {
    case PF_OKX:     return L"/api/v5/market/ticker?instId=" + to_w(s);
    case PF_BINANCE: return L"/api/v3/ticker/24hr?symbol=" + to_w(s);
    case PF_BYBIT:   return L"/v5/market/tickers?category=spot&symbol=" + to_w(s);
    case PF_GATE:    return L"/api/v4/spot/tickers?currency_pair=" + to_w(s);
    case PF_MEXC:    return L"/api/v3/ticker/24hr?symbol=" + to_w(s);
    case PF_KUCOIN:  return L"/api/v1/market/stats?symbol=" + to_w(s);
    case PF_HTX:     return L"/market/detail/merged?symbol=" + to_w(s);
    case PF_BITGET:  return L"/api/v2/spot/market/tickers?symbol=" + to_w(s);
    }
    return L"";
}

static std::wstring kline_path(int pf, const std::string& norm, const std::string& bar) {
    const std::string s = sym_for(pf, norm);
    const std::string b = bar_for(pf, bar);
    switch (pf) {
    case PF_OKX:
        return L"/api/v5/market/candles?instId=" + to_w(s) + L"&bar=" + to_w(b) + L"&limit=30";
    case PF_BINANCE:
        return L"/api/v3/klines?symbol=" + to_w(s) + L"&interval=" + to_w(b) + L"&limit=30";
    case PF_BYBIT:
        return L"/v5/market/kline?category=spot&symbol=" + to_w(s) +
               L"&interval=" + to_w(b) + L"&limit=30";
    case PF_GATE:
        return L"/api/v4/spot/candlesticks?currency_pair=" + to_w(s) +
               L"&interval=" + to_w(b) + L"&limit=30";
    case PF_MEXC:
        return L"/api/v3/klines?symbol=" + to_w(s) + L"&interval=" + to_w(b) + L"&limit=30";
    case PF_KUCOIN: {
        // KuCoin 的 candles 固定返回上千根，用 startAt 只要最近 30 根
        unsigned long long now = (unsigned long long)time(NULL);
        unsigned long long from = now > bar_ms(bar) * 30ULL ? now - bar_ms(bar) * 30ULL : 0;
        wchar_t tail[64];
        swprintf(tail, 64, L"&startAt=%llu", from);
        return L"/api/v1/market/candles?type=" + to_w(b) + L"&symbol=" + to_w(s) + tail;
    }
    case PF_HTX:
        return L"/market/history/kline?symbol=" + to_w(s) + L"&period=" + to_w(b) + L"&size=30";
    case PF_BITGET:
        return L"/api/v2/spot/market/candles?symbol=" + to_w(s) +
               L"&granularity=" + to_w(b) + L"&limit=30";
    }
    return L"";
}

// ==================== 解析 ====================
// 各平台字段有的是字符串、有的是数字，统一按 double 取
static double jnum(const JsonValue& v) {
    if (v.type == JsonValue::String) return atof(v.str.c_str());
    if (v.type == JsonValue::Number) return v.number;
    return 0;
}

// 触发限频后全局冷却，避免继续撞墙
static unsigned long long g_cooldownUntil = 0;
static const unsigned long long COOLDOWN_MS = 5000;

static void set_cooldown() {
    g_cooldownUntil = GetTickCount64() + COOLDOWN_MS;
}

static bool is_rate_limit_msg(const std::string& s) {
    return s.find("429") != std::string::npos ||
           s.find("50011") != std::string::npos ||
           s.find("rate") != std::string::npos ||
           s.find("limit") != std::string::npos;
}

// 返回 false 表示失败（err 里带原因）
static bool parse_ticker(int pf, const JsonValue& root,
                         double& last, double& pct, std::string& err) {
    double open = 0;
    bool havePct = false;
    last = 0; pct = 0;

    switch (pf) {
    case PF_OKX: {
        const std::string& code = root["code"].str;
        if (code == "50011") { set_cooldown(); err = "限频，稍后重试"; return false; }
        if (code != "0") { err = "code " + code; return false; }
        if (root["data"].size() == 0) { err = "empty data"; return false; }
        const JsonValue& d = root["data"].at(0);
        last = jnum(d["last"]);
        open = jnum(d["open24h"]);
        break;
    }
    case PF_BINANCE: {
        const std::string& msg = root["msg"].str;
        if (!msg.empty()) { err = msg; if (is_rate_limit_msg(msg)) set_cooldown(); return false; }
        last = jnum(root["lastPrice"]);
        open = jnum(root["openPrice"]);
        break;
    }
    case PF_BYBIT: {
        if ((int)jnum(root["retCode"]) != 0) {
            err = root["retMsg"].str;
            if (is_rate_limit_msg(err)) set_cooldown();
            return false;
        }
        const JsonValue& l = root["result"]["list"];
        if (l.size() == 0) { err = "empty data"; return false; }
        last = jnum(l.at(0)["lastPrice"]);
        open = jnum(l.at(0)["prevPrice24h"]);
        break;
    }
    case PF_GATE: {
        // 直接返回数组；限频时是 {"label":"TOO_MANY_REQUESTS",...} 对象
        if (root.type != JsonValue::Array) {
            err = root["label"].str.empty() ? "bad response" : root["label"].str;
            if (is_rate_limit_msg(err)) set_cooldown();
            return false;
        }
        if (root.size() == 0) { err = "empty data"; return false; }
        last = jnum(root.at(0)["last"]);
        pct = jnum(root.at(0)["change_percentage"]);
        havePct = true;
        break;
    }
    case PF_MEXC: {
        const std::string& msg = root["msg"].str;
        if (!msg.empty()) { err = msg; if (is_rate_limit_msg(msg)) set_cooldown(); return false; }
        last = jnum(root["lastPrice"]);
        open = jnum(root["prevClosePrice"]);
        break;
    }
    case PF_KUCOIN: {
        const std::string& code = root["code"].str;
        if (code == "429000") { set_cooldown(); err = "限频，稍后重试"; return false; }
        if (code != "200000") { err = "code " + code; return false; }
        const JsonValue& d = root["data"];
        last = jnum(d["last"]);
        pct = jnum(d["changeRate"]) * 100.0;   // 小数比例
        havePct = true;
        break;
    }
    case PF_HTX: {
        const std::string& st = root["status"].str;
        if (st != "ok") { err = st.empty() ? "bad response" : st; return false; }
        const JsonValue& t = root["tick"];
        last = jnum(t["close"]);
        open = jnum(t["open"]);
        break;
    }
    case PF_BITGET: {
        const std::string& code = root["code"].str;
        if (code != "00000") {
            err = code.empty() ? "bad response" : ("code " + code);
            if (code == "429") set_cooldown();
            return false;
        }
        const JsonValue& d = root["data"];
        if (d.size() == 0) { err = "empty data"; return false; }
        last = jnum(d.at(0)["lastPr"]);
        open = jnum(d.at(0)["open"]);
        break;
    }
    default:
        err = "unknown platform";
        return false;
    }

    if (last <= 0) { err = "bad price"; return false; }
    if (!havePct) pct = (open > 0) ? (last - open) / open * 100.0 : 0.0;
    return true;
}

// K 线所在的数组节点
static const JsonValue& kline_list(int pf, const JsonValue& root) {
    switch (pf) {
    case PF_OKX:    return root["data"];
    case PF_BYBIT:  return root["result"]["list"];
    case PF_KUCOIN: return root["data"];
    case PF_HTX:    return root["data"];
    case PF_BITGET: return root["data"];
    default:        return root;      // Binance / MEXC / Gate 顶层直接就是数组
    }
}

// 收盘价在每行里的位置：-1 表示按字段名取（HTX 是对象数组）
static int close_index(int pf, bool& newestFirst) {
    switch (pf) {
    case PF_OKX:     newestFirst = true;  return 4;
    case PF_BINANCE: newestFirst = false; return 4;
    case PF_BYBIT:   newestFirst = true;  return 4;
    case PF_GATE:    newestFirst = false; return 2;
    case PF_MEXC:    newestFirst = false; return 4;
    case PF_KUCOIN:  newestFirst = true;  return 2;
    case PF_HTX:     newestFirst = true;  return -1;
    case PF_BITGET:  newestFirst = false; return 4;
    }
    newestFirst = true;
    return 4;
}

static bool parse_klines(int pf, const JsonValue& root, std::vector<double>& vals) {
    bool newestFirst = true;
    const int idx = close_index(pf, newestFirst);
    const JsonValue& list = kline_list(pf, root);

    std::vector<double> out;
    out.reserve(list.size());
    for (size_t k = 0; k < list.size(); k++) {
        const JsonValue& row = list.at(k);
        double c = (idx >= 0) ? jnum(row.at((size_t)idx)) : jnum(row["close"]);
        if (c > 0) out.push_back(c);
    }
    if (newestFirst) std::reverse(out.begin(), out.end());   // 统一成时间正序
    if (out.empty()) return false;
    // KuCoin 只按 type 返回固定上千根（startAt 不一定生效），只留最近的 30 根
    if (out.size() > 30) out.erase(out.begin(), out.end() - 30);
    vals.swap(out);
    return true;
}

// ==================== K 线缓存 ====================
struct CandleCache {
    std::vector<double> vals;
    unsigned long long at = 0;
};
static std::map<std::string, CandleCache> g_candles;

// 缓存由拉取线程读写，切换数据源时 UI 线程会清空它，必须加锁
static CRITICAL_SECTION g_csCache;
static INIT_ONCE g_onceCache = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cache_cs_init(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_csCache);
    return TRUE;
}
static void cache_lock() {
    InitOnceExecuteOnce(&g_onceCache, cache_cs_init, NULL, NULL);
    EnterCriticalSection(&g_csCache);
}
static void cache_unlock() { LeaveCriticalSection(&g_csCache); }

void market_clear_cache() {
    cache_lock();
    g_candles.clear();
    cache_unlock();
    // 切到别的平台就是换了一台服务器，上一家的限频冷却不该跟着过来，
    // 否则切完要白等最多 5 秒才出第一个报价
    g_cooldownUntil = 0;
}

// ==================== 对外接口 ====================
// 各家说「这个交易对我没有」的方式五花八门（HTTP 400、code 51001、
// "Not supported symbols"、data 为空…），统一成一句人能看懂的话。
// 原始信息只留给调试输出，界面上不展示错误码。
static std::string friendly_err(const std::string& raw) {
    if (raw.empty()) return "取数失败";
    if (raw == "timeout") return "请求超时";
    if (raw == "busy") return "上一轮未完成，已跳过";
    if (raw == "rate limited") return "刷新过频，本轮跳过";
    if (raw.find("http 400") != std::string::npos ||
        raw.find("http 404") != std::string::npos ||
        raw.find("Not supported") != std::string::npos ||
        raw.find("code 51001") != std::string::npos ||
        raw.find("code 51000") != std::string::npos ||
        raw == "bad price" || raw == "empty data" || raw == "error")
        return "该平台无此交易对";
    return raw;
}

// 失败收尾：q.err 给界面用，*err 给调用方/调试用，两处都写
static bool fetch_fail(CoinQuote& out, std::string* err, const std::string& msg) {
    out.err = friendly_err(msg);
    if (err) *err = msg;
    return false;
}

bool market_fetch(int pf, const std::string& sym, const std::string& bar,
                  CoinQuote& out, std::string* err) {
    out.instId = sym; out.ok = false; out.last = 0; out.open24h = 0;
    out.changePct = 0; out.spark.clear(); out.err.clear();

    if (pf < 0 || pf >= PF_COUNT) return fetch_fail(out, err, "invalid platform");
    if (GetTickCount64() < g_cooldownUntil) return fetch_fail(out, err, "限频冷却中");

    const std::string norm = market_norm_symbol(sym);
    const std::wstring host = kPf[pf].host;

    // 1) 最新价 + 24h 涨跌幅
    std::string body, e;
    if (!http_get(host, ticker_path(pf, norm), body, &e)) {
        if (is_rate_limit_msg(e)) set_cooldown();
        return fetch_fail(out, err, e);
    }
    JsonValue root;
    if (!JsonParser(body, &e).parse(root)) return fetch_fail(out, err, "json:" + e);

    double last = 0, pct = 0;
    std::string terr;
    if (!parse_ticker(pf, root, last, pct, terr)) return fetch_fail(out, err, terr);
    out.last = last;
    out.changePct = pct;
    out.open24h = (pct > -99.9 && last > 0) ? last / (1.0 + pct / 100.0) : 0;
    out.ok = true;

    // 2) 30 根 K 线（按「平台 + 币种 + 周期」缓存）
    const std::string key = std::to_string(pf) + "|" + norm + "|" + bar_map(bar).key;
    unsigned long long now = GetTickCount64();

    cache_lock();
    auto it = g_candles.find(key);
    bool need = (it == g_candles.end()) || (now - it->second.at >= bar_ttl_ms(bar));
    cache_unlock();

    if (need) {
        std::string cbody, ce;
        if (http_get(host, kline_path(pf, norm, bar), cbody, &ce)) {
            JsonValue croot;
            if (JsonParser(cbody, nullptr).parse(croot)) {
                std::vector<double> vals;
                if (parse_klines(pf, croot, vals)) {
                    CandleCache cc;
                    cc.vals.swap(vals);
                    cc.at = now;
                    cache_lock();                          // 网络请求期间不持锁
                    if (g_candles.size() > 64) g_candles.clear();  // 换周期/换平台后的残留
                    g_candles[key] = cc;
                    cache_unlock();
                }
            }
        } else if (is_rate_limit_msg(ce)) {
            set_cooldown();
        }
    }

    cache_lock();
    it = g_candles.find(key);
    if (it != g_candles.end()) out.spark = it->second.vals;
    cache_unlock();

    // 涨跌幅改为「跟随走势图周期」：用该周期 K 线窗口（时间正序：旧→新）的首末
    // 收盘算净涨跌，而不是固定的 24h。这样走势线和百分比都随周期设置联动；
    // K 线取不到时退回 ticker 给的 24h 涨跌幅，百分比不至于空白。
    if (out.spark.size() >= 2) {
        double a = out.spark.front(), b = out.spark.back();
        if (a > 0) out.changePct = (b - a) / a * 100.0;
    }
    return true;
}
