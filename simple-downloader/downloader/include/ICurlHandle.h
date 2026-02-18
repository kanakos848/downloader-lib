#pragma once
// =============================================================================
// ICurlHandle.h
// libcurl 操作を抽象化するインターフェース
// テスト時はモックに差し替え可能にし、curl 依存を分離する
// =============================================================================

#include <cstdint>
#include <functional>
#include <string>

namespace Downloader {

/// curl_easy_perform の戻り値相当
enum class CurlResult {
    OK,
    ABORTED_BY_CALLBACK, ///< コールバックからの中断
    HTTP_ERROR,          ///< 4xx/5xx
    NETWORK_ERROR,       ///< 接続失敗など
    RANGE_NOT_SATISFIED, ///< 416 - Range 未対応サーバ
    OTHER_ERROR
};

/// @brief libcurl ハンドル操作を抽象化するインターフェース
/// 本番実装は CurlHandle, テスト用はモッククラスを用意する
class ICurlHandle {
public:
    virtual ~ICurlHandle() = default;

    /// @brief 書き込みコールバック型
    /// @return 書き込んだバイト数。それ以外を返すと curl が中断する
    using WriteCallback = std::function<size_t(const char* data, size_t size)>;

    /// @brief 進捗コールバック型
    /// @return 0 継続, 非0 で curl が中断する
    using ProgressCallback =
        std::function<int(int64_t dltotal, int64_t dlnow)>;

    /// @brief URL を設定する
    virtual void setUrl(const std::string& url) = 0;

    /// @brief Range ヘッダーを設定してレジュームに対応する
    /// @param startByte 再開開始バイト位置
    virtual void setResumeFrom(int64_t startByte) = 0;

    /// @brief HTTP2 を有効化する
    virtual void enableHttp2() = 0;

    /// @brief 書き込みコールバックを設定する
    virtual void setWriteCallback(WriteCallback cb) = 0;

    /// @brief 進捗コールバックを設定する
    virtual void setProgressCallback(ProgressCallback cb) = 0;

    /// @brief タイムアウト設定 (秒)
    virtual void setConnectTimeout(long seconds) = 0;

    /// @brief User-Agent を設定する
    virtual void setUserAgent(const std::string& ua) = 0;

    /// @brief リダイレクトを許可する
    virtual void setFollowLocation(bool follow) = 0;

    /// @brief SSL 証明書検証の有効/無効
    virtual void setSslVerify(bool verify) = 0;

    /// @brief ダウンロードを実行する (ブロッキング)
    /// @return 結果コード
    virtual CurlResult perform() = 0;

    /// @brief 最後の HTTP レスポンスコードを取得する
    virtual long getHttpResponseCode() const = 0;

    /// @brief 直前のエラーメッセージを取得する
    virtual std::string getLastError() const = 0;
};

} // namespace Downloader
