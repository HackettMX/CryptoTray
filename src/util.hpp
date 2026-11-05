#pragma once
#include <string>
#include <windows.h>

// UTF-8 <-> UTF-16 互转，便于把业务字符串传给 Win32 的 *W 接口
// 注意：调用方常用 std::wstring(len+1, 0) 缓冲区接收，末尾会残留 L'\0'，
// 这里统一按第一个 NUL 截断，避免把 NUL 写进配置或 URL。
inline std::wstring to_w(const std::string& s) {
    size_t len = s.find('\0');
    if (len == std::string::npos) len = s.size();
    if (len == 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)len, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)len, &w[0], n);
    return w;
}

inline std::string to_a(const std::wstring& w) {
    size_t len = w.find(L'\0');
    if (len == std::wstring::npos) len = w.size();
    if (len == 0) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, &s[0], n, nullptr, nullptr);
    return s;
}
