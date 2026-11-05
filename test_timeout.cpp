// 超时验证（控制台）：所有行情接口统一 1 秒超时，慢接口必须准时被掐断
// 用法：test_timeout
#include "http.hpp"
#include <windows.h>
#include <cstdio>
#include <string>

static void probe(const wchar_t* host, const wchar_t* path, const char* tag) {
    std::string body, err;
    unsigned long long t0 = GetTickCount64();
    bool ok = http_get(host, path, body, &err);
    unsigned long long dt = GetTickCount64() - t0;
    printf("%-9s ok=%d %5llums bytes=%zu err=%s\n", tag, ok ? 1 : 0, dt, body.size(),
           ok ? "-" : err.c_str());
}

int main() {
    // 正常接口：应当几十毫秒返回
    probe(L"www.okx.com", L"/api/v5/market/ticker?instId=BTC-USDT", "ticker");
    // 50MB 下载：本机 1.5 秒才下完 10MB，这里必须被 1 秒截止时间掐断
    probe(L"speed.cloudflare.com", L"/__down?bytes=50000000", "slow-50m");
    // 超时后同域名再请求一次：验证连接被丢弃后能正常重建
    probe(L"www.okx.com", L"/api/v5/market/ticker?instId=ETH-USDT", "after");
    http_shutdown();
    return 0;
}
