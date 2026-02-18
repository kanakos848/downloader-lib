#pragma once
// =============================================================================
// MockCurlHandle.h
// テスト用 ICurlHandle モック実装
//
// 設計方針:
//  - curl ネットワーク呼び出しを一切行わずに動作をシミュレートする
//  - perform() が呼ばれると「仮想データ」を writeCallback に送信する
//  - progressCallback を適切なタイミングで呼び出す
//  - pause/cancel によるコールバックからの中断を再現する
//  - 完全に制御可能なため、再現性のあるテストが書ける
// =============================================================================

#include "ICurlHandle.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace Downloader {
namespace Test {

/// @brief モック設定 - テストケースごとに動作を変える
struct MockConfig {
    size_t      totalSize     = 10 * 1024;  ///< 仮想ファイルサイズ (バイト)
    size_t      chunkSize     = 1024;       ///< 1回の write コールバックサイズ
    CurlResult  returnResult  = CurlResult::OK; ///< perform() の戻り値
    long        httpCode      = 200;        ///< HTTP レスポンスコード
    std::string errorMessage  = "";         ///< エラーメッセージ
    bool        supportsRange = true;       ///< Range ヘッダーをサポートするか
    /// perform 中にスリープする間隔（テストを遅くしすぎないため小さくする）
    std::chrono::milliseconds chunkDelay{1};
};

/// @brief ICurlHandle のモック実装
class MockCurlHandle final : public ICurlHandle {
public:
    explicit MockCurlHandle(MockConfig config = {})
        : mockConfig_(std::move(config)) {}

    ~MockCurlHandle() override = default;

    // -------------------------------------------------------------------------
    // ICurlHandle インターフェース実装
    // -------------------------------------------------------------------------

    void setUrl(const std::string& url) override {
        url_ = url;
    }

    void setResumeFrom(int64_t startByte) override {
        resumeFrom_ = startByte;
        ++resumeFromCallCount_;
    }

    void enableHttp2() override {
        http2Enabled_ = true;
    }

    void setWriteCallback(WriteCallback cb) override {
        writeCallback_ = std::move(cb);
    }

    void setProgressCallback(ProgressCallback cb) override {
        progressCallback_ = std::move(cb);
    }

    void setConnectTimeout(long seconds) override {
        connectTimeout_ = seconds;
    }

    void setUserAgent(const std::string& ua) override {
        userAgent_ = ua;
    }

    void setFollowLocation(bool follow) override {
        followLocation_ = follow;
    }

    void setSslVerify(bool verify) override {
        sslVerify_ = verify;
    }

    /// @brief 仮想ダウンロードを実行する
    /// totalSize バイトのゼロデータを chunkSize 単位で writeCallback に送る
    CurlResult perform() override {
        ++performCallCount_;

        // エラー即時返却の設定
        if (mockConfig_.returnResult != CurlResult::OK &&
            mockConfig_.returnResult != CurlResult::ABORTED_BY_CALLBACK) {
            return mockConfig_.returnResult;
        }

        // Range 非対応サーバをシミュレート
        if (resumeFrom_ > 0 && !mockConfig_.supportsRange) {
            mockConfig_.httpCode = 416;
            return CurlResult::RANGE_NOT_SATISFIED;
        }

        // 仮想データを生成してチャンク単位で送信
        const size_t totalSize  = mockConfig_.totalSize;
        const size_t chunkSize  = mockConfig_.chunkSize;
        size_t       sent       = static_cast<size_t>(resumeFrom_);
        const int64_t totalForProgress = static_cast<int64_t>(totalSize);

        // チャンクデータバッファ（ゼロ埋め）
        std::vector<char> buffer(chunkSize, '\0');

        while (sent < totalSize) {
            // 残りサイズを計算
            const size_t remaining  = totalSize - sent;
            const size_t toSend     = std::min(chunkSize, remaining);

            // 進捗コールバックを呼び出す
            if (progressCallback_) {
                int64_t dlnow  = static_cast<int64_t>(sent - static_cast<size_t>(resumeFrom_));
                int64_t dltotal = static_cast<int64_t>(totalSize - static_cast<size_t>(resumeFrom_));
                int ret = progressCallback_(dltotal, dlnow);
                if (ret != 0) {
                    // キャンセルされた
                    return CurlResult::ABORTED_BY_CALLBACK;
                }
            }

            // 書き込みコールバックを呼び出す
            if (writeCallback_) {
                size_t written = writeCallback_(buffer.data(), toSend);
                if (written != toSend) {
                    // 書き込み失敗 = pause/cancel
                    return CurlResult::ABORTED_BY_CALLBACK;
                }
            }

            sent += toSend;

            // チャンク間のデリミタスリープ（テスト速度優先で最小限）
            if (mockConfig_.chunkDelay.count() > 0) {
                std::this_thread::sleep_for(mockConfig_.chunkDelay);
            }
        }

        // 最終進捗通知 (100%)
        if (progressCallback_) {
            progressCallback_(totalForProgress,
                              static_cast<int64_t>(totalSize - static_cast<size_t>(resumeFrom_)));
        }

        return mockConfig_.returnResult;
    }

    long getHttpResponseCode() const override {
        return mockConfig_.httpCode;
    }

    std::string getLastError() const override {
        return mockConfig_.errorMessage.empty()
                   ? "Mock error"
                   : mockConfig_.errorMessage;
    }

    // -------------------------------------------------------------------------
    // テスト検証用アクセサ
    // -------------------------------------------------------------------------

    const std::string& getUrl()       const { return url_; }
    int64_t  getResumeFrom()          const { return resumeFrom_; }
    int      getPerformCallCount()    const { return performCallCount_; }
    int      getResumeFromCallCount() const { return resumeFromCallCount_; }
    bool     isHttp2Enabled()         const { return http2Enabled_; }
    bool     isSslVerify()            const { return sslVerify_; }
    long     getConnectTimeout()      const { return connectTimeout_; }
    const std::string& getUserAgent() const { return userAgent_; }

private:
    MockConfig       mockConfig_;

    // 設定値記録（検証用）
    std::string      url_;
    int64_t          resumeFrom_{0};
    bool             http2Enabled_{false};
    bool             sslVerify_{true};
    bool             followLocation_{true};
    long             connectTimeout_{30};
    std::string      userAgent_;

    // コールバック
    WriteCallback    writeCallback_;
    ProgressCallback progressCallback_;

    // 呼び出し回数カウンタ（検証用）
    int              performCallCount_{0};
    int              resumeFromCallCount_{0};
};

} // namespace Test
} // namespace Downloader
