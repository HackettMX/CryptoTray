// 数据层冒烟测试（控制台程序）：验证各平台公开接口可达、JSON 解析与涨跌幅/走势线提取正确
// 用法：test_okx [platform] [instId] [bar]
//   platform: okx / binance / bybit / gate / mexc / kucoin / htx / bitget
//   例：test_okx binance BTC-USDT 1m      test_okx            （默认 OKX）
//       test_okx all                       （遍历所有平台）
#include "market.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

static void one(int pf, const char* inst, const char* bar) {
    CoinQuote q; std::string e;
    unsigned long long t0 = GetTickCount64();
    bool ok = market_fetch(pf, inst, bar, q, &e);
    unsigned long long dt = GetTickCount64() - t0;
    printf("%-8s ok=%d last=%.4f change=%+.2f%% spark=%d %llums err=%s\n",
           market_platform_id(pf), q.ok ? 1 : 0, q.last, q.changePct,
           (int)q.spark.size(), dt, ok ? q.err.c_str() : e.c_str());
}

int main(int argc, char** argv) {
    const char* arg1 = (argc > 1) ? argv[1] : nullptr;
    const char* inst = (argc > 2) ? argv[2] : "BTC-USDT";
    const char* bar  = (argc > 3) ? argv[3] : "1m";

    if (arg1 && std::string(arg1) == "all") {
        for (int i = 0; i < market_platform_count(); i++) {
            int pf = i;                       // 逐个平台，各自冷启动（缓存互不影响）
            one(pf, inst, bar);
        }
        return 0;
    }

    int pf = 0;
    if (arg1) {
        for (int i = 0; i < market_platform_count(); i++)
            if (std::string(arg1) == market_platform_id(i)) { pf = i; break; }
    }
    one(pf, inst, bar);
    return 0;
}
