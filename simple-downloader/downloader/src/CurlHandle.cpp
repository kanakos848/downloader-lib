// =============================================================================
// CurlHandle.cpp
// libcurl をラップする本番実装
// =============================================================================

#include "CurlHandle.h"

#include <stdexcept>
#include <cstring>

namespace Downloader {

// -----------------------------------------------------------------------------
// コンストラクタ / デストラクタ
// -----------------------------------------------------------------------------

CurlHandle::CurlHandle() {
    handle_ = curl_easy_init();
    if (!handle_) {
        throw std::runtime_error("curl_easy_init() failed");
    }

    // エラーバッファを curl に登録（詳細なエラーメッセージを取得するため）
    curl_easy_setopt(handle_, CURLOPT_ERRORBUFFER, errorBuffer_);

    // デフォルト設定
    // 進捗コールバックを有効化するために必要
    curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);
}

CurlHandle::~CurlHandle() {
    if (handle_) {
        curl_easy_cleanup(handle_);
        handle_ = nullptr;
    }
}

// -----------------------------------------------------------------------------
// 設定メソッド
// -----------------------------------------------------------------------------

void CurlHandle::setUrl(const std::string& url) {
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
}

void CurlHandle::setResumeFrom(int64_t startByte) {
    // curl_off_t (64bit) を使用することで 2GB 超のファイルに対応
    curl_easy_setopt(handle_, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(startByte));
}

void CurlHandle::enableHttp2() {
    // HTTP/2 を優先的に使用（サーバが対応していない場合は HTTP/1.1 にフォールバック）
    curl_easy_setopt(handle_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
}

void CurlHandle::setWriteCallback(WriteCallback cb) {
    writeCallback_ = std::move(cb);
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION,
                     &CurlHandle::curlWriteCallback);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, this);
}

void CurlHandle::setProgressCallback(ProgressCallback cb) {
    progressCallback_ = std::move(cb);
    curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION,
                     &CurlHandle::curlProgressCallback);
    curl_easy_setopt(handle_, CURLOPT_XFERINFODATA, this);
}

void CurlHandle::setConnectTimeout(long seconds) {
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, seconds);
}

void CurlHandle::setUserAgent(const std::string& ua) {
    curl_easy_setopt(handle_, CURLOPT_USERAGENT, ua.c_str());
}

void CurlHandle::setFollowLocation(bool follow) {
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
}

void CurlHandle::setSslVerify(bool verify) {
    curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
}

// -----------------------------------------------------------------------------
// ダウンロード実行
// -----------------------------------------------------------------------------

CurlResult CurlHandle::perform() {
    errorBuffer_[0] = '\0'; // エラーバッファをクリア
    CURLcode code = curl_easy_perform(handle_);
    return toCurlResult(code);
}

long CurlHandle::getHttpResponseCode() const {
    long httpCode = 0;
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &httpCode);
    return httpCode;
}

std::string CurlHandle::getLastError() const {
    if (errorBuffer_[0] != '\0') {
        return std::string(errorBuffer_);
    }
    return "Unknown curl error";
}

// -----------------------------------------------------------------------------
// 静的コールバックブリッジ
// -----------------------------------------------------------------------------

size_t CurlHandle::curlWriteCallback(char* ptr, size_t size,
                                      size_t nmemb, void* userdata) {
    auto* self = static_cast<CurlHandle*>(userdata);
    if (!self || !self->writeCallback_) {
        return 0; // 0 を返すと curl がエラーとして中断する
    }
    const size_t totalBytes = size * nmemb;
    return self->writeCallback_(ptr, totalBytes);
}

int CurlHandle::curlProgressCallback(void* clientp,
                                      curl_off_t dltotal, curl_off_t dlnow,
                                      curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* self = static_cast<CurlHandle*>(clientp);
    if (!self || !self->progressCallback_) {
        return 0; // 継続
    }
    return self->progressCallback_(
        static_cast<int64_t>(dltotal),
        static_cast<int64_t>(dlnow));
}

// -----------------------------------------------------------------------------
// ヘルパー: CURLcode → CurlResult 変換
// -----------------------------------------------------------------------------

CurlResult CurlHandle::toCurlResult(CURLcode code) const {
    switch (code) {
    case CURLE_OK:
        return CurlResult::OK;

    case CURLE_WRITE_ERROR:
        // 書き込みコールバックから CURL_WRITEFUNC_ERROR が返された場合
        // またはファイル書き込み中断
        return CurlResult::ABORTED_BY_CALLBACK;

    case CURLE_ABORTED_BY_CALLBACK:
        return CurlResult::ABORTED_BY_CALLBACK;

    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
        return CurlResult::NETWORK_ERROR;

    case CURLE_RANGE_ERROR:
        return CurlResult::RANGE_NOT_SATISFIED;

    default:
        return CurlResult::OTHER_ERROR;
    }
}

} // namespace Downloader
