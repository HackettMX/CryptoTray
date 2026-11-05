#pragma once
#include <string>
#include <vector>

// 单个币种的行情视图（供 UI 使用）
struct CoinQuote {
    std::string instId;   // 统一格式，如 BTC-USDT
    double last = 0;      // 最新价
    double open24h = 0;   // 24h 开盘价（用于算涨跌幅）
    double changePct = 0; // 24h 涨跌幅(%)
    std::vector<double> spark; // 迷你走势线（收盘价序列，时间正序）
    bool ok = false;      // 是否成功取到数据
    std::string err;      // 失败原因
};

// ---- 数据源（交易所）----
// 全部使用各平台的公开行情接口，不需要 API Key。
int            market_platform_count();
const wchar_t* market_platform_label(int i);  // 界面显示名，如 L"Binance"
const char*    market_platform_id(int i);     // 配置值，如 "binance"
int            market_platform_index(const std::string& id); // 未知回落到 0

// 把用户输入整理成统一的 "BASE-QUOTE"：
// BTCUSDT / btc_usdt / btc-usdt / BTC-USDT 都归一成 BTC-USDT
std::string market_norm_symbol(const std::string& raw);

// 清空 K 线缓存（切换数据源后必须调用，否则会沿用上一个平台的走势）
void market_clear_cache();

// 拉取某币种行情（ticker + 30 根指定周期 K 线）
// bar 为内部统一周期：5m/15m/1H/4H/1D/1W/1M/3M
bool market_fetch(int pf, const std::string& sym, const std::string& bar,
                  CoinQuote& out, std::string* err);
