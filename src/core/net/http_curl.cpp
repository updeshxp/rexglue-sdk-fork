/**
 * @file        core/net/http_curl.cpp
 * @brief       libcurl-backed implementation of rex::net::Http*. See
 *              include/rex/net/http.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/net/http.h>

#include <curl/curl.h>

#include <fstream>
#include <mutex>

namespace rex::net {

namespace {

constexpr long kConnectTimeoutSec = 10;
constexpr long kTotalTimeoutSec = 60;
constexpr long kMaxRedirects = 5;

// curl_global_init/cleanup is process-wide and not thread-safe to call
// concurrently; every request goes through a fresh easy handle, but the
// global init only needs to happen once.
void EnsureGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

size_t WriteFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::ofstream*>(userdata);
  out->write(ptr, static_cast<std::streamsize>(size * nmemb));
  return out ? size * nmemb : 0;
}

struct ProgressState {
  const ProgressFn* fn = nullptr;
};

int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
  auto* state = static_cast<ProgressState*>(clientp);
  if (state->fn && *state->fn) {
    (*state->fn)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal));
  }
  return 0;
}

bool IsHttpsUrl(std::string_view url) {
  return url.substr(0, 8) == "https://";
}

}  // namespace

HttpResponse HttpGet(std::string_view url, const ProgressFn& progress) {
  HttpResponse response;
  if (!IsHttpsUrl(url)) {
    response.error = "only https:// URLs are supported";
    return response;
  }
  EnsureGlobalInit();

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "curl_easy_init failed";
    return response;
  }

  std::string url_str(url);
  curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSec);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "rex-net-http/1.0");

  ProgressState state{&progress};
  if (progress) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  }

  CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK) {
    response.error = curl_easy_strerror(result);
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = status;
  }
  curl_easy_cleanup(curl);
  return response;
}

HttpResponse HttpPostJson(std::string_view url, std::string_view json_body) {
  HttpResponse response;
  if (!IsHttpsUrl(url)) {
    response.error = "only https:// URLs are supported";
    return response;
  }
  EnsureGlobalInit();

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "curl_easy_init failed";
    return response;
  }

  std::string url_str(url);
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSec);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "rex-net-http/1.0");

  CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK) {
    response.error = curl_easy_strerror(result);
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = status;
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error) {
  if (!IsHttpsUrl(url)) {
    error = "only https:// URLs are supported";
    return false;
  }
  EnsureGlobalInit();

  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "failed to open destination file for writing";
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "curl_easy_init failed";
    return false;
  }

  std::string url_str(url);
  curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // downloads can be larger than a metadata query
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "rex-net-http/1.0");

  ProgressState state{&progress};
  if (progress) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  }

  CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  out.close();

  if (result != CURLE_OK) {
    error = curl_easy_strerror(result);
    return false;
  }
  if (status < 200 || status >= 300) {
    error = "HTTP " + std::to_string(status);
    return false;
  }
  return true;
}

}  // namespace rex::net
