#pragma once
#include <string>

// 基于 WinHTTP 的 HTTPS GET（系统自带，无需额外依赖）
// 内部复用长连接，并做全局限频（16 次/2 秒），避免触发交易所限频。
bool http_get(const std::wstring& host, const std::wstring& path, std::string& out, std::string* err);

// 整轮预算：本轮至少要发 n 个请求，配额不够返回 false（调用方整轮跳过）。
// 不让请求排队等配额，否则 1 秒刷新会被拖成忽快忽慢。
bool http_reserve(int n);

// 退出时释放长连接
void http_shutdown();
