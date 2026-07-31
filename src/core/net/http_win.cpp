/**
 * @file        core/net/http_win.cpp
 * @brief       WinHTTP-backed implementation of rex::net::Http*. See
 *              include/rex/net/http.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/net/http.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace rex::net {

namespace {

constexpr DWORD kConnectTimeoutMs = 10000;
constexpr DWORD kResolveTimeoutMs = 10000;
constexpr DWORD kSendTimeoutMs = 10000;
constexpr DWORD kReceiveTimeoutMs = 20000;

struct ParsedUrl {
  bool ok = false;
  std::wstring host;
  std::wstring path;  // includes query
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

std::wstring Widen(std::string_view s) {
  if (s.empty()) {
    return {};
  }
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

ParsedUrl ParseHttpsUrl(std::string_view url) {
  ParsedUrl result;
  if (url.substr(0, 8) != "https://") {
    return result;
  }
  std::wstring wide = Widen(url);

  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  wchar_t host_buf[256]{};
  wchar_t path_buf[4096]{};
  components.lpszHostName = host_buf;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
  components.lpszUrlPath = path_buf;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);

  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) {
    return result;
  }
  result.host = host_buf;
  result.path = path_buf;
  result.port = components.nPort ? components.nPort : INTERNET_DEFAULT_HTTPS_PORT;
  result.ok = true;
  return result;
}

class Session {
 public:
  Session() {
    handle_ = WinHttpOpen(L"rex-net-http/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handle_) {
      WinHttpSetTimeouts(handle_, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                         kReceiveTimeoutMs);
    }
  }
  ~Session() {
    if (handle_)
      WinHttpCloseHandle(handle_);
  }
  HINTERNET get() const { return handle_; }

 private:
  HINTERNET handle_ = nullptr;
};

// Performs one HTTPS request. `method` is L"GET" or L"POST". `body` is only
// sent for POST. Streams the response body via `on_data` (called 0+ times
// with a chunk); returns the final status/error.
HttpResponse DoRequest(std::string_view url, const wchar_t* method, std::string_view body,
                       const char* content_type,
                       const std::function<void(const uint8_t*, size_t)>& on_data,
                       const ProgressFn& progress) {
  HttpResponse response;
  ParsedUrl parsed = ParseHttpsUrl(url);
  if (!parsed.ok) {
    response.error = "only https:// URLs are supported";
    return response;
  }

  static Session session;
  if (!session.get()) {
    response.error = "WinHttpOpen failed";
    return response;
  }

  HINTERNET connect = WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0);
  if (!connect) {
    response.error = "WinHttpConnect failed";
    return response;
  }

  HINTERNET request =
      WinHttpOpenRequest(connect, method, parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    WinHttpCloseHandle(connect);
    response.error = "WinHttpOpenRequest failed";
    return response;
  }

  std::wstring headers;
  if (content_type) {
    headers = L"Content-Type: ";
    headers += Widen(content_type);
    headers += L"\r\n";
  }

  BOOL sent =
      WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                         headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
  if (sent) {
    sent = WinHttpReceiveResponse(request, nullptr);
  }
  if (!sent) {
    response.error =
        "request failed (network/TLS error, code " + std::to_string(GetLastError()) + ")";
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    return response;
  }

  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                      WINHTTP_NO_HEADER_INDEX);
  response.status = static_cast<long>(status_code);

  DWORD content_length = 0;
  DWORD cl_size = sizeof(content_length);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_CONTENT_LENGTH,
                      WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &cl_size,
                      WINHTTP_NO_HEADER_INDEX);

  uint64_t downloaded = 0;
  std::vector<uint8_t> buffer(65536);
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
      break;
    }
    if (buffer.size() < available) {
      buffer.resize(available);
    }
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) {
      break;
    }
    downloaded += read;
    if (on_data) {
      on_data(buffer.data(), read);
    }
    if (progress) {
      progress(downloaded, content_length);
    }
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  return response;
}

}  // namespace

HttpResponse HttpGet(std::string_view url, const ProgressFn& progress) {
  std::string body;
  HttpResponse response = DoRequest(
      url, L"GET", {}, nullptr,
      [&](const uint8_t* data, size_t len) {
        body.append(reinterpret_cast<const char*>(data), len);
      },
      progress);
  response.body = std::move(body);
  return response;
}

HttpResponse HttpPostJson(std::string_view url, std::string_view json_body) {
  std::string body;
  HttpResponse response = DoRequest(url, L"POST", json_body, "application/json",
                                    [&](const uint8_t* data, size_t len) {
                                      body.append(reinterpret_cast<const char*>(data), len);
                                    },
                                    {});
  response.body = std::move(body);
  return response;
}

bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error) {
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "failed to open destination file for writing";
    return false;
  }
  HttpResponse response = DoRequest(
      url, L"GET", {}, nullptr,
      [&](const uint8_t* data, size_t len) { out.write(reinterpret_cast<const char*>(data), len); },
      progress);
  out.close();
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (response.status < 200 || response.status >= 300) {
    error = "HTTP " + std::to_string(response.status);
    return false;
  }
  return true;
}

}  // namespace rex::net
