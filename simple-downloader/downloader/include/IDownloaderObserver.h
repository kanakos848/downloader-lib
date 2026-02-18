#pragma once
// =============================================================================
// IDownloaderObserver.h
// ダウンローダーの通知を受け取るオブザーバーインターフェース
// Observer パターンによりUIや上位モジュールへの疎結合な通知を実現する
// =============================================================================

#include <cstdint>
#include <string>

namespace Downloader {

/// ダウンロード状態
enum class DownloadState {
    IDLE,        ///< 未開始
    DOWNLOADING, ///< ダウンロード中
    PAUSED,      ///< 一時停止中
    COMPLETED,   ///< 完了
    CANCELLED,   ///< キャンセル済み
    ERROR        ///< エラー発生
};

/// @brief ダウンローダーのイベントを受け取るオブザーバーインターフェース
/// UI スレッドからの継承を想定。コールバックはワーカースレッドから発火されることに注意。
class IDownloaderObserver {
public:
    virtual ~IDownloaderObserver() = default;

    /// @brief 進捗通知
    /// @param downloadedBytes ダウンロード済みバイト数
    /// @param totalBytes      総バイト数 (不明な場合は 0)
    /// @param percent         進捗パーセント (0.0〜100.0、不明な場合は -1.0)
    virtual void onProgress(int64_t downloadedBytes,
                            int64_t totalBytes,
                            double  percent) = 0;

    /// @brief ダウンロード完了通知
    virtual void onCompleted() = 0;

    /// @brief エラー通知
    /// @param errorMessage エラーの説明文字列
    virtual void onError(const std::string& errorMessage) = 0;

    /// @brief 一時停止通知
    virtual void onPaused() = 0;

    /// @brief 再開通知
    virtual void onResumed() = 0;

    /// @brief キャンセル通知
    virtual void onCancelled() = 0;
};

} // namespace Downloader
