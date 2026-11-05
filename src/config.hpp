#pragma once
#include <string>
#include <vector>

// 全局配置（持久化到 %APPDATA%\CryptoTray\config.ini）
struct Config {
    std::string platform = "okx"; // 数据源：okx/binance/bybit/gate/mexc/kucoin/htx/bitget
    int bgInterval = 30;         // 后台刷新间隔（秒）
    int winInterval = 5;         // 窗口显示时的刷新间隔（秒）
    std::vector<std::string> coins = { "BTC-USDT", "ETH-USDT", "SOL-USDT" };
    
    // UI 设置
    bool noTitleBar = true;      // 无标题栏
    int bgAlpha = 230;           // 整体不透明度 0-255 (230≈90%)，作用于整个弹窗（背景+文字）
    bool pinToDesktop = false;   // 固定到桌面：始终显示、不自动隐藏、最前
    bool alwaysOnTop = true;     // 置于最顶层：压在其它窗口之上
    bool mouseThrough = false;   // 鼠标穿透：点击直接落到下层窗口
    bool dockTaskbar = false;    // 停靠任务栏：弹窗变成嵌在任务栏内部的迷你横条
    int dockOffsetX = -1;        // 横条在任务栏内的水平偏移（-1 表示未记录，用默认位置）
    int popupWidth = 300;        // 弹窗宽度（像素，260-460，默认紧凑）
    int windowX = -1, windowY = -1; // 弹窗位置（-1 表示未记录，使用默认右下角）
    std::string klineBar = "1m"; // 走势图 K 线周期：5m/15m/1H/4H/1D/1W/1M/3M
};

bool config_load(Config& c, std::string* err);
bool config_save(const Config& c, std::string* err);
