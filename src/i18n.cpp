#include "i18n.hpp"
#include "util.hpp"
#include <windows.h>
#include <cstring>
#include <cwchar>

// ---------------------------------------------------------------------------
// 三语词表
//   每行 = 一个 key + 三种语言的文案，并排放在一起便于核对漏译。
//   新增语言只需在 kIds / kNativeNames / kLangCount 与 Row::v 上加一列。
//   缺译（nullptr）自动回退到简体中文，不会出现空白。
// ---------------------------------------------------------------------------
namespace {

struct Row {
    const wchar_t* key;
    const wchar_t* v[3];   // 0=zh-CN  1=zh-TW  2=en-US
};

const wchar_t* const kIds[]         = { L"zh-CN", L"zh-TW", L"en-US" };
const wchar_t* const kNativeNames[] = { L"简体",  L"繁體",  L"EN"    };
const int kLangCount = 3;

const Row kRows[] = {
    // ---- 托盘右键菜单 ----
    { L"menu.settings", { L"设置...",                 L"設定...",                    L"Settings..." } },
    { L"menu.topmost",  { L"置于最顶层",               L"置於最上層",                  L"Always on top" } },
    { L"menu.through",  { L"鼠标穿透",                 L"滑鼠穿透",                    L"Click-through" } },
    { L"menu.dock",     { L"嵌入任务栏",               L"嵌入工作列",                  L"Dock to taskbar" } },
    { L"menu.sponsor",  { L"❤ 赞助支持",               L"❤ 贊助支持",                  L"❤ Sponsor" } },
    { L"menu.star",     { L"为项目点个 Star",          L"為專案按個 Star",             L"Star on GitHub" } },
    { L"menu.exit",     { L"退出",                     L"結束",                        L"Exit" } },

    // ---- 托盘提示 / 浮窗 / 任务栏横条 ----
    { L"tray.tip",      { L"CryptoTray 行情",          L"CryptoTray 行情",             L"CryptoTray Ticker" } },
    { L"ui.empty",      { L"暂无关注币种，右键托盘 → 设置 添加",
                          L"尚無關注幣種，請在系統匣按右鍵 → 設定 新增",
                          L"No coins yet — open Settings" } },
    { L"err.loading",   { L"加载中",                   L"載入中",                      L"Loading" } },
    { L"hint.pin",      { L"松手固定窗口",             L"放開以固定視窗",              L"Release to pin window" } },
    { L"hint.move",     { L"将窗口移到此处",           L"將視窗移到此處",              L"Move window here" } },

    // ---- 设置窗：标题 / 语言 ----
    { L"set.title",     { L"CryptoTray 设置",          L"CryptoTray 設定",             L"CryptoTray Settings" } },
    { L"set.subtitle",  { L"行情刷新与外观",           L"行情更新與外觀",              L"Refresh & appearance" } },

    // ---- 卡片 1：刷新频率 ----
    { L"sec.rate",      { L"刷新频率",                 L"更新頻率",                    L"Refresh rate" } },
    { L"set.bg_interval", { L"后台刷新间隔",           L"背景更新間隔",                L"Background interval" } },
    { L"set.win_interval", { L"窗口显示时刷新",        L"視窗顯示時更新",              L"Visible interval" } },
    { L"unit.sec",      { L"秒",                       L"秒",                          L"s" } },
    { L"hint.rate_over", { L"约 %d 次 / 2 秒，已超安全阈值（16），可能被限频",
                           L"約 %d 次 / 2 秒，已超過安全閾值（16），可能被限流",
                           L"~%d req / 2s — over the safe limit (16), may be throttled" } },
    { L"hint.rate_near", { L"约 %d 次 / 2 秒，接近安全阈值（16），建议放慢",
                           L"約 %d 次 / 2 秒，接近安全閾值（16），建議放慢",
                           L"~%d req / 2s — near the safe limit (16), slow down" } },
    { L"hint.rate_ok",  { L"当前约 %d 次请求 / 2 秒 · 本地限流 16 次 / 2 秒",
                          L"目前約 %d 次請求 / 2 秒 · 本機限流 16 次 / 2 秒",
                          L"~%d req / 2s now · local limit is 16 req / 2s" } },

    // ---- 卡片 2：关注币种 ----
    { L"sec.coins",     { L"关注币种",                 L"關注幣種",                    L"Coins" } },
    { L"set.coin_hint", { L"格式如 BTC-USDT",          L"格式如 BTC-USDT",             L"e.g. BTC-USDT" } },
    { L"btn.add",       { L"添加",                     L"新增",                        L"Add" } },
    { L"btn.del",       { L"删除",                     L"刪除",                        L"Delete" } },
    { L"tag.spot",      { L"现货",                     L"現貨",                        L"Spot" } },
    { L"tag.perp",      { L"永续",                     L"永續",                        L"Perp" } },
    { L"msg.keep_one",  { L"请至少保留一个关注币种",   L"請至少保留一個關注幣種",      L"Keep at least one coin" } },
    { L"msg.tip",       { L"提示",                     L"提示",                        L"Notice" } },

    // ---- 卡片 3：数据源 ----
    { L"sec.source",    { L"数据源",                   L"資料來源",                    L"Data source" } },
    { L"set.source_hint", { L"公开行情接口 · 无需 API Key",
                            L"公開行情介面 · 無需 API Key",
                            L"Public APIs · no API key needed" } },

    // ---- 卡片 4：走势图周期 ----
    { L"sec.bars",      { L"走势图周期",               L"走勢圖週期",                  L"Chart period" } },
    { L"set.bars_hint", { L"每行 30 根",               L"每列 30 根",                  L"30 candles per row" } },
    { L"set.bars_note", { L"周期越长，走势线覆盖的时间跨度越大",
                          L"週期越長，走勢線涵蓋的時間跨度越大",
                          L"Longer periods cover a wider time span" } },

    // ---- 卡片 5：外观 ----
    { L"sec.look",      { L"外观",                     L"外觀",                        L"Appearance" } },
    { L"set.pin",       { L"固定到桌面（常驻显示）",   L"固定到桌面（常駐顯示）",      L"Pin to desktop" } },
    { L"set.through",   { L"鼠标穿透（点击穿透到下层）", L"滑鼠穿透（點擊穿透到下層）", L"Click-through (ignore mouse)" } },
    { L"set.topmost",   { L"置于最顶层（始终在最前）", L"置於最上層（始終在最前）",    L"Always on top (keep in front)" } },
    { L"set.dock",      { L"嵌入任务栏（迷你横条）",   L"嵌入工作列（迷你橫條）",      L"Dock to taskbar (mini bar)" } },
    { L"set.alpha",     { L"整体不透明度",             L"整體不透明度",                L"Opacity" } },
    { L"set.width",     { L"弹窗宽度",                 L"彈窗寬度",                    L"Popup width" } },
    { L"set.auto",      { L"自动",                     L"自動",                        L"Auto" } },
    { L"set.footer_dock", { L"嵌入任务栏：横条可左右拖动，位置自动记住",
                            L"嵌入工作列：橫條可左右拖曳，位置自動記住",
                            L"Dock mode: drag the bar left/right — position is remembered" } },
    { L"set.footer",    { L"改动即刻保存并生效，关闭窗口无需确认",
                          L"改動即刻儲存並生效，關閉視窗無需確認",
                          L"Changes save instantly — no need to confirm" } },

    // ---- K 线周期按钮 ----
    { L"bar.5m",        { L"5 分钟",                   L"5 分鐘",                      L"5 min" } },
    { L"bar.15m",       { L"15 分钟",                  L"15 分鐘",                     L"15 min" } },
    { L"bar.1h",        { L"1 小时",                   L"1 小時",                      L"1 hour" } },
    { L"bar.4h",        { L"4 小时",                   L"4 小時",                      L"4 hours" } },
    { L"bar.1d",        { L"1 天",                     L"1 天",                        L"1 day" } },
    { L"bar.1w",        { L"1 周",                     L"1 週",                        L"1 week" } },
    { L"bar.1m",        { L"1 月",                     L"1 個月",                      L"1 month" } },
    { L"bar.3m",        { L"3 月",                     L"3 個月",                      L"3 months" } },

    // ---- 行情错误（market.cpp 只产出机器键，这里翻译） ----
    { L"err.fail",      { L"取数失败",                 L"取數失敗",                    L"Fetch failed" } },
    { L"err.timeout",   { L"请求超时",                 L"請求逾時",                    L"Timed out" } },
    { L"err.busy",      { L"上一轮未完成，已跳过",     L"上一輪未完成，已略過",        L"Busy — skipped" } },
    { L"err.rate",      { L"刷新过频，本轮跳过",       L"更新過頻，本輪略過",          L"Rate limited" } },
    { L"err.notfound",  { L"该平台无此交易对",         L"該平台無此交易對",            L"Not listed here" } },
    { L"err.cooldown",  { L"限频冷却中",               L"限流冷卻中",                  L"Cooling down" } },
};

const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));
int g_lang = 0;

const wchar_t* find_or_null(const wchar_t* key) {
    if (!key || !*key) return nullptr;
    for (int i = 0; i < kRowCount; i++) {
        if (wcscmp(kRows[i].key, key) == 0) {
            const wchar_t* s = kRows[i].v[g_lang];
            return s ? s : kRows[i].v[0];
        }
    }
    return nullptr;
}

}  // namespace

namespace i18n {

int count() { return kLangCount; }

const wchar_t* id(int idx) {
    return (idx >= 0 && idx < kLangCount) ? kIds[idx] : kIds[0];
}

const wchar_t* native_name(int idx) {
    return (idx >= 0 && idx < kLangCount) ? kNativeNames[idx] : kNativeNames[0];
}

int from_id(const std::string& s) {
    for (int i = 0; i < kLangCount; i++) {
        const wchar_t* w = kIds[i];
        size_t n = wcslen(w);
        if (s.size() != n) continue;
        bool same = true;
        for (size_t k = 0; k < n; k++) {
            if ((unsigned char)s[k] != (unsigned char)w[k]) { same = false; break; }
        }
        if (same) return i;
    }
    return -1;
}

// 首次运行按系统 UI 语言猜：台/港/澳 → 繁体；其它中文 → 简体；其余 → 英文
int detect_system() {
    LANGID lid = GetUserDefaultUILanguage();
    switch (lid) {
        case 0x0404:   // zh-TW
        case 0x0C04:   // zh-HK
        case 0x1404:   // zh-MO
            return 1;
        default: break;
    }
    if ((lid & 0x00FF) == 0x04) return 0;
    return 2;
}

void set(int idx) {
    if (idx >= 0 && idx < kLangCount) g_lang = idx;
}

int current() { return g_lang; }

const wchar_t* t(const wchar_t* key) {
    const wchar_t* s = find_or_null(key);
    return s ? s : (key ? key : L"");
}

std::wstring err_text(const std::string& code) {
    if (code.empty()) return L"";
    // 机器键都是纯 ASCII，直接拼成 "err.xxx" 去查表
    std::wstring k = L"err.";
    for (char c : code) k += (wchar_t)(unsigned char)c;
    if (const wchar_t* s = find_or_null(k.c_str())) return s;
    return to_w(code);   // 认不出的原始报错（平台返回的内容）原样显示
}

int audit_missing() {
    int miss = 0;
    for (int i = 0; i < kRowCount; i++)
        for (int l = 0; l < kLangCount; l++)
            if (!kRows[i].v[l]) {
                miss++;
                OutputDebugStringW(L"[i18n] missing: ");
                OutputDebugStringW(kRows[i].key);
                OutputDebugStringW(L"\n");
            }
    return miss;
}

}  // namespace i18n
