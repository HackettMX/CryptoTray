// 限频/时延验证（控制台）
// 用法：test_rate            -> 模拟「窗口显示时刷新 = 1 秒」，4 个币，持续 10 秒
//       test_rate <币数> <秒数>
#include "http.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const wchar_t* kCoins[] = {
    L"BTC-USDT", L"ETH-USDT", L"SOL-USDT", L"DOGE-USDT",
    L"XRP-USDT", L"ADA-USDT", L"BNB-USDT", L"TON-USDT"
};

int main(int argc, char** argv) {
    int nCoins = (argc > 1) ? atoi(argv[1]) : 4;
    int seconds = (argc > 2) ? atoi(argv[2]) : 10;
    if (nCoins < 1) nCoins = 1;
    if (nCoins > 8) nCoins = 8;
    if (seconds < 1) seconds = 1;

    printf("simulate: 1s interval x %d coins x %ds\n", nCoins, seconds);

    int okCount = 0, failCount = 0, hit50011 = 0;
    ULONGLONG sumRtt = 0, maxRtt = 0;
    double sumRound = 0, maxRound = 0;
    int rounds = 0;

    ULONGLONG t0 = GetTickCount64();
    for (int r = 0; r < seconds; r++) {
        ULONGLONG rs = GetTickCount64();
        for (int i = 0; i < nCoins; i++) {
            std::wstring path = std::wstring(L"/api/v5/market/ticker?instId=") + kCoins[i];
            std::string body, err;
            ULONGLONG a = GetTickCount64();
            bool ok = http_get(L"www.okx.com", path, body, &err);
            ULONGLONG rtt = GetTickCount64() - a;
            sumRtt += rtt;
            if (rtt > maxRtt) maxRtt = rtt;
            if (ok) okCount++; else { failCount++; printf("  fail %ls: %s\n", kCoins[i], err.c_str()); }
            if (body.find("50011") != std::string::npos) { hit50011++; printf("  hit 50011\n"); }
        }
        double roundMs = (double)(GetTickCount64() - rs);
        sumRound += roundMs;
        if (roundMs > maxRound) maxRound = roundMs;
        rounds++;

        // 按 1 秒节奏对齐
        ULONGLONG target = t0 + (ULONGLONG)(r + 1) * 1000;
        ULONGLONG now = GetTickCount64();
        if (now < target) Sleep((DWORD)(target - now));
    }

    int total = okCount + failCount;
    printf("total_req=%d ok=%d fail=%d hit50011=%d\n", total, okCount, failCount, hit50011);
    if (total > 0) printf("avg_rtt=%.0fms max_rtt=%llums\n", (double)sumRtt / total, (unsigned long long)maxRtt);
    if (rounds > 0) printf("avg_round=%.0fms max_round=%.0fms (interval budget 1000ms)\n",
                           sumRound / rounds, maxRound);
    http_shutdown();
    return 0;
}
