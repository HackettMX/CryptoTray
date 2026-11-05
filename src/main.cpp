#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <process.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include "util.hpp"
#include "json.hpp"
#include "market.hpp"
#include "http.hpp"
#include "config.hpp"

#pragma comment(lib, "dwmapi.lib")

// DWM 属性常量（可能 SDK 版本较旧未定义）
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMNCRP_DISABLED
#define DWMNCRP_DISABLED 1
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
#ifndef DWMSBT_MICA
#define DWMSBT_MICA 2
#endif

#define WM_TRAY    (WM_USER + 1)
#define WM_REFRESH (WM_USER + 2)

#define IDM_SETTINGS 1001
#define IDM_EXIT     1002
#define IDM_TOPMOST  1003
#define IDM_THROUGH  1004
#define IDM_DOCK     1005
#define IDM_SPONSOR  1006
#define IDM_STAR     1007

// 项目与赞助地址
#define APP_SPONSOR_URL L"https://sponsor.mxcos.com"
#define APP_REPO_URL    L"https://github.com/mxcos-source/CryptoTray"

#define IDC_BG       2001
#define IDC_WIN      2002
#define IDC_LIST     2003
#define IDC_COIN     2004
#define IDC_ADD      2005
#define IDC_DEL      2006
#define IDC_PIN_SW   2009
#define IDC_ALPHA    2010
#define IDC_ALPHA_TEXT 2011
#define IDC_CLOSE    2012
#define IDC_THROUGH  2013
#define IDC_TOPMOST  2014
#define IDC_DOCK_SW  2015
#define IDC_PF_BASE  2100     // 数据源按钮：2100 + 平台下标（共 8 个）

static const int POPUP_RADIUS = 12;
static const int SETTINGS_RADIUS = 14;

struct CoinView {
    std::string instId;
    double last = 0;
    double changePct = 0;
    std::vector<double> spark;
    bool ok = false;
    std::string err;
};

// ---- 全局状态 ----
static Config g_cfg;
static CRITICAL_SECTION g_cs;
static std::vector<CoinView> g_view;     // 受 g_cs 保护
static volatile LONG g_fetching = 0;
// 数据代数：切换平台 / 改币种 / 改周期都 +1。
// 拉取途中发现代数变了，就丢掉这一轮的半成品，立刻按新配置重拉，
// 否则切完数据源还会先把上一家的价格刷到屏幕上。
static volatile LONG g_fetchGen = 0;
// 配置变更的强制标记：这一轮不受限频预算约束，保证「切完立刻出数」
static volatile LONG g_fetchForce = 0;
static HANDLE g_hExit = NULL;
static HANDLE g_hFetchEvt = NULL;        // 拉取请求（自动复位）
static HANDLE g_hFetchThread = NULL;
static HWND g_hMsgWnd = NULL;            // 消息专用窗口（接收托盘回调，始终存在）
static HWND g_hMenuOwner = NULL;         // 右键菜单宿主（置顶的普通窗口，保证菜单压住任务栏）
static bool g_menuOpen = false;          // 右键菜单打开期间不碰 Z 序，免得把菜单压在下面

// 拖动落点提示：虚线占位框（仿汽水音乐播控的「松手固定」提示）
static HWND g_hHint = NULL;
static bool g_hintOn = false;            // 提示框当前是否显示
static bool g_hintDock = false;          // 落点是否为「停靠到任务栏」
static RECT g_hintRc = { 0, 0, 0, 0 };   // 落点矩形（屏幕坐标）
static HWND g_hWnd = NULL;               // 托盘弹出小窗（按需显示/隐藏）
static HWND g_hSettings = NULL;          // 设置窗口（NULL 表示未打开）
static HINSTANCE g_hInst = NULL;
static HICON g_hIcon = NULL;
static NOTIFYICONDATAW g_nid = { 0 };
static UINT g_taskbarCreated = 0;
static time_t g_lastUpdate = 0;

static volatile bool g_popupVisible = false;   // UI 线程写，拉取线程读
static bool g_hidePending = false;

// 拖动状态（弹窗与设置窗口共用，同一时刻仅一个在拖动）
static bool g_dragging = false;
static bool g_dragDock = false;      // 拖动开始时是否处于停靠形态
static bool g_dragMoved = false;     // 是否真的移动过（区分「单击」与「拖动」）
static POINT g_dragStart = { 0, 0 };
static RECT g_dragRect = { 0, 0, 0, 0 };

// 设置窗口 UI 状态
static bool g_setPin = false;
static bool g_setThrough = false;    // 鼠标穿透（实时生效）
static bool g_setTop = true;         // 置于最顶层（实时生效）
static bool g_setDock = false;       // 停靠任务栏（实时生效）
static int  g_setPlatform = 0;       // 数据源（market.hpp 里的下标）
static int  g_pfHover = -1;          // 设置页里鼠标悬停的数据源按钮
static int  g_alphaVal = 230;        // 自绘滑块当前值（50-255）
static int  g_previewAlpha = 230;    // 弹窗实时预览用的背景 alpha（保存后写入 g_cfg）
static int  g_sliderDrag = 0;        // 0=无，1=不透明度滑块，2=宽度滑块
static int  g_widthVal = 300;        // 宽度滑块当前值（260-460）
static HWND g_hoverBtn = NULL;
static bool g_trackMouse = false;

static DWORD g_lastPreviewAt = 0;    // 透明度预览节流

// 托盘单击/双击判定
static DWORD g_lastTrayClick = 0;
static const UINT_PTR TIMER_TRAYCLICK = 3;

static HFONT g_fontName = NULL, g_fontPrice = NULL, g_fontSmall = NULL, g_fontChange = NULL;

// 前置声明
static void show_context_menu();
static void open_settings();
static void settings_save_now();

// ---- 基础辅助 ----
static void Lock() { EnterCriticalSection(&g_cs); }
static void Unlock() { LeaveCriticalSection(&g_cs); }

static std::string format_price(double p) {
    if (p == 0) return "0.00";
    int dec = (p >= 1000) ? 2 : (p >= 1 ? 2 : (p >= 0.01 ? 4 : 6));
    char tmp[64];
    sprintf(tmp, "%.*f", dec, p);
    std::string s = tmp;
    size_t dot = s.find('.');
    std::string intpart = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot);
    std::string out;
    int cnt = 0;
    for (int i = (int)intpart.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), intpart[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out + frac;
}

// ---- 数据层：拉取全部关注币种（只在拉取线程中执行，绝不阻塞 UI） ----
// 明确不排队：同一时刻最多一轮在跑，忙的时候来的刷新请求直接丢弃。
// 唯一例外是配置变更（切平台 / 改币种 / 改周期），它用代数作废旧轮并接管，
// 属于「抢占」而非排队——不会累积，且新配置一定生效。
static void fetch_all() {
    if (InterlockedCompareExchange(&g_fetching, 1, 0) != 0) return;   // 忙：直接忽略
    const bool force = InterlockedExchange(&g_fetchForce, 0) != 0;

    for (;;) {
        const LONG gen = InterlockedCompareExchange(&g_fetchGen, 0, 0);

        std::vector<std::string> ids;
        std::string bar, plat;
        // 快照，避免与设置窗口并发修改冲突
        { Lock(); ids = g_cfg.coins; bar = g_cfg.klineBar; plat = g_cfg.platform; Unlock(); }
        const int pf = market_platform_index(plat);   // 未知值会回落到 OKX

        // 本轮至少要发 ids.size() 个 ticker 请求。配额不够就整轮跳过：
        // 不发一半、也不排队等配额，避免刷新周期被拖成忽快忽慢。
        // 配置变更（force）优先放行，保证切完数据源立刻出数
        if (!force && !http_reserve((int)ids.size())) break;

        std::vector<CoinView> nv;
        nv.reserve(ids.size());
        bool stale = false;
        for (const auto& id : ids) {
            // 换平台/换币种了：剩下的别再问旧接口，直接弃掉这一轮
            if (InterlockedCompareExchange(&g_fetchGen, 0, 0) != gen) { stale = true; break; }
            CoinQuote q; std::string e;
            market_fetch(pf, id, bar, q, &e);
            CoinView v;
            v.instId = id; v.last = q.last; v.changePct = q.changePct;
            v.spark = q.spark; v.ok = q.ok; v.err = q.err;
            nv.push_back(v);
        }

        if (!stale && InterlockedCompareExchange(&g_fetchGen, 0, 0) == gen) {
            Lock(); g_view.swap(nv); g_lastUpdate = time(NULL); Unlock();
            if (g_hMsgWnd && IsWindow(g_hMsgWnd)) PostMessageW(g_hMsgWnd, WM_REFRESH, 0, 0);
        }
        // 代数没变就收工；变了说明配置被改过，旧轮作废，立刻用新配置重跑
        if (InterlockedCompareExchange(&g_fetchGen, 0, 0) == gen) break;
    }

    InterlockedExchange(&g_fetching, 0);
}

// 数据源 / 币种 / 周期变化时调用：让正在跑的拉取作废，并立刻重新拉
static void fetch_invalidate() {
    InterlockedIncrement(&g_fetchGen);
    InterlockedExchange(&g_fetchForce, 1);   // 用户主动改配置：这一轮不受限频预算约束
    if (g_hFetchEvt) SetEvent(g_hFetchEvt);
}

// 刷新周期：与设置窗口的写入共用 g_cs，避免读到改到一半的间隔值
static DWORD refresh_interval_ms() {
    DWORD ms;
    { Lock(); ms = (DWORD)((g_popupVisible ? g_cfg.winInterval : g_cfg.bgInterval) * 1000); Unlock(); }
    return ms < 200 ? 200 : ms;
}

// ---- 拉取线程：唯一执行网络 IO 的线程，刷新节奏也由它自己掌握 ----
// 之前靠 UI 线程的 WM_TIMER 每秒 SetEvent：消息队列被重绘/拖动挤一下就会迟到，
// 加上自动复位事件在拉取期间会留下一次信号，出现「一轮刚完立刻又拉、再等一秒」
// 的忽快忽慢。现在改成线程自带 Wait 超时，节奏不受 UI 影响，且忙时一律忽略。
static unsigned __stdcall fetch_thread(void*) {
    HANDLE hs[2] = { g_hExit, g_hFetchEvt };
    DWORD interval = refresh_interval_ms();
    DWORD due = GetTickCount();      // 首次立刻拉；之后按固定节拍对齐
    for (;;) {
        // 分段等待（每段最多 100ms）：显示/隐藏切换会改前后台间隔，
        // 这里能立刻重排节拍，不用等旧间隔走完，也不依赖 UI 线程发消息
        for (;;) {
            DWORD now = GetTickCount();
            DWORD wait = (due > now) ? (due - now) : 0;
            DWORD slice = wait < 100 ? wait : 100;
            DWORD r = WaitForMultipleObjects(2, hs, FALSE, slice);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) return 0;   // 退出
            if (r == WAIT_OBJECT_0 + 1) break;                       // 配置变更：立刻刷新
            DWORD ni = refresh_interval_ms();
            if (ni != interval) {
                // 显示/隐藏切换（1 秒 ⇄ 后台间隔）：立刻按新配置拉一轮，
                // 而不是重新开始等一个完整周期——否则关掉设置窗口要白等 1 秒才出数
                interval = ni;
                due = GetTickCount();
                continue;
            }
            if (wait <= 100) break;                                  // 周期到点
        }
        // 事件是自动复位：拉取期间积累的触发一并清掉，忙时触发绝不补跑
        ResetEvent(g_hFetchEvt);

        const DWORD roundStart = GetTickCount();
        interval = refresh_interval_ms();
        fetch_all();
        const DWORD spent = GetTickCount() - roundStart;
        // 固定节拍：本轮耗时从等待里扣掉，刷新点落在间隔的整数倍上；
        // 一轮跑超时则不追赶落后节拍，从现在起重新起拍（忽略这次迟到的周期）
        due = (spent < interval) ? (roundStart + interval) : (GetTickCount() + interval);
    }
}

// ---- 运行时生成托盘图标（橙色圆 + $） ----
static HICON CreateTrayIcon() {
    const int S = 32;
    HDC hdc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(hdc);
    ReleaseDC(NULL, hdc);

    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(bi); bi.biWidth = S; bi.biHeight = -S;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(mem, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HBITMAP hOld = (HBITMAP)SelectObject(mem, hColor);
    memset(bits, 0, S * S * 4);

    HBRUSH br = CreateSolidBrush(RGB(0xF7, 0x93, 0x1A));
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ o1 = SelectObject(mem, br); HGDIOBJ o2 = SelectObject(mem, pen);
    Ellipse(mem, 2, 2, S - 2, S - 2);
    SelectObject(mem, o1); SelectObject(mem, o2);
    DeleteObject(br); DeleteObject(pen);
    for (int i = 0; i < S * S; i++)
        if (bits[i].rgbRed || bits[i].rgbGreen || bits[i].rgbBlue) bits[i].rgbReserved = 255;

    SetBkMode(mem, TRANSPARENT);
    LOGFONTW lf = { 0 };
    lf.lfHeight = 22; lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"Microsoft YaHei");
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ o3 = SelectObject(mem, f);
    SetTextColor(mem, RGB(255, 255, 255));
    RECT r = { 2, 2, S - 2, S - 2 };
    DrawTextW(mem, L"$", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, o3); DeleteObject(f);
    for (int i = 0; i < S * S; i++)
        if (bits[i].rgbRed || bits[i].rgbGreen || bits[i].rgbBlue) bits[i].rgbReserved = 255;

    SelectObject(mem, hOld);

    HBITMAP hMask = CreateBitmap(S, S, 1, 1, NULL);
    HDC m2 = CreateCompatibleDC(NULL);
    HBITMAP mOld = (HBITMAP)SelectObject(m2, hMask);
    PatBlt(m2, 0, 0, S, S, BLACKNESS);
    SelectObject(m2, mOld); DeleteDC(m2);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE; ii.hbmMask = hMask; ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hColor); DeleteObject(hMask); DeleteDC(mem);
    return hIcon;
}

// ---- DWM 磨砂/圆角/阴影辅助 ----
static void enable_dwm_effects(HWND h) {
    // 启用 Mica/Acrylic 效果 (Win11) 或 BlurBehind (Win10)
    BOOL enabled = FALSE;
    DwmIsCompositionEnabled(&enabled);
    if (!enabled) return;

    // 内嵌任务栏：背景全透明，任何系统装饰（投影 / 圆角 / 边框线）都会露出来
    if (g_cfg.dockTaskbar) {
        int noRound = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
        int ncrpOff = DWMNCRP_DISABLED;          // 关掉非客户区渲染 = 没有 DWM 投影
        DwmSetWindowAttribute(h, DWMWA_NCRENDERING_POLICY, &ncrpOff, sizeof(ncrpOff));
        COLORREF noBorder = (COLORREF)DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));

        // 关掉系统材质背景。浮窗模式会设 Mica，切回横条时必须显式关掉：
        // 否则窗口底下会留一层 Mica 的浅灰底（≈#F2F2F2），在透明背景上透出来，
        // 看着就像「拖回任务栏后还是一块桌面卡片」。
        int backdropNone = 1;                    // DWMSBT_NONE
        DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdropNone, sizeof(backdropNone));
        DWM_BLURBEHIND bb = { 0 };
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = FALSE;
        bb.hRgnBlur = NULL;
        DwmEnableBlurBehindWindow(h, &bb);
        return;
    }

    // 浮窗：保证 Mica 背景重新打开（横条模式会被显式关掉）
    // 1) 圆角 (Win11+ DWMWA_WINDOW_CORNER_PREFERENCE = 33)
    int pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

    // 2) 不要系统材质背景。窗口自己画了不透明卡片底板，Mica 只会坏事：
    //    它画在分层表面**之下**，不受像素 alpha 影响，所以「整体不透明度」会失效
    //    （看起来永远是一块实心白卡片）
    int backdropNone2 = 1;                       // DWMSBT_NONE
    DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdropNone2, sizeof(backdropNone2));
    DWM_BLURBEHIND bbf = { 0 };
    bbf.dwFlags = DWM_BB_ENABLE;
    bbf.fEnable = FALSE;
    bbf.hRgnBlur = NULL;
    DwmEnableBlurBehindWindow(h, &bbf);

    // 3) 阴影 (DWMWA_NCRENDERING_POLICY)
    int ncrp = 2; // DWMNCRP_ENABLED
    DwmSetWindowAttribute(h, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    // 4) 标题栏颜色协调 (亮色模式)
    COLORREF clr = RGB(0xF0, 0xF0, 0xF0);
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &clr, sizeof(clr));
    DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR, &clr, sizeof(clr));
    // 边框颜色
    COLORREF borderClr = RGB(0xDD, 0xDD, 0xDD);
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &borderClr, sizeof(borderClr));
}

static void draw_rounded_rect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

// ---- 绘制迷你走势线 ----
static void draw_spark(HDC dc, const std::vector<double>& vals, int x, int y, int w, int h, COLORREF col) {
    if (vals.size() < 2) return;
    double mn = vals[0], mx = vals[0];
    for (double v : vals) { if (v < mn) mn = v; if (v > mx) mx = v; }
    double range = mx - mn; if (range == 0) range = 1;
    POINT* pts = new POINT[vals.size()];
    for (size_t i = 0; i < vals.size(); i++) {
        int px = x + (int)((double)i / (vals.size() - 1) * w);
        int py = y + h - (int)((vals[i] - mn) / range * h);
        pts[i].x = px; pts[i].y = py;
    }
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    HPEN op = (HPEN)SelectObject(dc, pen);
    Polyline(dc, pts, (int)vals.size());
    SelectObject(dc, op); DeleteObject(pen);
    delete[] pts;
}

static void create_tray() {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMsgWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_hIcon;
    wcsncpy(g_nid.szTip, L"CryptoTray 行情", 127);
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = 4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

// 无边框自绘窗口：圆角 + 阴影（不启用 Mica，避免覆盖自绘背景）
static void apply_modern_frame(HWND h, int radius) {
    BOOL enabled = FALSE;
    DwmIsCompositionEnabled(&enabled);
    if (!enabled) return;
    int pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    int ncrp = 2; // DWMNCRP_ENABLED（投影）
    DwmSetWindowAttribute(h, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));
}

// 为窗口设置圆角裁剪区域（与自绘圆角一致）
static void set_rounded_region(HWND h, int radius) {
    RECT rc; GetClientRect(h, &rc);
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SetWindowRgn(h, rgn, TRUE);
}

// ---- 弹窗几何 ----
static const int POPUP_W_MIN = 260;
static const int POPUP_W_MAX = 460;
static const int POPUP_ROW_H = 56;

// 任务栏内嵌迷你条
static const int DOCK_CELL_W = 100;    // 每个币种格宽（紧凑）
static const int DOCK_PAD    = 6;      // 横条左右内边距
static const int DOCK_INSET  = 4;      // 格内文字左右留白

static bool taskbar_free_span(LONG& lo, LONG& hi);   // 定义见下

static int dock_w() {
    int n = (int)g_cfg.coins.size();
    if (n < 1) n = 1;
    int w = 2 * DOCK_PAD + n * DOCK_CELL_W;

    // 币种多到盛不下时，按任务栏空档宽度压缩，别压到托盘和应用按钮
    LONG lo = 0, hi = 0;
    if (taskbar_free_span(lo, hi)) {
        int avail = (int)(hi - lo) - 24;
        if (avail > 120 && w > avail) w = avail;
    }
    return w;
}

// 任务栏矩形与所属显示器（每次现取，Explorer 重启后句柄会失效）
static bool taskbar_rect(RECT& tr, RECT& mon) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb || !GetWindowRect(tb, &tr)) return false;
    HMONITOR hm = MonitorFromWindow(tb, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return false;
    mon = mi.rcMonitor;
    return true;
}

// 横条高度：比任务栏略矮，嵌进去才像任务栏自带的控件；随 DPI / 任务栏高度自适应
static int dock_h() {
    RECT tr, mon;
    if (taskbar_rect(tr, mon)) {
        int th = tr.bottom - tr.top;
        if (th > 4) {
            int h = th - 10;
            if (h < 26) h = 26;
            if (h > 44) h = 44;
            return h;
        }
    }
    return 36;
}

// 弹窗宽度：内嵌任务栏时用横条宽度，否则跟随配置（越窄越紧凑）
static int popup_w() {
    if (g_cfg.dockTaskbar) return dock_w();
    int w = g_cfg.popupWidth;
    if (w < POPUP_W_MIN) w = POPUP_W_MIN;
    if (w > POPUP_W_MAX) w = POPUP_W_MAX;
    return w;
}
static int popup_height(int rows) {
    if (rows < 1) rows = 1;
    return 12 + rows * (POPUP_ROW_H + 4) + 4;
}

// 弹窗高度：统一入口（横条 / 竖排列表）
static int popup_h() {
    if (g_cfg.dockTaskbar) return dock_h();
    return popup_height((int)g_cfg.coins.size());
}

// 圆角：横条小一点，更像任务栏里的浮岛控件
static int popup_radius() { return g_cfg.dockTaskbar ? 24 : POPUP_RADIUS; }  // RoundRect 收的是直径，视觉半径减半

// 任务栏可见（自动隐藏收起时整条会缩到屏幕边缘之外）
static bool taskbar_revealed() {
    RECT tr, mon;
    if (!taskbar_rect(tr, mon)) return false;
    if ((tr.bottom - tr.top) <= 4 || (tr.right - tr.left) <= 4) return false;
    if (tr.bottom <= mon.top + 2 || tr.top >= mon.bottom - 2) return false;
    if (tr.right <= mon.left + 2 || tr.left >= mon.right - 2) return false;
    return true;
}

// 量出任务栏上的空白带：左边是开始按钮 / 任务按钮区的右边界，右边是托盘区的左边界
static bool taskbar_free_span(LONG& lo, LONG& hi) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb) return false;
    RECT tr; GetWindowRect(tb, &tr);
    lo = tr.left;
    hi = tr.right;

    HWND tray = FindWindowExW(tb, NULL, L"TrayNotifyWnd", NULL);
    if (tray) {
        RECT r; GetWindowRect(tray, &r);
        if (r.left > lo) hi = r.left;
    }

    const wchar_t* probes[] = { L"Start", L"ReBarWindow32", L"MSTaskSwWClass" };
    for (const wchar_t* cls : probes) {
        HWND c = FindWindowExW(tb, NULL, cls, NULL);
        if (!c) continue;
        RECT r; GetWindowRect(c, &r);
        if ((r.right - r.left) > 4 && r.right > lo) lo = r.right;   // 已被占用的右边界
    }
    return hi > lo + 40;
}

// 内嵌任务栏：横条落在任务栏内部（覆盖在任务栏之上的同层窗口），竖直居中
static bool dock_place(int W, int H, int& x, int& y) {
    RECT tr, mon;
    if (!taskbar_rect(tr, mon)) return false;

    const int trayH = tr.bottom - tr.top;
    const int trayW = tr.right - tr.left;
    if (trayH <= 4 || trayW <= 4) return false;

    if (trayH > trayW) {
        // 竖排任务栏塞不下横向条，退回贴在任务栏外侧
        x = (tr.left <= mon.left + 2) ? (tr.right + 4) : (tr.left - W - 4);
        y = (tr.top + tr.bottom - H) / 2;
        return true;
    }

    // 水平任务栏：垂直方向居中，水平方向找任务栏上没被图标占用的空档
    y = tr.top + (trayH - H) / 2;

    LONG lo = tr.left, hi = tr.right;
    taskbar_free_span(lo, hi);
    int spanLo = (int)(lo - tr.left);
    int spanHi = (int)(hi - tr.left);

    int off;
    if (g_cfg.dockOffsetX >= 0) {
        off = g_cfg.dockOffsetX;                      // 用户拖过，沿用记录位置
    } else if (spanHi - spanLo >= W) {
        off = spanLo + (spanHi - spanLo - W) / 2;     // 默认摆在空档正中
    } else {
        off = spanLo;                                 // 空档太窄，贴着任务按钮区右侧
    }
    if (off + W > spanHi) off = spanHi - W;           // 别压到托盘
    if (off < spanLo - 40) off = spanLo - 40;
    if (off < 4) off = 4;

    x = tr.left + off;
    return true;
}

// 任务栏迷你条：每个币种一格，上行为代号 + 涨跌幅，下行为价格
// 这里不画底板：横条的背景由 dock_present() 逐像素合成成全透明，只留文字与分隔线
// mono = true 时统一用白色绘制，作为「覆盖率遮罩」来决定每个像素的 alpha
static void paint_dock(HDC dc, const RECT& rc, bool mono) {
    auto C = [mono](COLORREF c) { return mono ? RGB(255, 255, 255) : c; };

    SetBkMode(dc, TRANSPARENT);

    std::vector<std::string> coins;
    Lock(); coins = g_cfg.coins; Unlock();
    std::vector<CoinView> rows;
    Lock(); rows = g_view; Unlock();
    std::map<std::string, CoinView> m;
    for (auto& v : rows) m[v.instId] = v;

    if (coins.empty()) {
        SelectObject(dc, g_fontSmall);
        SetTextColor(dc, C(RGB(0x99, 0x99, 0x99)));
        RECT mr = rc;
        DrawTextW(dc, L"暂无关注币种，右键托盘 → 设置 添加", -1, &mr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const int n = (int)coins.size();
    int cellW = ((rc.right - rc.left) - 2 * DOCK_PAD) / n;

    for (int i = 0; i < n; i++) {
        const std::string& id = coins[i];
        CoinView v;
        auto it = m.find(id);
        if (it != m.end()) v = it->second;
        else { v.instId = id; v.ok = false; v.err = "加载中"; }

        const int cx = DOCK_PAD + i * cellW;
        if (i > 0) {                       // 格间细分隔线
            HPEN pen = CreatePen(PS_SOLID, 1, C(RGB(0xE6, 0xE9, 0xEE)));
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx, rc.top + 8, NULL);
            LineTo(dc, cx, rc.bottom - 8);
            SelectObject(dc, op);
            DeleteObject(pen);
        }

        bool up = v.changePct >= 0;
        COLORREF changeColor = C(up ? RGB(0xD0, 0x3A, 0x2A) : RGB(0x1E, 0x9E, 0x5A));

        // 代号：去掉计价后缀，任务栏条上更省地方
        std::string name = id;
        size_t dash = name.find('-');
        if (dash != std::string::npos && dash > 0) name = name.substr(0, dash);

        // 第一行：代号（左）+ 涨跌幅（右）
        int midX = cx + cellW * 45 / 100;
        {
            RECT nr = { cx + DOCK_INSET, rc.top + 4, midX, rc.top + 22 };
            SelectObject(dc, g_fontChange);
            SetTextColor(dc, C(RGB(0x6B, 0x72, 0x7C)));
            DrawTextW(dc, to_w(name).c_str(), -1, &nr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (v.ok) {
            wchar_t ch[32];
            swprintf(ch, _countof(ch), L"%s%.2f%%", up ? L"+" : L"", v.changePct);
            RECT cr = { midX, rc.top + 4, cx + cellW - DOCK_INSET, rc.top + 22 };
            SelectObject(dc, g_fontChange);
            SetTextColor(dc, changeColor);
            DrawTextW(dc, ch, -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        // 第二行：价格
        RECT pr = { cx + DOCK_INSET, rc.top + 20, cx + cellW - DOCK_INSET, rc.bottom - 2 };
        SelectObject(dc, g_fontPrice);
        SetTextColor(dc, C(RGB(0x11, 0x14, 0x18)));
        std::wstring priceStr = (v.ok && v.last > 0) ? to_w(format_price(v.last)) : L"—";
        DrawTextW(dc, priceStr.c_str(), -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ---- 弹窗绘制 ----
static void paint_popup(HDC dc, const RECT& rc) {
    if (g_cfg.dockTaskbar) { paint_dock(dc, rc, false); return; }

    const int RADIUS = POPUP_RADIUS;

    // 底板
    draw_rounded_rect(dc, rc, RADIUS, RGB(0xFA, 0xFA, 0xFA), RGB(0xDC, 0xDF, 0xE4));

    // 裁剪圆角区域，防止内容溢出
    HRGN clipRgn = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, RADIUS, RADIUS);
    SelectClipRgn(dc, clipRgn);
    DeleteObject(clipRgn);

    SetBkMode(dc, TRANSPARENT);

    std::vector<std::string> coins;
    Lock(); coins = g_cfg.coins; Unlock();
    std::vector<CoinView> rows;
    Lock(); rows = g_view; Unlock();
    std::map<std::string, CoinView> m;
    for (auto& v : rows) m[v.instId] = v;

    int y = rc.top + 12;
    const int rowH = POPUP_ROW_H;
    const int padding = 12;

    // 自适应列宽：左侧名称列按比例占位，其余留给价格与走势线，窄窗口也不会挤在一起
    const int textL = padding + 14;                // 文字左边界
    const int textR = rc.right - padding - 14;     // 文字右边界
    const int textW = textR - textL;
    int nameW = textW * 52 / 100;
    if (nameW < 96) nameW = 96;                    // 名称列下限，避免 "-USDT" 被截断
    if (nameW > textW - 96) nameW = textW - 96;    // 价格列至少留 96
    const int colSplit = textL + nameW;            // 左右分栏的位置
    const int colGap = 8;

    if (coins.empty()) {
        RECT rr = { padding, y, rc.right - padding, y + rowH };
        draw_rounded_rect(dc, rr, 10, RGB(0xFF, 0xFF, 0xFF), RGB(0xEE, 0xF0, 0xF3));
        SelectObject(dc, g_fontSmall);
        SetTextColor(dc, RGB(0x99, 0x99, 0x99));
        RECT mr = rr;
        DrawTextW(dc, L"暂无关注币种，右键托盘 → 设置 添加", -1, &mr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    for (const auto& id : coins) {
        CoinView v;
        auto it = m.find(id);
        if (it != m.end()) v = it->second; else { v.instId = id; v.ok = false; v.err = "加载中"; }

        RECT rr = { padding, y, rc.right - padding, y + rowH };

        // 行卡片：统一白底 + 极浅描边
        draw_rounded_rect(dc, rr, 10, RGB(0xFF, 0xFF, 0xFF), RGB(0xEE, 0xF0, 0xF3));

        bool up = v.changePct >= 0;

        // 币种名（左上）
        RECT nr = { textL, rr.top + 7, colSplit - colGap, rr.top + 30 };
        SelectObject(dc, g_fontName);
        SetTextColor(dc, RGB(0x1A, 0x1D, 0x21));
        DrawTextW(dc, to_w(v.instId).c_str(), -1, &nr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 价格（右上，主视觉）
        RECT pr = { colSplit, rr.top + 7, textR, rr.top + 32 };
        SelectObject(dc, g_fontPrice);
        SetTextColor(dc, RGB(0x11, 0x14, 0x18));
        std::wstring priceStr = (v.ok && v.last > 0) ? to_w(format_price(v.last)) : L"—";
        DrawTextW(dc, priceStr.c_str(), -1, &pr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // 涨跌幅（左下）
        SelectObject(dc, g_fontChange);
        COLORREF changeColor = up ? RGB(0xD0, 0x3A, 0x2A) : RGB(0x1E, 0x9E, 0x5A); // 涨红跌绿
        SetTextColor(dc, changeColor);
        wchar_t ch[32];
        swprintf(ch, _countof(ch), L"%s%.2f%%", up ? L"+" : L"", v.changePct);
        RECT cr = { textL, rr.top + 30, colSplit - colGap, rr.top + 50 };
        DrawTextW(dc, ch, -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 迷你走势线（右下，占满剩余宽度，不与文字重叠）
        if (!v.spark.empty()) {
            int sx = colSplit + 2;
            int sw = textR - sx;
            if (sw > 20) draw_spark(dc, v.spark, sx, rr.top + 32, sw, 16, changeColor);
        }

        // 错误信息
        if (!v.ok && !v.err.empty()) {
            SelectObject(dc, g_fontSmall);
            SetTextColor(dc, RGB(0xE0, 0x6C, 0x5C));
            RECT er = { colSplit, rr.top + 30, textR, rr.top + 50 };
            DrawTextW(dc, to_w(v.err).c_str(), -1, &er,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        y += rowH + 4;
    }
}

// 内容全部由 UpdateLayeredWindow 直接合成上屏，WM_PAINT 里不必再画一遍
static void draw_popup(HWND h) {
    PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);
}

// ---- 任务栏横条：逐像素透明合成 ----
// 整窗 alpha（LWA_ALPHA）会把文字一起变淡，做不到「背景全透明、文字实心」；
// 这里改用 UpdateLayeredWindow + 32 位 DIB：
//   pass A 用白色把同样的内容再画一遍，得到每个像素的覆盖率 → alpha
//   pass B 在黑底上用真彩画一遍，结果天然就是预乘 alpha 的颜色，直接可用
static void dock_present(HWND h) {
    RECT wr; GetWindowRect(h, &wr);
    const int W = wr.right - wr.left, H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;              // 自上而下，与 GDI 坐标一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bitsA = nullptr;
    void* bitsB = nullptr;
    HDC mdc = CreateCompatibleDC(NULL);
    HBITMAP bmA = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bitsA, NULL, 0);
    HBITMAP bmB = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bitsB, NULL, 0);
    if (!bmA || !bmB || !bitsA || !bitsB) {
        if (bmA) DeleteObject(bmA);
        if (bmB) DeleteObject(bmB);
        DeleteDC(mdc);
        return;
    }

    RECT rc = { 0, 0, W, H };
    HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));   // 黑底＝全透明，也是预乘的零点
    HGDIOBJ old = SelectObject(mdc, bmA);
    FillRect(mdc, &rc, bk);
    paint_dock(mdc, rc, true);                    // 遮罩：只认覆盖率
    SelectObject(mdc, bmB);                       // 之后一直保持 bmB 选中，提交时要用
    FillRect(mdc, &rc, bk);
    paint_dock(mdc, rc, false);                   // 真彩
    DeleteObject(bk);

    // 亮度当覆盖率写进 alpha。背景留 1 而不是 0：ULW 的命中测试按 alpha==0 判穿透，
    // 留 1 才能让整条都能点中、拖得动；四角圆角外给 0，让形状靠 alpha 而不是窗口 region
    const BYTE* qa = (const BYTE*)bitsA;
    BYTE* qb = (BYTE*)bitsB;
    const int npx = W * H;
    const int rad = popup_radius() / 2;
    for (int i = 0; i < npx; i++) {
        const BYTE* a = qa + i * 4;
        int cov = (a[0] * 29 + a[1] * 150 + a[2] * 77) >> 8;
        if (cov > 255) cov = 255;
        bool outside = false;                       // 落在圆角之外
        if (rad > 0) {
            const int x = i % W, y = i / W;
            int dx = 0, dy = 0;
            if (x < rad)           dx = rad - x;
            else if (x >= W - rad) dx = x - (W - rad - 1);
            if (y < rad)           dy = rad - y;
            else if (y >= H - rad) dy = y - (H - rad - 1);
            if (dx > 0 && dy > 0 && (dx * dx + dy * dy) >= rad * rad) outside = true;
        }
        qb[i * 4 + 3] = (BYTE)(outside ? 0 : (cov < 1 ? 1 : cov));
    }

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 };
    POINT dst = { wr.left, wr.top };
    SIZE size = { W, H };
    HDC sdc = GetDC(NULL);
    UpdateLayeredWindow(h, sdc, &dst, &size, mdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, sdc);

    SelectObject(mdc, old);       // 取消选中后才能删除位图
    DeleteObject(bmA);
    DeleteObject(bmB);
    DeleteDC(mdc);
}

// 整体透明度：整个窗口（背景 + 文字 + 走势线）统一按 alpha 合成
static int clamp_alpha(int a) { return a < 0 ? 0 : (a > 255 ? 255 : a); }

// 浮窗的整窗不透明度也走 UpdateLayeredWindow。
// 之前用 SetLayeredWindowAttributes(LWA_ALPHA)，但它和 ULW 互斥：LWA 模式下再调 ULW
// 直接失败（ERROR_INVALID_PARAMETER=87），横条就停在上一次的旧画面上——
// 用户看到的就是「拖到任务栏后还是桌面模式」。改成两边都用 ULW，从此没有模式切换。
static void alpha_present(HWND h) {
    RECT wr; GetWindowRect(h, &wr);
    const int W = wr.right - wr.left, H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC mdc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(mdc); return; }

    HGDIOBJ old = SelectObject(mdc, bm);
    RECT rc = { 0, 0, W, H };
    paint_popup(mdc, rc);
    // GDI 是延迟批处理的：不 flush 就直接读 DIB 内存会读到全 0，
    // 于是 alpha 和颜色全算错，窗口看着是一块底色或者干脆透明
    GdiFlush();

    const int a = clamp_alpha(g_previewAlpha);
    const int rad = popup_radius() / 2;          // RoundRect 收直径，这里要视觉半径
    BYTE* q = (BYTE*)bits;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            BYTE* p = q + (y * W + x) * 4;
            int cov = 255;
            if (rad > 0) {                        // 圆角外做软裁，四角才不会露出直角白边
                int dx = 0, dy = 0;
                if (x < rad)           dx = rad - x;
                else if (x >= W - rad) dx = x - (W - rad - 1);
                if (y < rad)           dy = rad - y;
                else if (y >= H - rad) dy = y - (H - rad - 1);
                if (dx > 0 && dy > 0 && (dx * dx + dy * dy) >= rad * rad) cov = 0;
            }
            const int al = cov ? a : 0;
            p[0] = (BYTE)(p[0] * al / 255);       // 预乘 alpha
            p[1] = (BYTE)(p[1] * al / 255);
            p[2] = (BYTE)(p[2] * al / 255);
            p[3] = (BYTE)al;
        }
    }

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 };
    POINT dst = { wr.left, wr.top };
    SIZE size = { W, H };
    HDC sdc = GetDC(NULL);
    UpdateLayeredWindow(h, sdc, &dst, &size, mdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, sdc);

    SelectObject(mdc, old);
    DeleteObject(bm);
    DeleteDC(mdc);
}

static void popup_render(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (g_cfg.dockTaskbar) dock_present(h);   // 横条逐像素透明，背景真正透出任务栏
    else                   alpha_present(h);  // 浮窗整窗不透明度，同样走 ULW
}

static void popup_refresh(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h)) return;
    popup_render(h);
}

// 窗口层级与鼠标穿透：按配置实时改写扩展样式
static void apply_popup_layers() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;

    LONG_PTR ex = GetWindowLongPtrW(g_hWnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED;                              // 透明与穿透都依赖分层窗口
    // 停靠任务栏时无条件置顶：否则会被其它置顶窗口（托盘菜单、通知、全屏容器）盖住
    if (g_cfg.alwaysOnTop || g_cfg.dockTaskbar) ex |= WS_EX_TOPMOST;
    else                                        ex &= ~WS_EX_TOPMOST;
    if (g_cfg.mouseThrough) ex |= WS_EX_TRANSPARENT;  // 命中测试直接交给下层窗口
    else                    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hWnd, GWL_EXSTYLE, ex);

    // WS_EX_TOPMOST 只改样式不生效，必须配合 SetWindowPos 调整 Z 序
    SetWindowPos(g_hWnd, (g_cfg.alwaysOnTop || g_cfg.dockTaskbar) ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 弹窗该用的 Z 序
static HWND popup_zorder() {
    // 嵌在任务栏里的横条必须压在任务栏之上，否则会被任务栏盖住
    if (g_cfg.dockTaskbar) return HWND_TOPMOST;
    return g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
}

// 把横条认任务栏当「宿主窗口」：Windows 保证 owned window 永远画在 owner 之上，
// 任务栏再怎么把自己提到最前也盖不住它。退出停靠时解除。
static void dock_own() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;

    if (!g_cfg.dockTaskbar) {
        if ((HWND)GetWindowLongPtrW(g_hWnd, GWLP_HWNDPARENT) != NULL)
            SetWindowLongPtrW(g_hWnd, GWLP_HWNDPARENT, 0);
        return;
    }

    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb) return;                       // 拿不到任务栏就先保持现状，别把宿主清掉
    if ((HWND)GetWindowLongPtrW(g_hWnd, GWLP_HWNDPARENT) != tb)
        SetWindowLongPtrW(g_hWnd, GWLP_HWNDPARENT, (LONG_PTR)tb);

    // 挂宿主会顺手抹掉 TOPMOST 标志：宿主负责压住任务栏，TOPMOST 负责压住其它置顶窗口
    // （托盘右键菜单、通知、全屏远程桌面容器等，丢了它横条就会被盖掉）
    LONG_PTR ex = GetWindowLongPtrW(g_hWnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_TOPMOST)) SetWindowLongPtrW(g_hWnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
}

// 横条是否已经被任务栏压住（Z 序里 taskbar 排在横条前面 = 盖在上面）
static bool dock_covered_by_taskbar(HWND tb) {
    for (HWND h = GetWindow(g_hWnd, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
        if (h == tb) return true;
    return false;
}

// 重新压到任务栏上方：**只在真的被任务栏盖住时**才动。
// 以前是每 300ms 无条件 SetWindowPos(HWND_TOPMOST)，每次都把横条提到置顶组最前面，
// 于是它永远压在别人刚弹出的置顶菜单（托盘右键菜单、通知）之上——看着像「菜单被挡住」。
// 现在只要还在任务栏之上就不碰 Z 序，让出位置给临时弹出物。
static void dock_raise() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;

    LONG_PTR ex = GetWindowLongPtrW(g_hWnd, GWL_EXSTYLE);
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    // 不在置顶组里（丢过 TOPMOST）或确实被任务栏压住，才需要抬
    if (tb && (ex & WS_EX_TOPMOST) && !dock_covered_by_taskbar(tb)) return;

    SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 创建主弹窗。横条以任务栏为宿主，Explorer 重建任务栏时窗口会被连带销毁，所以支持重建
static void create_popup_window() {
    DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_LAYERED;
    if (g_cfg.alwaysOnTop)  exStyle |= WS_EX_TOPMOST;
    if (g_cfg.mouseThrough) exStyle |= WS_EX_TRANSPARENT;
    g_hWnd = CreateWindowExW(exStyle, L"CryptoTrayPopup", L"", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, popup_w(), popup_h(),
        NULL, NULL, g_hInst, NULL);
    apply_popup_layers();
    enable_dwm_effects(g_hWnd);
}

// 计算弹窗位置：有记录用记录位置，否则默认右下角，并夹在显示器工作区内
static void popup_place(int W, int H, int& x, int& y) {
    if (g_cfg.dockTaskbar && dock_place(W, H, x, y)) return;

    POINT p; GetCursorPos(&p);
    HMONITOR hMon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    RECT work = mi.rcWork;   // 工作区，不含任务栏

    if (g_cfg.windowX >= 0 && g_cfg.windowY >= 0) {
        x = g_cfg.windowX; y = g_cfg.windowY;   // 优先使用记录的位置
    } else {
        x = work.right - W - 16;                // 默认屏幕右下角
        y = work.bottom - H - 16;
    }
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + W > work.right) x = work.right - W;
    if (y + H > work.bottom) y = work.bottom - H;
}

// 停靠模式下跟随任务栏：任务栏换边、自动隐藏、分辨率变化后把横条贴回去
static int g_trayStrikes = 0;   // 连续探测不到任务栏的次数

static void dock_follow() {
    if (!g_cfg.dockTaskbar) return;
    if (!g_hWnd || !IsWindow(g_hWnd) || !g_popupVisible) return;
    if (g_dragging) return;          // 拖动中别把窗口拽回任务栏，落点由拖动逻辑决定

    dock_own();   // 宿主关系可能因 Explorer 重启丢失，这里补上

    // 任务栏自动隐藏收起时，横条跟着收起来。
    // 但 Explorer 重启瞬间会短暂找不到任务栏，别急着隐藏，连续几次确认才算真的收起
    if (!taskbar_revealed()) {
        if (++g_trayStrikes >= 3) ShowWindow(g_hWnd, SW_HIDE);
        return;
    }
    g_trayStrikes = 0;
    if (!IsWindowVisible(g_hWnd)) ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);

    int W = popup_w(), H = popup_h();
    int x = 0, y = 0;
    if (!dock_place(W, H, x, y)) return;

    RECT r; GetWindowRect(g_hWnd, &r);
    bool same = (r.left == x && r.top == y &&
                 (r.right - r.left) == W && (r.bottom - r.top) == H);

    if (!same) {
        SetWindowPos(g_hWnd, popup_zorder(), x, y, W, H, SWP_NOACTIVATE);
    }
    if (!g_menuOpen) dock_raise();   // 位置没变也要确认没被任务栏压住（按需，不主动抢最前）
}

// ---- 拖动落点提示窗：浅蓝圆角虚线框 + 一句说明 ----
static void hint_paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(h, &ps); return; }

    // 铺满整个客户区：分层窗口里没画到的像素会按黑底参与整体 alpha，四角会发黑
    HBRUSH br = CreateSolidBrush(RGB(0xE7, 0xEE, 0xFD));
    FillRect(dc, &rc, br);
    DeleteObject(br);

    // 虚线圆角边框（1px cosmetic pen 才支持 PS_DOT）
    HPEN pen = CreatePen(PS_DOT, 1, RGB(0x76, 0x97, 0xDD));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 1, 1, rc.right - 1, rc.bottom - 1, 14, 14);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontChange ? g_fontChange : g_fontSmall);
    SetTextColor(dc, RGB(0x46, 0x57, 0x77));
    DrawTextW(dc, g_hintDock ? L"松手固定窗口" : L"将窗口移到此处", -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    EndPaint(h, &ps);
}

static LRESULT CALLBACK HintProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: hint_paint(h); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;    // 提示框不吃鼠标
    }
    return DefWindowProcW(h, m, w, l);
}

// 显示/更新落点框；locked=true 表示已经对准（松手就固定到任务栏）
static int g_hintW = 0, g_hintH = 0;      // 上次设定的窗口圆角尺寸
static void hint_show(bool locked, int x, int y, int w, int h) {
    if (w < 90) w = 90;
    if (h < 26) h = 26;
    g_hintDock = locked;
    g_hintRc = { x, y, x + w, y + h };
    if (!g_hHint || !IsWindow(g_hHint)) return;
    g_hintOn = true;

    // 尺寸变了才重设圆角区域：让四角真的被裁掉，不然会露出半透明黑角
    if (w != g_hintW || h != g_hintH) {
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 16, 16);
        if (!SetWindowRgn(g_hHint, rgn, TRUE)) DeleteObject(rgn);
        g_hintW = w; g_hintH = h;
    }
    SetWindowPos(g_hHint, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hHint, NULL, FALSE);
    UpdateWindow(g_hHint);
}

static void hint_hide() {
    g_hintOn = false;
    if (g_hHint && IsWindow(g_hHint)) ShowWindow(g_hHint, SW_HIDE);
}

static void show_popup() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;
    if (g_popupVisible) return;

    int W = popup_w(), H = popup_h();
    int x = 0, y = 0;
    popup_place(W, H, x, y);

    SetWindowPos(g_hWnd, popup_zorder(), x, y, W, H, SWP_NOACTIVATE);
    dock_own();              // 停靠时认任务栏当宿主窗口，彻底避免被它盖住
    if (g_cfg.pinToDesktop) {
        ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);   // 常驻模式不抢焦点
    } else {
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
    }
    popup_render(g_hWnd);    // 上屏后立即出首帧

    SetTimer(g_hWnd, 1, (UINT)(g_cfg.winInterval * 1000), NULL);
    // 不再挂 300ms 的 Z 序保活定时器：它每次 SetWindowPos(HWND_TOPMOST) 都会把横条
    // 顶到置顶组最前面，反而压住别人刚弹出的托盘菜单。
    // 现在只在 dock_follow()（随刷新周期）里按需检查，被任务栏压住才抬。
    g_popupVisible = true;
}

static void hide_popup_force() {
    if (!g_popupVisible) return;
    KillTimer(g_hWnd, 1);
    KillTimer(g_hWnd, 3);
    KillTimer(g_hWnd, 5);                          // 拖动轮询
    if (g_dragging) { g_dragging = false; hint_hide(); }
    ShowWindow(g_hWnd, SW_HIDE);
    SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_popupVisible = false;
    g_hidePending = false;
}

static void hide_popup() {
    // 固定到桌面 / 停靠任务栏时不自动隐藏（托盘双击可强制隐藏）
    if (g_cfg.pinToDesktop || g_cfg.dockTaskbar) return;
    hide_popup_force();
}

// 币种数 / 形态变化后重算尺寸与位置：竖排回记录位置，横条贴任务栏
static void relayout_popup() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;
    int W = popup_w(), H = popup_h();

    int x = 0, y = 0;
    popup_place(W, H, x, y);          // 内部已处理「停靠任务栏」与越界夹取

    SetWindowPos(g_hWnd, popup_zorder(), x, y, W, H, SWP_NOACTIVATE);
    dock_own();       // 进入/退出停靠时同步宿主关系
    if (!g_cfg.dockTaskbar) { g_cfg.windowX = x; g_cfg.windowY = y; }  // 停靠位置不覆盖记录
    popup_refresh(g_hWnd);
}

// 透明度预览：临时把弹窗显示出来（不抢焦点、不额外拉数据）
static void show_popup_preview() {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;
    if (g_popupVisible) { popup_refresh(g_hWnd); return; }

    int W = popup_w(), H = popup_h();
    int x = 0, y = 0;
    popup_place(W, H, x, y);

    SetWindowPos(g_hWnd, popup_zorder(), x, y, W, H, SWP_NOACTIVATE);
    ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);
    popup_render(g_hWnd);
    g_popupVisible = true;
}

// 设置窗口关闭后按「是否常驻」决定弹窗去留：常驻→显示，非常驻→隐藏
static void settle_popup_after_settings() {
    if (g_cfg.pinToDesktop) {
        if (!g_popupVisible) show_popup();
        else popup_refresh(g_hWnd);
    } else {
        hide_popup_force();
    }
}

// 托盘左键：单击与双击（常驻模式用双击切换显示 / 隐藏）
static void tray_click(bool dbl) {
    if (g_cfg.pinToDesktop) {
        if (dbl) {
            if (g_popupVisible) hide_popup_force();
            else show_popup();
        } else if (!g_popupVisible) {
            show_popup();          // 常驻时单击 = 确保显示出来
        }
    } else {
        if (g_popupVisible) hide_popup();
        else show_popup();
    }
}

static void show_context_menu() {
    POINT p; GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TOPMOST, L"置于最顶层");
    AppendMenuW(m, MF_STRING, IDM_THROUGH, L"鼠标穿透");
    AppendMenuW(m, MF_STRING, IDM_DOCK, L"嵌入任务栏");
    CheckMenuItem(m, IDM_TOPMOST, MF_BYCOMMAND | (g_cfg.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_THROUGH, MF_BYCOMMAND | (g_cfg.mouseThrough ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_DOCK, MF_BYCOMMAND | (g_cfg.dockTaskbar ? MF_CHECKED : MF_UNCHECKED));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SPONSOR, L"❤ 赞助支持");
    AppendMenuW(m, MF_STRING, IDM_STAR, L"为项目点个 Star");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    // 菜单位置：TPM_BOTTOMALIGN 让菜单底边落在 p.y。
    // 光标在任务栏里时，把 p.y 抬到该显示器工作区的底边，
    // 菜单就整块浮在任务栏上方，不会压进任务栏区域被它盖住。
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        if (p.y > mi.rcWork.bottom) p.y = mi.rcWork.bottom;
        if (p.x < mi.rcWork.left)   p.x = mi.rcWork.left + 4;
    }

    // 菜单必须挂在「普通且置顶」的窗口上：菜单窗口会继承 owner 的 topmost，
    // 否则会被同样置顶的任务栏盖住（message-only 窗口不参与 Z 序，靠它不行）。
    HWND owner = (g_hMenuOwner && IsWindow(g_hMenuOwner)) ? g_hMenuOwner : g_hMsgWnd;
    SetForegroundWindow(owner);
    g_menuOpen = true;                    // 菜单期间让保活定时器别动 Z 序
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   p.x, p.y, 0, owner, NULL);
    g_menuOpen = false;
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void open_settings();

// ---- 消息专用窗口过程（接收托盘回调）----
static LRESULT CALLBACK MsgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TRAY: {
        // lParam 低 16 位是鼠标消息
        UINT mouseMsg = (UINT)(l & 0xFFFF);
        if (mouseMsg == WM_LBUTTONUP) {
            g_hidePending = false; KillTimer(g_hWnd, 2);
            DWORD now = GetTickCount();
            if (g_lastTrayClick != 0 && now - g_lastTrayClick < GetDoubleClickTime()) {
                // 第二击：撤销待处理的单击，按双击处理
                KillTimer(h, TIMER_TRAYCLICK);
                g_lastTrayClick = 0;
                tray_click(true);
            } else {
                g_lastTrayClick = now;
                SetTimer(h, TIMER_TRAYCLICK, GetDoubleClickTime(), NULL);
            }
        } else if (mouseMsg == WM_RBUTTONUP) {
            show_context_menu();
        }
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_TRAYCLICK) {
            KillTimer(h, TIMER_TRAYCLICK);
            g_lastTrayClick = 0;
            tray_click(false);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDM_SETTINGS) open_settings();
        else if (LOWORD(w) == IDM_EXIT) PostQuitMessage(0);
        else if (LOWORD(w) == IDM_TOPMOST) {
            g_cfg.alwaysOnTop = !g_cfg.alwaysOnTop;
            g_setTop = g_cfg.alwaysOnTop;
            apply_popup_layers();
            config_save(g_cfg, nullptr);
            if (g_hSettings && IsWindow(g_hSettings))
                InvalidateRect(GetDlgItem(g_hSettings, IDC_TOPMOST), NULL, FALSE);
        }
        else if (LOWORD(w) == IDM_THROUGH) {
            g_cfg.mouseThrough = !g_cfg.mouseThrough;
            g_setThrough = g_cfg.mouseThrough;
            apply_popup_layers();
            config_save(g_cfg, nullptr);
            if (g_hSettings && IsWindow(g_hSettings))
                InvalidateRect(GetDlgItem(g_hSettings, IDC_THROUGH), NULL, FALSE);
        }
        else if (LOWORD(w) == IDM_DOCK) {
            g_cfg.dockTaskbar = !g_cfg.dockTaskbar;
            g_setDock = g_cfg.dockTaskbar;
            enable_dwm_effects(g_hWnd);       // 横条不要系统投影/边框，竖排恢复
            if (g_popupVisible) { relayout_popup(); dock_follow(); }
            else if (g_cfg.dockTaskbar) show_popup();
            popup_render(g_hWnd);
            config_save(g_cfg, nullptr);
            if (g_hSettings && IsWindow(g_hSettings))
                InvalidateRect(GetDlgItem(g_hSettings, IDC_DOCK_SW), NULL, FALSE);
        }
        else if (LOWORD(w) == IDM_SPONSOR) {
            ShellExecuteW(NULL, L"open", APP_SPONSOR_URL, NULL, NULL, SW_SHOWNORMAL);
        }
        else if (LOWORD(w) == IDM_STAR) {
            ShellExecuteW(NULL, L"open", APP_REPO_URL, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    case WM_REFRESH:
        popup_refresh(g_hWnd);   // 停靠横条走分层合成，靠 InvalidateRect 刷新不出来
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- 拖动：跟随与落定 ----
// 拖动期间同时由 WM_MOUSEMOVE 和 16ms 定时器轮询驱动：分层窗口 + 认任务栏当 owner 的
// 组合下 SetCapture 偶尔会失效，只靠鼠标消息就会出现「拖到一半窗口不动了」，
// 轮询则始终跟手；松手也由轮询检测按键状态，捕获丢了也能正确结束。
static HWND g_dragHwnd = NULL;
static DWORD g_dragTick = 0;        // 拖动起点，给按键检测留一段宽限
static void drag_update(HWND h);
static void drag_finish(HWND h);

static void drag_update(HWND h) {
    if (!g_dragging) return;
    POINT pt; GetCursorPos(&pt);
    // 位移不足 3px 当作单击：否则点一下横条就会把它变成浮窗
    if (!g_dragMoved) {
        int ddx = pt.x - g_dragStart.x, ddy = pt.y - g_dragStart.y;
        if (ddx < 0) ddx = -ddx;
        if (ddy < 0) ddy = -ddy;
        if (ddx < 3 && ddy < 3) return;
        g_dragMoved = true;
    }
    int W = g_dragRect.right - g_dragRect.left;
    int H = g_dragRect.bottom - g_dragRect.top;
    int nx = g_dragRect.left + (pt.x - g_dragStart.x);
    int ny = g_dragRect.top  + (pt.y - g_dragStart.y);

    // 拖动中窗口跟着光标走：只夹在整块显示器内（允许压到任务栏上）。
    // 之前夹在工作区，窗口底边永远到不了任务栏，两态就只能靠光标猜
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hm, &mi);
    RECT mr = mi.rcMonitor;
    if (nx < mr.left) nx = mr.left;
    if (ny < mr.top) ny = mr.top;
    if (nx + W > mr.right) nx = mr.right - W;
    if (ny + H > mr.bottom) ny = mr.bottom - H;
    SetWindowPos(h, HWND_TOPMOST, nx, ny, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);

    // 落点判定以「窗口边缘」到任务栏/虚线框的间距为准，不再看光标：
    // 光标按在窗口的哪一处并不固定，只看光标会和眼里看到的窗口位置对不上。
    //   gap > 96px        → 不提示
    //   8px < gap <= 96px → 「将窗口移到此处」（靠近了，还没贴上）
    //   窗口贴住虚线框    → 「松手固定窗口」
    bool nearBar = false, locked = false;
    int hx = nx, hy = ny, hw = W, hh = H;
    RECT tr, mon;
    if (taskbar_rect(tr, mon) && (tr.right - tr.left) > (tr.bottom - tr.top)) {
        const int trayH = tr.bottom - tr.top;
        const bool barOnTop = (tr.top <= mon.top + 2);          // 任务栏吸顶的情况
        const int gap = barOnTop ? (ny - tr.bottom)             // 窗口顶边到任务栏底边
                                 : (tr.top - (ny + H));         // 窗口底边到任务栏顶边
        if (gap <= 96) {
            nearBar = true;
            hw = dock_w();
            hh = dock_h();
            LONG lo = 0, hi = 0;
            taskbar_free_span(lo, hi);
            int off = pt.x - tr.left - hw / 2;          // 落点框跟光标水平居中对齐
            if (off + hw > (int)(hi - tr.left)) off = (int)(hi - tr.left) - hw;
            if (off < (int)(lo - tr.left)) off = (int)(lo - tr.left);
            if (off < 4) off = 4;
            hx = tr.left + off;
            hy = tr.top + (trayH - hh) / 2;

            // 窗口边缘贴住虚线框（差 8px 内）→ 松手即固定。
            // 用虚线框而不是任务栏当基准，切换点才和眼睛看到的一致
            const int gapBox = barOnTop ? (ny - (hy + hh)) : (hy - (ny + H));
            locked = (gapBox <= 8) ||
                     (barOnTop ? (pt.y <= tr.bottom) : (pt.y >= tr.top));
        }
    }
    if (nearBar) hint_show(locked, hx, hy, hw, hh);
    else         hint_hide();
}

static void drag_finish(HWND h) {
    if (!g_dragging) return;
    g_dragging = false;
    KillTimer(h, 5);
    ReleaseCapture();

    if (!g_dragMoved) {                     // 只是点了一下，形态不变
        hint_hide();
        KillTimer(h, 2); g_hidePending = false;
        return;
    }

    const bool toDock = (g_hintOn && g_hintDock);
    const RECT hr = g_hintRc;
    hint_hide();

    if (toDock) {
        // 松开时落点框是亮的：停靠到任务栏，水平位置记下来
        g_cfg.dockTaskbar = true;
        g_setDock = true;
        RECT tr, mon;
        if (taskbar_rect(tr, mon)) g_cfg.dockOffsetX = hr.left - tr.left;
    } else {
        // 没对准任务栏：按当前位置落回浮窗（也覆盖「从横条拖出去」的情况）
        RECT r; GetWindowRect(h, &r);
        g_cfg.dockTaskbar = false;
        g_setDock = false;
        g_cfg.windowX = r.left;
        g_cfg.windowY = r.top;
    }
    apply_popup_layers();       // 横条无条件置顶 / 浮窗按「置于最顶层」开关
    enable_dwm_effects(g_hWnd); // 横条去系统投影，竖排恢复
    relayout_popup();
    dock_follow();
    config_save(g_cfg, nullptr);
    if (g_hSettings && IsWindow(g_hSettings))
        InvalidateRect(GetDlgItem(g_hSettings, IDC_DOCK_SW), NULL, FALSE);
    KillTimer(h, 2); g_hidePending = false;        // 拖动结束，取消待隐藏
}

// ---- 弹出小窗过程 ----
static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: return 0;
    case WM_ERASEBKGND: return 1;   // 全程双缓冲自绘，不需要系统擦背景
    case WM_REFRESH:
        popup_refresh(h); return 0;
    case WM_TIMER:
        // 定时器只负责 Z 序保活与界面重绘；数据刷新节奏由拉取线程自己按周期掌握，
        // 这里不再 SetEvent，否则 1 秒周期会和拉取线程的自有周期双重触发
        if (w == 1) { dock_follow(); popup_refresh(h); }
        else if (w == 2) {
            KillTimer(h, 2);
            if (g_hidePending) {
                HWND fg = GetForegroundWindow();
                if (fg != g_hWnd && fg != g_hSettings) hide_popup();
                else g_hidePending = false;
            }
        }
        else if (w == 5) {          // 拖动轮询：跟手 + 检测松手（不依赖鼠标捕获）
            if (!g_dragging || g_dragHwnd != h) { KillTimer(h, 5); return 0; }
            // 前 700ms 不做按键判定：刚按下时按键状态可能还没同步，免得一开拖就结束
            if (GetTickCount() - g_dragTick > 700 &&
                !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { drag_finish(h); return 0; }
            drag_update(h);
        }
        return 0;
    case WM_ACTIVATE:
        // 停靠任务栏时横条是常驻控件，不参与「失焦即隐藏」
        if (LOWORD(w) == WA_INACTIVE && g_popupVisible && !g_cfg.dockTaskbar) {
            g_hidePending = true; SetTimer(h, 2, 200, NULL); // 200ms 延迟，规避“点托盘本身”误触
        }
        return 0;
    case WM_LBUTTONDOWN:
        // 统一起见：不管当前是横条还是浮窗，都先自由拖动，松手时按落点决定形态
        g_dragging = true;
        g_dragHwnd = h;
        g_dragTick = GetTickCount();
        g_dragMoved = false;
        g_dragDock = g_cfg.dockTaskbar;
        GetCursorPos(&g_dragStart);
        GetWindowRect(h, &g_dragRect);
        SetCapture(h);
        hint_hide();
        SetTimer(h, 5, 16, NULL);   // 轮询兜底：捕获丢了也不会「拖到一半不动」
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging && g_dragHwnd == h) drag_update(h);
        return 0;
    case WM_LBUTTONUP:
        drag_finish(h);
        return 0;
    case WM_CAPTURECHANGED:
        // 捕获被抢走（分层窗口 + 任务栏 owner 下偶发）时不再中断拖动：
        // 位置与松手都由 16ms 轮询接管，等真松手再落定
        return 0;
    case WM_PAINT: draw_popup(h); return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- 设置窗口 ----
// 从列表框读回币种，立即应用到运行中的配置并重算弹窗尺寸
static void settings_sync_coins(HWND h) {
    HWND lb = GetDlgItem(h, IDC_LIST);
    if (!lb) return;
    std::vector<std::string> coins;
    int cnt = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        int l2 = (int)SendMessageW(lb, LB_GETTEXTLEN, i, 0);
        std::wstring t(l2 + 1, 0); SendMessageW(lb, LB_GETTEXT, i, (LPARAM)&t[0]);
        std::string s = to_a(t);
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) coins.push_back(s.substr(a, b - a + 1));
    }
    Lock(); g_cfg.coins = coins; Unlock();
    relayout_popup();                       // 行数变了，窗口尺寸/位置跟着重算
    if (!coins.empty()) settings_save_now(); // 添加/删除即时落盘
    fetch_invalidate();                      // 币种变了：新币立刻取数，旧轮作废
}

static void on_add(HWND h) {
    HWND e = GetDlgItem(h, IDC_COIN); HWND lb = GetDlgItem(h, IDC_LIST);
    int len = GetWindowTextLengthW(e); if (len <= 0) return;
    std::wstring buf(len + 1, 0); GetWindowTextW(e, &buf[0], len + 1);
    std::string s = to_a(buf);
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return;
    s = s.substr(a, b - a + 1);
    for (char& c : s) if (c >= 'a' && c <= 'z') c -= 32;
    // 统一成 BASE-QUOTE：各平台对分隔符的写法不同（BTCUSDT / BTC_USDT / BTC-USDT），
    // 内部只存一种，请求时再按平台转换
    s = market_norm_symbol(s);
    int cnt = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    bool dup = false;
    for (int i = 0; i < cnt; i++) {
        int l2 = (int)SendMessageW(lb, LB_GETTEXTLEN, i, 0);
        std::wstring t(l2 + 1, 0); SendMessageW(lb, LB_GETTEXT, i, (LPARAM)&t[0]);
        if (to_a(t) == s) { dup = true; break; }
    }
    if (!dup) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)to_w(s).c_str());
    SetWindowTextW(e, L"");
    settings_sync_coins(h);
    InvalidateRect(h, NULL, FALSE);   // 币种数变了，限频提示同步刷新
}

static void on_del(HWND h) {
    HWND lb = GetDlgItem(h, IDC_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) SendMessageW(lb, LB_DELETESTRING, sel, 0);
    settings_sync_coins(h);
    InvalidateRect(h, NULL, FALSE);
}

// ================= 设置窗口布局常量 =================
static const int SET_W = 440, SET_H = 1000;  // 新增「数据源」卡片后高度 +124
static const int CARD_X = 16, CARD_W = 408;
static const int FIELD_X = 276, FIELD_W = 96;          // 数字输入框
static const int SL_X1 = 152, SL_X2 = 330, SL_CY = 910; // 自绘滑块：不透明度
static const int SL2_CY = 938;                          // 自绘滑块：弹窗宽度

// 数据源按钮（两行四列）
static const int PF_Y1 = 492, PF_Y2 = 528, PF_H = 30;
static const int PF_X0 = 32, PF_W = 88, PF_GAP = 8;

static RECT pf_rect(int i) {
    int col = i % 4, row = i / 4;
    int x = PF_X0 + col * (PF_W + PF_GAP);
    int y = (row == 0) ? PF_Y1 : PF_Y2;
    RECT r = { x, y, x + PF_W, y + PF_H };
    return r;
}
static const int SW_X = 356, SW_W = 48, SW_H = 26;      // 自绘开关

// K 线周期胶囊（8 个，两行各 4 个）
struct BarOpt { const wchar_t* label; const char* value; };
static const BarOpt kBars[] = {
    { L"5 分钟",  "5m" }, { L"15 分钟", "15m" }, { L"1 小时", "1H" }, { L"4 小时", "4H" },
    { L"1 天",    "1D" }, { L"1 周",    "1W" },  { L"1 月",   "1M" }, { L"3 月",   "3M" },
};
static const int kBarsCount = (int)(sizeof(kBars) / sizeof(kBars[0]));
static const int BAR_X0 = 32, BAR_W = 86, BAR_GAP = 8;
static const int BAR_Y1 = 622, BAR_Y2 = 660, BAR_H = 30;

static int g_barIndex = 0;

static int bar_index_of(const std::string& v) {
    for (int i = 0; i < kBarsCount; i++) if (v == kBars[i].value) return i;
    return 0;
}

// 胶囊矩形
static RECT bar_rect(int i) {
    int col = i % 4, row = i / 4;
    int x = BAR_X0 + col * (BAR_W + BAR_GAP);
    int y = (row == 0) ? BAR_Y1 : BAR_Y2;
    RECT r = { x, y, x + BAR_W, y + BAR_H };
    return r;
}

static void ui_text(HDC dc, HFONT f, COLORREF c, const wchar_t* s, const RECT& rc, UINT flags) {
    HGDIOBJ of = f ? SelectObject(dc, f) : NULL;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, (RECT*)&rc, flags | DT_NOPREFIX);
    if (of) SelectObject(dc, of);
}

// 输入框底槽（白底圆角，EDIT 内缩其中）
static void draw_field(HDC dc, RECT rc, bool focus) {
    COLORREF border = focus ? RGB(0xF7, 0x93, 0x1A) : RGB(0xE3, 0xE7, 0xEE);
    draw_rounded_rect(dc, rc, 8, RGB(0xFF, 0xFF, 0xFF), border);
}

static void draw_card(HDC dc, int y, int h) {
    RECT rc = { CARD_X, y, CARD_X + CARD_W, y + h };
    draw_rounded_rect(dc, rc, 12, RGB(0xFF, 0xFF, 0xFF), RGB(0xEB, 0xEE, 0xF3));
}

// 自绘滑块（轨道 + 进度 + 圆形滑钮）
static void draw_slider(HDC dc, int cy, int kx, int x1, int x2, COLORREF accent) {
    RECT track = { x1, cy - 3, x2, cy + 3 };
    draw_rounded_rect(dc, track, 3, RGB(0xE5, 0xE7, 0xEB), RGB(0xE5, 0xE7, 0xEB));
    if (kx > x1) {
        RECT fill = { x1, cy - 3, kx, cy + 3 };
        draw_rounded_rect(dc, fill, 3, accent, accent);
    }
    RECT knob = { kx - 8, cy - 8, kx + 8, cy + 8 };
    HBRUSH kb = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    HPEN kp = CreatePen(PS_SOLID, 1, RGB(0xD5, 0xD9, 0xE0));
    HGDIOBJ ob = SelectObject(dc, kb), okp = SelectObject(dc, kp);
    Ellipse(dc, knob.left, knob.top, knob.right, knob.bottom);
    SelectObject(dc, ob); SelectObject(dc, okp);
    DeleteObject(kb); DeleteObject(kp);
}

// 每 2 秒的请求数（用于界面上展示限频余量）
// 每轮拉取 = N 次 ticker（candles 有 60 秒缓存，长期分摊可忽略）
static int est_req_per_2s_for(int coins, int sec) {
    if (coins < 1) coins = 1;
    if (sec < 1) sec = 1;
    if (sec > 3600) sec = 3600;
    int v = (coins * 2000 + sec * 1000 - 1) / (sec * 1000);
    return v < 1 ? 1 : v;
}

static int est_req_per_2s() {
    int n = (int)g_cfg.coins.size();
    return est_req_per_2s_for(n, g_cfg.winInterval);
}

// ================= 设置窗口绘制 =================
static void draw_settings(HWND h, HDC dc, const RECT& rc) {
    HFONT fT = (HFONT)GetPropW(h, L"fontTitle");
    HFONT fS = (HFONT)GetPropW(h, L"fontSection");
    HFONT fN = (HFONT)GetPropW(h, L"fontNormal");
    HFONT fM = (HFONT)GetPropW(h, L"fontSmall");

    const COLORREF BG      = RGB(0xF6, 0xF7, 0xF9);
    const COLORREF TXT     = RGB(0x1F, 0x23, 0x28);
    const COLORREF SUB     = RGB(0x8A, 0x90, 0x99);
    const COLORREF ACCENT  = RGB(0xF7, 0x93, 0x1A);

    // 背景
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    // 顶部标题区
    RECT top = { 0, 0, rc.right, 64 };
    HBRUSH wb = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(dc, &top, wb);
    DeleteObject(wb);

    // 品牌小方块 + 标题
    RECT mark = { 20, 20, 32, 32 };
    draw_rounded_rect(dc, mark, 4, ACCENT, ACCENT);
    RECT tr = { 40, 14, rc.right - 60, 38 };
    ui_text(dc, fT, TXT, L"CryptoTray 设置", tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT sr = { 41, 36, rc.right - 60, 54 };
    ui_text(dc, fM, SUB, L"行情刷新与外观", sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 顶部分隔线
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xEC, 0xEE, 0xF2));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, 0, 64, NULL); LineTo(dc, rc.right, 64);
    SelectObject(dc, op); DeleteObject(pen);

    // ---- 卡片 1：刷新频率 ----
    draw_card(dc, 76, 142);
    ui_text(dc, fS, SUB, L"刷新频率", RECT{ 32, 86, 300, 106 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ui_text(dc, fN, TXT, L"后台刷新间隔", RECT{ 32, 112, 260, 144 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_field(dc, RECT{ FIELD_X, 112, FIELD_X + FIELD_W, 144 }, GetFocus() == GetDlgItem(h, IDC_BG));
    ui_text(dc, fM, SUB, L"秒", RECT{ FIELD_X + FIELD_W + 6, 112, 420, 144 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ui_text(dc, fN, TXT, L"窗口显示时刷新", RECT{ 32, 150, 260, 182 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_field(dc, RECT{ FIELD_X, 150, FIELD_X + FIELD_W, 182 }, GetFocus() == GetDlgItem(h, IDC_WIN));
    ui_text(dc, fM, SUB, L"秒", RECT{ FIELD_X + FIELD_W + 6, 150, 420, 182 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 限频提示（按输入框里的即时值计算，改数字立即反馈）
    int curCoins = (int)SendMessageW(GetDlgItem(h, IDC_LIST), LB_GETCOUNT, 0, 0);
    int curWin = GetDlgItemInt(h, IDC_WIN, NULL, FALSE);
    int per2s = est_req_per_2s_for(curCoins, curWin);
    wchar_t hint[160];
    if (per2s > 16) {
        swprintf(hint, _countof(hint), L"约 %d 次 / 2 秒，已超安全阈值（16），可能被限频", per2s);
    } else if (per2s > 12) {
        swprintf(hint, _countof(hint), L"约 %d 次 / 2 秒，接近安全阈值（16），建议放慢", per2s);
    } else {
        swprintf(hint, _countof(hint), L"当前约 %d 次请求 / 2 秒 · 本地限流 16 次 / 2 秒", per2s);
    }
    COLORREF hintColor = (per2s > 16) ? RGB(0xD9, 0x3B, 0x2B)
                       : (per2s > 12) ? RGB(0xC8, 0x7A, 0x00)
                                      : RGB(0x9A, 0xA0, 0xA6);
    ui_text(dc, fM, hintColor, hint, RECT{ 32, 188, 408, 210 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 卡片 2：关注币种 ----
    draw_card(dc, 230, 218);
    ui_text(dc, fS, SUB, L"关注币种", RECT{ 32, 240, 200, 260 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fM, SUB, L"格式如 BTC-USDT", RECT{ 240, 240, 408, 260 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 列表底槽
    {
        RECT box = { 32, 268, 408, 390 };
        draw_rounded_rect(dc, box, 10, RGB(0xFF, 0xFF, 0xFF), RGB(0xE3, 0xE7, 0xEE));
    }
    // 输入框底槽
    draw_field(dc, RECT{ 32, 398, 250, 432 }, GetFocus() == GetDlgItem(h, IDC_COIN));

    // ---- 卡片 3：数据源 ----
    draw_card(dc, 460, 112);
    ui_text(dc, fS, SUB, L"数据源", RECT{ 32, 466, 200, 486 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fM, SUB, L"公开行情接口 · 无需 API Key",
        RECT{ 180, 466, 408, 486 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    // 按钮由 8 个自绘 BUTTON 承载，这里只画底槽外的空白（选中态在 WM_DRAWITEM 里画）
    {
        int npf = market_platform_count();
        for (int i = npf; i < 8; i++) {   // 平台数不足 8 时，剩余格子画成浅灰空位
            RECT r = pf_rect(i);
            draw_rounded_rect(dc, r, 8, RGB(0xF6, 0xF7, 0xF9), RGB(0xEC, 0xEF, 0xF4));
        }
    }

    // ---- 卡片 4：走势图周期 ----
    draw_card(dc, 584, 138);
    ui_text(dc, fS, SUB, L"走势图周期", RECT{ 32, 594, 200, 614 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fM, SUB, L"每行 30 根", RECT{ 240, 594, 408, 614 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < kBarsCount; i++) {
        RECT br = bar_rect(i);
        bool sel = (i == g_barIndex);
        COLORREF fill   = sel ? ACCENT : RGB(0xFF, 0xFF, 0xFF);
        COLORREF border = sel ? ACCENT : RGB(0xE3, 0xE7, 0xEE);
        draw_rounded_rect(dc, br, 8, fill, border);
        ui_text(dc, fM, sel ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x3A, 0x40, 0x48),
                kBars[i].label, br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    ui_text(dc, fM, RGB(0xA0, 0xA7, 0xB0), L"周期越长，走势线覆盖的时间跨度越大",
        RECT{ 32, 694, 408, 714 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 卡片 5：外观 ----
    draw_card(dc, 734, 228);
    ui_text(dc, fS, SUB, L"外观", RECT{ 32, 740, 200, 762 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ui_text(dc, fN, TXT, L"固定到桌面（常驻显示）", RECT{ 32, 765, 348, 795 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fN, TXT, L"鼠标穿透（点击穿透到下层）", RECT{ 32, 797, 348, 827 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fN, TXT, L"置于最顶层（始终在最前）", RECT{ 32, 829, 348, 859 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui_text(dc, fN, TXT, L"嵌入任务栏（迷你横条）", RECT{ 32, 861, 348, 891 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ui_text(dc, fN, TXT, L"整体不透明度", RECT{ 32, 897, 148, 923 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 不透明度滑块
    {
        double t = (g_alphaVal - 50) / 205.0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        draw_slider(dc, SL_CY, SL_X1 + (int)(t * (SL_X2 - SL_X1)), SL_X1, SL_X2, ACCENT);

        wchar_t pct[16];
        swprintf(pct, _countof(pct), L"%d%%", (g_alphaVal * 100) / 255);
        ui_text(dc, fN, SUB, pct, RECT{ 340, 897, 404, 923 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // 弹窗宽度滑块（拖动即时改窗口宽度，保存后写盘）
    // 嵌入任务栏时宽度由币种数决定，这一项置灰
    {
        COLORREF lbl = g_setDock ? RGB(0xBC, 0xC2, 0xCB) : TXT;
        ui_text(dc, fN, lbl, L"弹窗宽度", RECT{ 32, 925, 148, 951 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        double t = (g_widthVal - POPUP_W_MIN) / (double)(POPUP_W_MAX - POPUP_W_MIN);
        if (t < 0) t = 0; if (t > 1) t = 1;
        COLORREF acc = g_setDock ? RGB(0xD8, 0xDC, 0xE2) : ACCENT;
        draw_slider(dc, SL2_CY, SL_X1 + (int)(t * (SL_X2 - SL_X1)), SL_X1, SL_X2, acc);

        wchar_t wp[32];
        if (g_setDock) swprintf(wp, _countof(wp), L"自动");
        else           swprintf(wp, _countof(wp), L"%d px", g_widthVal);
        ui_text(dc, fN, g_setDock ? RGB(0xC2, 0xC7, 0xCF) : SUB, wp,
                RECT{ 340, 925, 404, 951 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // 页脚说明（本页没有保存按钮，改动直接落盘）
    if (g_setDock) {
        ui_text(dc, fM, RGB(0xA0, 0xA7, 0xB0), L"嵌入任务栏：横条可左右拖动，位置自动记住",
            RECT{ 32, 970, 408, 986 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        ui_text(dc, fM, RGB(0xA0, 0xA7, 0xB0), L"改动即刻保存并生效，关闭窗口无需确认",
            RECT{ 32, 970, 408, 986 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// 滑块：根据鼠标 x 反算不透明度
static void slider_set_from_x(HWND h, int x) {
    int span = SL_X2 - SL_X1;
    int t = (x - SL_X1) * 205 / (span > 0 ? span : 1);
    if (t < 0) t = 0; if (t > 205) t = 205;
    int v = 50 + t;
    if (v != g_alphaVal) {
        g_alphaVal = v;
        g_previewAlpha = v;                 // 实时预览，保存后才写入 g_cfg
        // 拖动时限流到 ~22 帧/秒，避免频繁重绘拖慢手感
        DWORD now = GetTickCount();
        if (now - g_lastPreviewAt >= 45) {
            g_lastPreviewAt = now;
            if (!g_popupVisible) show_popup_preview();
            else popup_refresh(g_hWnd);
        }
        InvalidateRect(h, NULL, FALSE);
    }
}

// 宽度滑块：根据鼠标 x 反算弹窗宽度，拖动即时重排窗口
static void width_set_from_x(HWND h, int x) {
    int span = SL_X2 - SL_X1;
    int range = POPUP_W_MAX - POPUP_W_MIN;
    int t = (x - SL_X1) * range / (span > 0 ? span : 1);
    if (t < 0) t = 0; if (t > range) t = range;
    int v = POPUP_W_MIN + t;
    if (v != g_widthVal) {
        g_widthVal = v;
        Lock(); g_cfg.popupWidth = v; Unlock();
        DWORD now = GetTickCount();
        if (now - g_lastPreviewAt >= 45) {
            g_lastPreviewAt = now;
            if (!g_popupVisible) show_popup_preview();
            else relayout_popup();          // 立刻按新宽度重排，所见即所得
        }
        InvalidateRect(h, NULL, FALSE);
    }
}

// 把界面状态写入配置并立即应用；返回 false 表示校验失败
static bool apply_settings(HWND h) {
    int bgi = GetDlgItemInt(h, IDC_BG, NULL, FALSE);
    int wi  = GetDlgItemInt(h, IDC_WIN, NULL, FALSE);
    if (bgi < 5) bgi = 5; if (wi < 1) wi = 1;
    if (bgi > 86400) bgi = 86400; if (wi > 3600) wi = 3600;

    HWND lb = GetDlgItem(h, IDC_LIST);
    std::vector<std::string> coins;
    int cnt = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        int l2 = (int)SendMessageW(lb, LB_GETTEXTLEN, i, 0);
        std::wstring t(l2 + 1, 0); SendMessageW(lb, LB_GETTEXT, i, (LPARAM)&t[0]);
        std::string s = to_a(t);
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) coins.push_back(s.substr(a, b - a + 1));
    }
    if (coins.empty()) {
        MessageBoxW(h, L"请至少保留一个关注币种", L"提示", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    Lock();
    g_cfg.bgInterval = bgi;
    g_cfg.winInterval = wi;
    g_cfg.coins = coins;
    g_cfg.pinToDesktop = g_setPin;
    g_cfg.mouseThrough = g_setThrough;
    g_cfg.alwaysOnTop = g_setTop;
    g_cfg.dockTaskbar = g_setDock;
    g_cfg.bgAlpha = g_alphaVal;
    g_cfg.popupWidth = g_widthVal;
    g_cfg.klineBar = kBars[g_barIndex].value;
    Unlock();
    g_previewAlpha = g_cfg.bgAlpha;

    apply_popup_layers();                    // 顶层 / 穿透按新配置生效
    relayout_popup();                        // 币种数可能变了（同时刷新记录的位置）
    settings_save_now();
    if (g_popupVisible && g_hWnd && IsWindow(g_hWnd))
        SetTimer(g_hWnd, 1, (UINT)(g_cfg.winInterval * 1000), NULL);  // 按新间隔刷新
    settle_popup_after_settings();           // 常驻→显示，非常驻→隐藏
    fetch_invalidate();                      // 币种/周期可能改过，立刻按新配置取数
    return true;
}

// 实时保存：本页没有保存按钮，任何改动都直接落盘（不弹窗、不重排窗口）
static void settings_save_now() {
    Lock(); Config c = g_cfg; Unlock();
    config_save(c, nullptr);
}

// 两个数字输入框：防抖后写入配置（编辑过程中不打字打断，也不写半截值）
static void settings_commit_numbers(HWND h) {
    HWND eb = GetDlgItem(h, IDC_BG), ew = GetDlgItem(h, IDC_WIN);
    if (!eb || !ew) return;
    if (GetWindowTextLengthW(eb) == 0 || GetWindowTextLengthW(ew) == 0) return;

    int bgi = GetDlgItemInt(h, IDC_BG, NULL, FALSE);
    int wi  = GetDlgItemInt(h, IDC_WIN, NULL, FALSE);
    if (bgi < 5) bgi = 5; if (bgi > 86400) bgi = 86400;
    if (wi < 1) wi = 1;   if (wi > 3600) wi = 3600;

    Lock();
    g_cfg.bgInterval = bgi;
    g_cfg.winInterval = wi;
    Unlock();
    // 按新间隔刷新
    if (g_popupVisible && g_hWnd && IsWindow(g_hWnd))
        SetTimer(g_hWnd, 1, (UINT)(wi * 1000), NULL);
    settings_save_now();
}

// 关闭：按当前界面最后确认一次并落盘，再销毁窗口。
// 之前是「关闭即回滚到打开前的快照」，于是删掉的币种一关窗口又冒回来；
// 现在改动全程实时保存，关闭只是收尾（校验不通过时留在页面上继续改）。
static void close_settings(HWND h) {
    if (!apply_settings(h)) return;
    DestroyWindow(h);
}

static void create_settings_controls(HWND h) {
    g_setPin = g_cfg.pinToDesktop;
    g_setThrough = g_cfg.mouseThrough;
    g_setTop = g_cfg.alwaysOnTop;
    g_setDock = g_cfg.dockTaskbar;
    g_alphaVal = g_cfg.bgAlpha; if (g_alphaVal < 50) g_alphaVal = 50; if (g_alphaVal > 255) g_alphaVal = 255;
    g_widthVal = g_cfg.popupWidth;
    if (g_widthVal < POPUP_W_MIN) g_widthVal = POPUP_W_MIN;
    if (g_widthVal > POPUP_W_MAX) g_widthVal = POPUP_W_MAX;
    g_previewAlpha = g_alphaVal;
    g_barIndex = bar_index_of(g_cfg.klineBar);
    g_setPlatform = market_platform_index(g_cfg.platform);
    g_pfHover = -1;

    // 无边框现代外观：圆角 + 阴影
    apply_modern_frame(h, SETTINGS_RADIUS);
    set_rounded_region(h, SETTINGS_RADIUS);

    // 字体
    HFONT fontTitle = CreateFontW(-17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    HFONT fontSection = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    HFONT fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    HFONT fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    SetPropW(h, L"fontTitle", (HANDLE)fontTitle);
    SetPropW(h, L"fontSection", (HANDLE)fontSection);
    SetPropW(h, L"fontNormal", (HANDLE)fontNormal);
    SetPropW(h, L"fontSmall", (HANDLE)fontSmall);

    SetWindowTheme(h, L"", L""); // 禁用子控件默认主题，便于自绘

    // 关闭按钮（自绘）
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        390, 15, 34, 34, h, (HMENU)IDC_CLOSE, g_hInst, NULL);

    // ---- 刷新频率：两个无边框数字输入框（底槽由父窗口自绘） ----
    HWND e1 = CreateWindowExW(0, L"EDIT", to_w(std::to_string(g_cfg.bgInterval)).c_str(),
        WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_CENTER | WS_TABSTOP,
        FIELD_X + 10, 117, FIELD_W - 20, 22, h, (HMENU)IDC_BG, g_hInst, NULL);
    SendMessageW(e1, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(2, 2));
    HWND e2 = CreateWindowExW(0, L"EDIT", to_w(std::to_string(g_cfg.winInterval)).c_str(),
        WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_CENTER | WS_TABSTOP,
        FIELD_X + 10, 155, FIELD_W - 20, 22, h, (HMENU)IDC_WIN, g_hInst, NULL);
    SendMessageW(e2, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(2, 2));

    // ---- 关注币种列表（自绘条目） ----
    HWND lb = CreateWindowExW(0, L"LISTBOX", L"",
        WS_VISIBLE | WS_CHILD | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
        WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT,
        38, 274, 364, 110, h, (HMENU)IDC_LIST, g_hInst, NULL);
    SendMessageW(lb, LB_SETITEMHEIGHT, 0, MAKELPARAM(28, 0));
    SetWindowTheme(lb, L"", L"");
    for (const auto& c : g_cfg.coins) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)to_w(c).c_str());

    HWND ec = CreateWindowExW(0, L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_UPPERCASE | ES_AUTOHSCROLL | WS_TABSTOP,
        42, 403, 198, 22, h, (HMENU)IDC_COIN, g_hInst, NULL);
    SendMessageW(ec, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(2, 2));

    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        258, 398, 68, 34, h, (HMENU)IDC_ADD, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        334, 398, 68, 34, h, (HMENU)IDC_DEL, g_hInst, NULL);

    // ---- 数据源：8 个自绘按钮（两行四列） ----
    for (int i = 0; i < market_platform_count() && i < 8; i++) {
        RECT r = pf_rect(i);
        CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            h, (HMENU)(INT_PTR)(IDC_PF_BASE + i), g_hInst, NULL);
    }

    // ---- 外观：开关 + 自绘滑块 ----
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        SW_X, 768, SW_W, SW_H, h, (HMENU)IDC_PIN_SW, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        SW_X, 800, SW_W, SW_H, h, (HMENU)IDC_THROUGH, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        SW_X, 832, SW_W, SW_H, h, (HMENU)IDC_TOPMOST, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
        SW_X, 864, SW_W, SW_H, h, (HMENU)IDC_DOCK_SW, g_hInst, NULL);

    // ---- 底部操作：没有保存按钮，改动即时落盘 ----

    // 子控件字体
    EnumChildWindows(h, [](HWND child, LPARAM) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)GetPropW(GetParent(child), L"fontNormal"), TRUE);
        return TRUE;
    }, 0);
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        create_settings_controls(h);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x1F, 0x23, 0x28));
        SetDCBrushColor(dc, RGB(0xFF, 0xFF, 0xFF));
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    case WM_ERASEBKGND:
        return 1;   // 全部在 WM_PAINT 里双缓冲绘制
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc0 = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);

        // 双缓冲，避免闪烁
        HDC dc = CreateCompatibleDC(dc0);
        HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        draw_settings(h, dc, rc);
        BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);

        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        HDC dc = di->hDC;
        RECT rc = di->rcItem;
        int id = di->CtlID;
        bool hover = (g_hoverBtn == di->hwndItem);
        SetBkMode(dc, TRANSPARENT);

        if (id == IDC_LIST) {
            // 自绘列表项
            if (di->itemID == (UINT)-1) return 0;
            bool sel = (di->itemState & ODS_SELECTED) != 0;
            HBRUSH b = CreateSolidBrush(sel ? RGB(0xFF, 0xEE, 0xD9) : RGB(0xFF, 0xFF, 0xFF));
            FillRect(dc, &rc, b);
            DeleteObject(b);
            if (sel) {
                RECT accent = { rc.left, rc.top + 4, rc.left + 3, rc.bottom - 4 };
                HBRUSH ab = CreateSolidBrush(RGB(0xF7, 0x93, 0x1A));
                FillRect(dc, &accent, ab);
                DeleteObject(ab);
            }
            wchar_t buf[128] = { 0 };
            SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)buf);
            HFONT f = (HFONT)GetPropW(h, L"fontNormal");
            RECT tr = rc; tr.left += 12;
            ui_text(dc, f, sel ? RGB(0xA0, 0x54, 0x0B) : RGB(0x1F, 0x23, 0x28),
                    buf, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return 0;
        }

        if (id == IDC_CLOSE) {
            COLORREF bg = hover ? RGB(0xF2, 0xF3, 0xF5) : RGB(0xFF, 0xFF, 0xFF);
            draw_rounded_rect(dc, rc, 8, bg, bg);
            int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, s = 6;
            HPEN pen = CreatePen(PS_SOLID, 2, hover ? RGB(0x1F, 0x23, 0x28) : RGB(0x6B, 0x72, 0x80));
            HPEN op = (HPEN)SelectObject(dc, pen);
            MoveToEx(dc, cx - s, cy - s, NULL); LineTo(dc, cx + s, cy + s);
            MoveToEx(dc, cx + s, cy - s, NULL); LineTo(dc, cx - s, cy + s);
            SelectObject(dc, op); DeleteObject(pen);
        } else if (id == IDC_PIN_SW || id == IDC_THROUGH || id == IDC_TOPMOST || id == IDC_DOCK_SW) {
            // 开关式控件
            bool val = (id == IDC_PIN_SW) ? g_setPin
                     : (id == IDC_THROUGH) ? g_setThrough
                     : (id == IDC_DOCK_SW) ? g_setDock : g_setTop;
            int rad = (rc.bottom - rc.top) / 2;
            COLORREF on = RGB(0xF7, 0x93, 0x1A);
            COLORREF off = RGB(0xCF, 0xD3, 0xDA);
            // RoundRect 的第 5/6 个参数是圆角椭圆的直径，传 2 倍才是胶囊形
            draw_rounded_rect(dc, rc, rad * 2, val ? on : off, val ? on : off);
            int kx = val ? (rc.right - rad) : (rc.left + rad);
            RECT knob = { kx - rad + 3, rc.top + 3, kx + rad - 3, rc.bottom - 3 };
            HBRUSH kb = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
            HPEN kp = CreatePen(PS_SOLID, 1, RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ ob2 = SelectObject(dc, kb), op2 = SelectObject(dc, kp);
            Ellipse(dc, knob.left, knob.top, knob.right, knob.bottom);
            SelectObject(dc, ob2); SelectObject(dc, op2);
            DeleteObject(kb); DeleteObject(kp);
        } else if (id >= IDC_PF_BASE && id < IDC_PF_BASE + 8) {
            // 数据源按钮：选中橙色实心，未选中白底 + 细边
            int pf = id - IDC_PF_BASE;
            bool sel = (pf == g_setPlatform);
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            COLORREF fill = sel ? (pressed ? RGB(0xEF, 0x86, 0x0C) : RGB(0xF7, 0x93, 0x1A))
                                : (hover ? RGB(0xF3, 0xF5, 0xF9) : RGB(0xFF, 0xFF, 0xFF));
            COLORREF border = sel ? fill : (hover ? RGB(0xD8, 0xDD, 0xE6) : RGB(0xE3, 0xE7, 0xEE));
            draw_rounded_rect(dc, rc, 8, fill, border);
            ui_text(dc, (HFONT)GetPropW(h, L"fontSmall"),
                    sel ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x3A, 0x40, 0x48),
                    market_platform_label(pf), rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            // 小按钮（添加 / 删除）
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            COLORREF fill = hover ? RGB(0xE6, 0xE8, 0xED) : RGB(0xF1, 0xF2, 0xF5);
            if (pressed) fill = RGB(0xDD, 0xE0, 0xE6);
            draw_rounded_rect(dc, rc, 8, fill, RGB(0xE3, 0xE7, 0xEE));
            const wchar_t* label = (id == IDC_ADD) ? L"添加" : L"删除";
            ui_text(dc, (HFONT)GetPropW(h, L"fontNormal"), RGB(0x2B, 0x30, 0x36),
                    label, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_trackMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            g_trackMouse = true;
        }
        POINT cl; GetCursorPos(&cl); ScreenToClient(h, &cl);

        // 拖动滑块
        if (g_sliderDrag) {
            if (g_sliderDrag == 2) width_set_from_x(h, (int)cl.x);
            else                   slider_set_from_x(h, (int)cl.x);
            return 0;
        }

        POINT pts; GetCursorPos(&pts);
        HWND newHover = NULL;
        int ids[] = { IDC_CLOSE, IDC_PIN_SW, IDC_THROUGH, IDC_TOPMOST, IDC_DOCK_SW, IDC_ADD, IDC_DEL };
        for (int id : ids) {
            HWND c = GetDlgItem(h, id); if (!c) continue;
            RECT wr; GetWindowRect(c, &wr);
            if (PtInRect(&wr, pts)) { newHover = c; break; }
        }
        for (int i = 0; !newHover && i < market_platform_count(); i++) {   // 数据源按钮
            HWND c = GetDlgItem(h, IDC_PF_BASE + i); if (!c) continue;
            RECT wr; GetWindowRect(c, &wr);
            if (PtInRect(&wr, pts)) newHover = c;
        }
        RECT slHit = { SL_X1 - 12, SL_CY - 16, SL_X2 + 12, SL_CY + 16 };
        RECT sl2Hit = { SL_X1 - 12, SL2_CY - 16, SL_X2 + 12, SL2_CY + 16 };
        bool onSlider = PtInRect(&slHit, cl) != 0 || PtInRect(&sl2Hit, cl) != 0;
        bool onBar = false;
        for (int i = 0; i < kBarsCount; i++) {
            RECT br = bar_rect(i);
            if (PtInRect(&br, cl)) { onBar = true; break; }
        }

        if (newHover != g_hoverBtn) {
            HWND old = g_hoverBtn; g_hoverBtn = newHover;
            if (old) InvalidateRect(old, NULL, FALSE);
            if (newHover) InvalidateRect(newHover, NULL, FALSE);
        }
        if (newHover || onSlider || onBar) SetCursor(LoadCursor(NULL, IDC_HAND));
        else SetCursor(LoadCursor(NULL, IDC_ARROW));

        // 标题栏拖动（设置窗口也支持贴边停靠）
        if (g_dragging) {
            int nx = g_dragRect.left + (pts.x - g_dragStart.x);
            int ny = g_dragRect.top + (pts.y - g_dragStart.y);
            RECT wr; HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hm, &mi); wr = mi.rcWork;
            int W = g_dragRect.right - g_dragRect.left, H = g_dragRect.bottom - g_dragRect.top;
            if (nx < wr.left) nx = wr.left; if (ny < wr.top) ny = wr.top;
            if (nx + W > wr.right) nx = wr.right - W; if (ny + H > wr.bottom) ny = wr.bottom - H;
            const int SNAP = 14;
            if (nx - wr.left <= SNAP) nx = wr.left;
            if (wr.right - (nx + W) <= SNAP) nx = wr.right - W;
            if (ny - wr.top <= SNAP) ny = wr.top;
            if (wr.bottom - (ny + H) <= SNAP) ny = wr.bottom - H;
            SetWindowPos(h, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_trackMouse = false;
        if (g_hoverBtn) { InvalidateRect(g_hoverBtn, NULL, FALSE); g_hoverBtn = NULL; }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pts; GetCursorPos(&pts); ScreenToClient(h, &pts);
        RECT sl2Hit = { SL_X1 - 12, SL2_CY - 16, SL_X2 + 12, SL2_CY + 16 };
        if (PtInRect(&sl2Hit, pts) && !g_setDock) {
            g_sliderDrag = 2;
            SetCapture(h);
            width_set_from_x(h, (int)pts.x);
            return 0;
        }
        RECT slHit = { SL_X1 - 12, SL_CY - 16, SL_X2 + 12, SL_CY + 16 };
        if (PtInRect(&slHit, pts)) {
            g_sliderDrag = 1;
            SetCapture(h);
            slider_set_from_x(h, (int)pts.x);
            return 0;
        }
        // K 线周期胶囊：点选后立即换周期取数
        for (int i = 0; i < kBarsCount; i++) {
            RECT br = bar_rect(i);
            if (PtInRect(&br, pts)) {
                if (g_barIndex != i) {
                    g_barIndex = i;
                    Lock(); g_cfg.klineBar = kBars[i].value; Unlock();
                    fetch_invalidate();            // 周期变了，作废在跑的那轮
                    settings_save_now();
                    InvalidateRect(h, NULL, FALSE);
                }
                return 0;
            }
        }
        if (pts.y < 60 && pts.x < 380) { // 标题栏区域（避开关闭按钮）
            g_dragging = true;
            GetCursorPos(&g_dragStart);
            GetWindowRect(h, &g_dragRect);
            SetCapture(h);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_sliderDrag) {
            if (g_sliderDrag == 1) { Lock(); g_cfg.bgAlpha = g_alphaVal; Unlock(); }
            g_sliderDrag = 0; ReleaseCapture();
            settings_save_now();          // 松手才写盘，拖动过程只做预览
            // 拖动中做过节流，松手补一次最终渲染
            if (!g_popupVisible) show_popup_preview();
            else popup_refresh(g_hWnd);
        }
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;
    }
    case WM_DESTROY: {
        HFONT f1 = (HFONT)GetPropW(h, L"fontTitle");
        HFONT f2 = (HFONT)GetPropW(h, L"fontSection");
        HFONT f3 = (HFONT)GetPropW(h, L"fontNormal");
        HFONT f4 = (HFONT)GetPropW(h, L"fontSmall");
        if (f1) DeleteObject(f1);
        if (f2) DeleteObject(f2);
        if (f3) DeleteObject(f3);
        if (f4) DeleteObject(f4);
        RemovePropW(h, L"fontTitle"); RemovePropW(h, L"fontSection");
        RemovePropW(h, L"fontNormal"); RemovePropW(h, L"fontSmall");
        g_hSettings = NULL;
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        // 数字改动：实时刷新限频提示；停手 600ms 后写入配置
        if ((id == IDC_BG || id == IDC_WIN) &&
            (code == EN_CHANGE || code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            if (code == EN_CHANGE) SetTimer(h, 6, 600, NULL);   // 防抖：连打不用每键都写盘
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (id >= IDC_PF_BASE && id < IDC_PF_BASE + 8) {
            // 切换数据源：清掉走势缓存与旧报价，立刻按新平台重新取数。
            // 用 fetch_invalidate 让正在跑的那一轮（问的是旧平台）当场作废，
            // 否则要等它把旧平台的数一个个问完，再等下一轮才会变新平台
            int pf = id - IDC_PF_BASE;
            if (pf >= market_platform_count()) return 0;
            if (pf != g_setPlatform) {
                g_setPlatform = pf;
                Lock(); g_cfg.platform = market_platform_id(pf); Unlock();
                market_clear_cache();
                settings_save_now();
                Lock(); g_view.clear(); Unlock();      // 别让上一家的价格留在屏幕上
                if (g_popupVisible) popup_refresh(g_hWnd);
                fetch_invalidate();
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        if (id == IDC_ADD) on_add(h);
        else if (id == IDC_DEL) on_del(h);
        else if (id == IDC_CLOSE) close_settings(h);
        else if (id == IDC_THROUGH) {
            g_setThrough = !g_setThrough;
            InvalidateRect(GetDlgItem(h, IDC_THROUGH), NULL, FALSE);
            Lock(); g_cfg.mouseThrough = g_setThrough; Unlock();
            apply_popup_layers();          // 立即生效（开启后弹窗不再接收鼠标）
            settings_save_now();
        }
        else if (id == IDC_TOPMOST) {
            g_setTop = !g_setTop;
            InvalidateRect(GetDlgItem(h, IDC_TOPMOST), NULL, FALSE);
            Lock(); g_cfg.alwaysOnTop = g_setTop; Unlock();
            apply_popup_layers();          // 立即生效
            settings_save_now();
        }
        else if (id == IDC_DOCK_SW) {
            g_setDock = !g_setDock;
            InvalidateRect(GetDlgItem(h, IDC_DOCK_SW), NULL, FALSE);
            Lock(); g_cfg.dockTaskbar = g_setDock; Unlock();
            enable_dwm_effects(g_hWnd);    // 横条透明无投影，竖排恢复系统阴影
            // 立即换成横条 / 竖排并重新贴位
            if (g_popupVisible) { relayout_popup(); dock_follow(); }
            else settle_popup_after_settings();
            settings_save_now();
        }
        else if (id == IDC_PIN_SW) {
            g_setPin = !g_setPin;
            InvalidateRect(GetDlgItem(h, IDC_PIN_SW), NULL, FALSE);
            // 实时生效：开启立即显示，关闭立即隐藏
            Lock(); g_cfg.pinToDesktop = g_setPin; Unlock();
            if (g_setPin) {
                if (!g_popupVisible) show_popup();
                else popup_refresh(g_hWnd);
            } else {
                hide_popup_force();
            }
            settings_save_now();
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 6) {   // 数字输入的防抖到点，落盘
            KillTimer(h, 6);
            settings_commit_numbers(h);
        }
        return 0;
    case WM_CLOSE: close_settings(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 调试：把窗口（含子控件）截屏存成 32 位 BMP，便于核对渲染效果
static void pump_ms(int ms) {
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < (DWORD)ms) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

static bool g_shotBlt = false;   // 调试：改用屏幕取色（对比 PrintWindow 是否有绘制伪影）

static void capture_window(HWND h, const wchar_t* path) {
    RECT rc; GetWindowRect(h, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, W, H);
    HGDIOBJ ob = SelectObject(mdc, bmp);
    // 优先让 DWM 直接渲染窗口（不怕被遮挡）
    if (g_shotBlt || !PrintWindow(h, mdc, 2)) {
        BitBlt(mdc, 0, 0, W, H, sdc, rc.left, rc.top, SRCCOPY);
    }
    SelectObject(mdc, ob);

    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(bi); bi.biWidth = W; bi.biHeight = -H;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)W * H * 4);
    GetDIBits(mdc, bmp, 0, H, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fh = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p = buf;
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"." : p.substr(0, s);
}

static void open_settings() {
    if (g_hSettings && IsWindow(g_hSettings)) { SetForegroundWindow(g_hSettings); return; }
    hide_popup();

    int W = SET_W, H = SET_H;
    // 主显示器居中
    HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    int availH = mi.rcWork.bottom - mi.rcWork.top;
    if (H > availH - 20) H = availH - 20;     // 小屏幕上别超出工作区
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2;
    int y = mi.rcWork.top + (availH - H) / 2;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    g_hSettings = CreateWindowExW(0, L"CryptoTraySettings", L"CryptoTray 设置",
        WS_POPUP, x, y, W, H, NULL, NULL, g_hInst, NULL);
    ShowWindow(g_hSettings, SW_SHOW);
    SetForegroundWindow(g_hSettings);
}

// ---- 入口 ----
int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmdLine, int) {
    g_hInst = hi;
    InitCommonControls();
    SetProcessDPIAware();

    InitializeCriticalSection(&g_cs);
    config_load(g_cfg, nullptr);

    g_fontName = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    g_fontPrice = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    g_fontChange = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");
    g_fontSmall = CreateFontW(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei");

    g_hIcon = CreateTrayIcon();

    // 消息专用窗口类（始终存在，接收托盘回调）
    WNDCLASSEXW mc = { sizeof(mc) };
    mc.lpfnWndProc = MsgWndProc; mc.hInstance = hi; mc.lpszClassName = L"CryptoTrayMsg";
    mc.hIcon = g_hIcon;
    RegisterClassExW(&mc);
    g_hMsgWnd = CreateWindowExW(0, L"CryptoTrayMsg", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hi, NULL);

    // 菜单宿主：屏幕外的 1x1 置顶小窗（不进 Alt+Tab，不可见）。
    // 只用来给 TrackPopupMenu 提供 topmost 的 Z 序归属，让菜单压得住任务栏。
    g_hMenuOwner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"CryptoTrayMsg", L"", WS_POPUP,
        -32000, -32000, 1, 1, NULL, NULL, hi, NULL);

    // 拖动落点提示窗：半透明、不抢焦点、不吃鼠标
    WNDCLASSEXW hc = { sizeof(hc) };
    hc.lpfnWndProc = HintProc; hc.hInstance = hi; hc.lpszClassName = L"CryptoTrayHint";
    RegisterClassExW(&hc);
    g_hHint = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"CryptoTrayHint", L"", WS_POPUP, 0, 0, 200, 40, NULL, NULL, hi, NULL);
    if (g_hHint) SetLayeredWindowAttributes(g_hHint, 0, 226, LWA_ALPHA);

    // 弹出窗口：默认无标题栏、支持透明度
    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = PopupProc; pc.hInstance = hi; pc.lpszClassName = L"CryptoTrayPopup";
    pc.hIcon = g_hIcon; pc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&pc);
    
    // 透明度的初值；实际由 popup_render() 通过 ULW 合成时生效
    g_previewAlpha = g_cfg.bgAlpha;
    create_popup_window();

    // 设置窗口：同样启用 DWM 效果
    WNDCLASSEXW sc = { sizeof(sc) };
    sc.lpfnWndProc = SettingsProc; sc.hInstance = hi; sc.lpszClassName = L"CryptoTraySettings";
    sc.hIcon = g_hIcon; sc.hbrBackground = NULL;
    sc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&sc);

    create_tray();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 拉取线程：所有网络请求都在这里串行执行，UI 线程永不阻塞；
    // 前台/后台刷新周期也在该线程内部调度，不再需要独立的 bg_thread
    g_hExit = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_hFetchEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_hFetchThread = (HANDLE)_beginthreadex(NULL, 0, fetch_thread, NULL, 0, NULL);
    SetEvent(g_hFetchEvt);   // 启动先拉一次，首次打开即有数据

    // 停靠任务栏：启动即显示，不需要点托盘
    if (g_cfg.dockTaskbar) show_popup();

    // 调试用：带 -settings 参数启动时直接打开设置窗口
    if (cmdLine && strstr(cmdLine, "-shotblt")) g_shotBlt = true;
    if (cmdLine && (strstr(cmdLine, "-settings") || strstr(cmdLine, "/settings"))) {
        open_settings();
        if (strstr(cmdLine, "-shot")) {
            pump_ms(900);   // 等窗口真正上屏并完成首帧绘制
            std::wstring p = exe_dir() + L"\\shot_settings.bmp";
            capture_window(g_hSettings, p.c_str());
            PostQuitMessage(0);   // 调试截完即退，避免挂住
        }
    }
    // 调试用：-shotpopup 显示弹窗并等数据到位后截图
    if (cmdLine && strstr(cmdLine, "-shotpopup")) {
        show_popup();
        pump_ms(3000);
        RedrawWindow(g_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        pump_ms(200);
        std::wstring p = exe_dir() + L"\\shot_popup.bmp";
        capture_window(g_hWnd, p.c_str());
        PostQuitMessage(0);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_taskbarCreated && msg.message == g_taskbarCreated) {
            create_tray();
            // 横条以任务栏为宿主窗口，Explorer 重建任务栏时会被连带销毁，这里重建
            if (!g_hWnd || !IsWindow(g_hWnd)) {
                g_popupVisible = false;
                g_hidePending = false;
                create_popup_window();
                if (g_cfg.dockTaskbar) show_popup();
            } else {
                dock_own();
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_hExit);
    if (g_hFetchThread) { WaitForSingleObject(g_hFetchThread, 5000); CloseHandle(g_hFetchThread); }
    CloseHandle(g_hExit);
    if (g_hFetchEvt) CloseHandle(g_hFetchEvt);
    http_shutdown();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hIcon) DestroyIcon(g_hIcon);
    DeleteCriticalSection(&g_cs);
    return 0;
}
