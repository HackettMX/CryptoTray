#include "http.hpp"
#include "util.hpp"
#include <windows.h>
#include <winhttp.h>
#include <vector>

// ---- 持久连接 + 全局限频 ----
// 各家公开行情接口都按 IP 限频，其中 OKX 最紧（ticker 20 次/2 秒、candles 40 次/2 秒）。
// 自我限流取 16 次/2 秒（最紧上限的 80%）：既给突发重试留余量，
// 又能支撑「1 秒刷新」这种高频场景（1 秒 / N 币 = 2N 次每 2 秒，N≤8 可稳）。
// 多平台共用同一份配额，切到限额更松的平台也不会更慢（受本地限流约束）。
static const int   RL_MAX    = 16;
static const DWORD RL_WINDOW = 2000;

// 所有行情接口统一 1 秒超时：
//   连接 / 发送 / 接收各自 1 秒，另加一个从发起到读完的总截止时间，
//   防止服务端低速滴流时接收超时反复重置、把一次请求拖成几十秒。
//   行情数据本身就是「过期即无用」，宁可这一轮空着也不要卡住拉取线程。
static const DWORD HTTP_TIMEOUT_MS = 1000;

static INIT_ONCE     g_once = INIT_ONCE_STATIC_INIT;
static HINTERNET     g_hSession = NULL;
static HINTERNET     g_hConnect = NULL;
static std::wstring  g_connHost;      // 当前长连接指向的域名
static CRITICAL_SECTION g_cs;
static ULONGLONG     g_rlStamp[RL_MAX] = { 0 };
static int           g_rlIdx = 0;

static BOOL CALLBACK http_once_init(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    // 用系统静态代理配置：AUTOMATIC_PROXY 会每次请求做 WPAD 探测，白等数百毫秒
    g_hSession = WinHttpOpen(L"CryptoTray/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (g_hSession) {
        WinHttpSetTimeouts(g_hSession, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS,
                           HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS);
    }
    return TRUE;
}

// 已持锁调用：占用一个配额；额度不足时返回需要等待的毫秒数（不占用）
static int rl_take_locked() {
    ULONGLONG now = GetTickCount64();
    int filled = 0;
    ULONGLONG oldest = now;
    for (int i = 0; i < RL_MAX; i++) {
        if (g_rlStamp[i] != 0 && now - g_rlStamp[i] < RL_WINDOW) {
            filled++;
            if (g_rlStamp[i] < oldest) oldest = g_rlStamp[i];
        }
    }
    if (filled < RL_MAX) {
        g_rlStamp[g_rlIdx] = now;
        g_rlIdx = (g_rlIdx + 1) % RL_MAX;
        return 0;
    }
    int wait = (int)(RL_WINDOW - (now - oldest)) + 15;
    return wait > 0 ? wait : 1;
}

static void close_connect() {
    if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; }
    g_connHost.clear();
}

static bool ensure_connect(const std::wstring& host, std::string* err) {
    // 换了数据源就是换了域名，必须重建：否则请求会继续打到上一个平台的服务器
    if (g_hConnect && g_connHost == host) return true;
    close_connect();
    if (!g_hSession) { if (err) *err = "WinHttpOpen failed"; return false; }
    g_hConnect = WinHttpConnect(g_hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!g_hConnect) { if (err) *err = "WinHttpConnect failed"; return false; }
    g_connHost = host;
    return true;
}

// 已持锁调用：执行一次请求。timedOut 用于区分「超时」和「连接断了」，
// 前者不该重试（重试会把 1 秒变成 2 秒，行情场景没有意义）
static bool do_request(const std::wstring& host, const std::wstring& path,
                       std::string& out, std::string* err, bool* timedOut) {
    out.clear();
    if (timedOut) *timedOut = false;
    if (!ensure_connect(host, err)) return false;

    HINTERNET hR = WinHttpOpenRequest(g_hConnect, L"GET", path.c_str(), NULL,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { if (err) *err = "WinHttpOpenRequest failed"; return false; }

    bool ok = false;
    bool to = false;
    DWORD status = 0;
    const ULONGLONG deadline = GetTickCount64() + HTTP_TIMEOUT_MS;
    do {
        if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
            to = (GetLastError() == ERROR_WINHTTP_TIMEOUT); break;
        }
        if (!WinHttpReceiveResponse(hR, NULL)) {
            to = (GetLastError() == ERROR_WINHTTP_TIMEOUT); break;
        }

        DWORD len = sizeof(status);
        if (!WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
            status = 0;
        }
        // 429 属于明确的限频信号，交给上层退避
        if (status == 429) { if (err) *err = "http 429"; break; }

        DWORD avail = 0;
        for (;;) {
            // 总截止时间：分块到达也要在 1 秒内收完
            if (GetTickCount64() >= deadline) { to = true; break; }
            if (!WinHttpQueryDataAvailable(hR, &avail)) {
                if (GetLastError() == ERROR_WINHTTP_TIMEOUT) to = true;
                break;
            }
            if (avail == 0) break;
            std::string buf((size_t)avail, 0);
            DWORD read = 0;
            if (!WinHttpReadData(hR, &buf[0], avail, &read) || read == 0) {
                if (GetLastError() == ERROR_WINHTTP_TIMEOUT) to = true;
                break;
            }
            out.append(buf, 0, (size_t)read);
        }
        if (to) break;
        ok = (status == 0 || status == 200);
        if (!ok && err) *err = "http " + std::to_string((int)status);
    } while (false);

    if (to) {
        out.clear();                        // 半截 JSON 不留，免得上层拿着残包去解析
        if (err) *err = "timeout";
        if (timedOut) *timedOut = true;
    }
    WinHttpCloseHandle(hR);
    return ok;
}

// 请求忙标志：上一个请求还没结束就来的新请求直接丢弃，不排队等临界区。
// 行情数据过期即无用，宁可这一轮空着，也不要把新请求堵在旧请求后面。
static volatile LONG g_httpBusy = 0;
struct HttpBusyGuard {
    ~HttpBusyGuard() { InterlockedExchange(&g_httpBusy, 0); }
};

// 整轮预算：配额不够就整轮跳过，绝不让请求排队等配额。
// 时间窗是滚动的 2 秒，这里只检查「现在还剩几个名额」，不预占，
// 实际占用仍由每次请求的 rl_take_locked() 完成（只有一个拉取线程，不会插队）。
bool http_reserve(int n) {
    InitOnceExecuteOnce(&g_once, http_once_init, NULL, NULL);
    EnterCriticalSection(&g_cs);
    ULONGLONG now = GetTickCount64();
    int used = 0;
    for (int i = 0; i < RL_MAX; i++) {
        if (g_rlStamp[i] != 0 && now - g_rlStamp[i] < RL_WINDOW) used++;
    }
    bool ok = (n > 0 && used + n <= RL_MAX);
    LeaveCriticalSection(&g_cs);
    return ok;
}

bool http_get(const std::wstring& host, const std::wstring& path, std::string& out, std::string* err) {
    out.clear();
    if (InterlockedCompareExchange(&g_httpBusy, 1, 0) != 0) {
        if (err) *err = "busy";
        return false;
    }
    HttpBusyGuard guard;
    InitOnceExecuteOnce(&g_once, http_once_init, NULL, NULL);
    if (!g_hSession) { if (err) *err = "WinHttpOpen failed"; return false; }

    // 配额不足直接忽略本次请求，不 Sleep 排队（整轮预算已由 http_reserve 把过关）
    EnterCriticalSection(&g_cs);
    int wait = rl_take_locked();
    LeaveCriticalSection(&g_cs);
    if (wait != 0) { if (err) *err = "rate limited"; return false; }

    EnterCriticalSection(&g_cs);
    bool to = false;
    bool ok = do_request(host, path, out, err, &to);
    if (!ok && !to) {
        // 连接可能已被服务端关闭，重建连接重试一次。
        // 超时则不重试也不丢连接：行情接口的偶发超时多半是这一次慢了，
        // 把连接一起丢掉反而会让下一轮重新握手、又超时，越试越糟
        close_connect();
        ok = do_request(host, path, out, err, &to);
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void http_shutdown() {
    EnterCriticalSection(&g_cs);
    close_connect();
    if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; }
    LeaveCriticalSection(&g_cs);
}
