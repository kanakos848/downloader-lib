#pragma once
// =============================================================================
// CurlHandle.h
// ICurlHandle の本番実装 - libcurl をラップする
// =============================================================================

#include "ICurlHandle.h"

#include <curl/curl.h>
#include <string>

namespace Downloader {

/// @brief libcurl の CURL* ハンドルを RAII でラップするクラス
/// ICurlHandle インターフェースを実装し、curl_easy_* API を安全に使用する
class CurlHandle final : public ICurlHandle {
public:
    /// @brief コンストラクタ - curl_easy_init() を呼び出す
    /// @throws std::runtime_error curl 初期化に失敗した場合
    CurlHandle();

    /// @brief デストラクタ - curl_easy_cleanup() を呼び出す (RAII)
    ~CurlHandle() override;

    // コピー不可
    CurlHandle(const CurlHandle&)            = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    // ICurlHandle インターフェース実装
    void setUrl(const std::string& url) override;
    void setResumeFrom(int64_t startByte) override;
    void enableHttp2() override;
    void setWriteCallback(WriteCallback cb) override;
    void setProgressCallback(ProgressCallback cb) override;
    void setConnectTimeout(long seconds) override;
    void setUserAgent(const std::string& ua) override;
    void setFollowLocation(bool follow) override;
    void setSslVerify(bool verify) override;
    CurlResult perform() override;
    long getHttpResponseCode() const override;
    std::string getLastError() const override;

private:
    /// curl 書き込みコールバックの静的ブリッジ関数
    static size_t curlWriteCallback(char* ptr, size_t size,
                                    size_t nmemb, void* userdata);

    /// curl 進捗コールバックの静的ブリッジ関数 (xferinfo)
    static int curlProgressCallback(void* clientp,
                                    curl_off_t dltotal, curl_off_t dlnow,
                                    curl_off_t ultotal, curl_off_t ulnow);

    /// CURLcode を CurlResult に変換するヘルパー
    CurlResult toCurlResult(CURLcode code) const;

    CURL*          handle_{nullptr};     ///< libcurl ハンドル
    WriteCallback  writeCallback_;       ///< ユーザー指定の書き込み CB
    ProgressCallback progressCallback_;  ///< ユーザー指定の進捗 CB
    char           errorBuffer_[CURL_ERROR_SIZE]{'\0'}; ///< エラー詳細バッファ
};

} // namespace Downloader
