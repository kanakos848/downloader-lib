#pragma once
// =============================================================================
// Downloader.h
// HTTP/HTTPS ファイルダウンローダーのメインインターフェース
//
// スレッドモデル:
//   - 呼び出し側スレッド : start/pause/resume/cancel を呼び出す
//   - ワーカースレッド   : 実際のダウンロードを実行し、オブザーバーを呼び出す
//
// ライフサイクル:
//   Downloader obj 生成
//     → addObserver()  (任意、複数可)
//     → startDownload()
//       ←→ pause() / resume()
//     → cancel() または完了
//     → デストラクタで安全にスレッドを終了
//
// RAII:
//   デストラクタで cancel + スレッドjoin を保証する
// =============================================================================

#include "ICurlHandle.h"
#include "IDownloaderObserver.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Downloader {

// =============================================================================
// DownloaderConfig: ダウンローダーの動作パラメータ
// =============================================================================
struct DownloaderConfig {
    size_t chunkSize         = 1024;   ///< 読み取りチャンクサイズ (bytes)
    long   connectTimeoutSec = 30;     ///< 接続タイムアウト (秒)
    bool   useHttp2          = true;   ///< HTTP/2 を有効にするか
    bool   sslVerify         = true;   ///< SSL 証明書を検証するか
    bool   followRedirects   = true;   ///< リダイレクトを追跡するか
    std::string userAgent    = "CppDownloader/1.0";
};

// =============================================================================
// DownloadStats: スナップショット取得用の統計情報
// =============================================================================
struct DownloadStats {
    int64_t       downloadedBytes = 0;
    int64_t       totalBytes      = 0; ///< 不明な場合は 0
    double        percent         = -1.0;
    DownloadState state           = DownloadState::IDLE;
    std::string   url;
    std::string   outputPath;
};

// =============================================================================
// Downloader クラス
// =============================================================================
class Downloader {
public:
    // -------------------------------------------------------------------------
    // ファクトリ関数 / コンストラクタ
    // -------------------------------------------------------------------------

    /// @brief デフォルトコンストラクタ（本番用 CurlHandle を使用）
    explicit Downloader(DownloaderConfig config = {});

    /// @brief テスト用コンストラクタ（curl ハンドルファクトリを外部注入）
    /// @param curlFactory  ICurlHandle を生成するファクトリ関数
    ///                     再開時に呼ばれることがあるため、毎回新規生成すること
    using CurlFactory = std::function<std::unique_ptr<ICurlHandle>()>;
    Downloader(DownloaderConfig config, CurlFactory curlFactory);

    /// @brief デストラクタ - cancel() + スレッド join を保証 (RAII)
    ~Downloader();

    // コピー不可（スレッドリソースを持つため）
    Downloader(const Downloader&)            = delete;
    Downloader& operator=(const Downloader&) = delete;

    // ムーブも不可（atomic/mutex を持つため）
    Downloader(Downloader&&)            = delete;
    Downloader& operator=(Downloader&&) = delete;

    // -------------------------------------------------------------------------
    // Observer 管理
    // -------------------------------------------------------------------------

    /// @brief オブザーバーを登録する（スレッドセーフ）
    /// @param observer 登録するオブザーバー（所有権は呼び出し元が保持）
    void addObserver(IDownloaderObserver* observer);

    /// @brief オブザーバーを解除する（スレッドセーフ）
    void removeObserver(IDownloaderObserver* observer);

    // -------------------------------------------------------------------------
    // ダウンロード制御
    // -------------------------------------------------------------------------

    /// @brief ダウンロードを開始する
    /// @param url        ダウンロード元 URL
    /// @param outputPath 保存先パス
    /// @return true: 開始成功 / false: すでに実行中など
    bool startDownload(const std::string& url,
                       const std::string& outputPath);

    /// @brief ダウンロードを一時停止する（スレッドセーフ）
    /// ワーカーが現在のチャンクを完了後に停止する
    void pause();

    /// @brief 一時停止を再開する（スレッドセーフ）
    /// Range リクエストで続きから再開する
    void resume();

    /// @brief ダウンロードをキャンセルする（スレッドセーフ）
    /// 呼び出し後、ワーカースレッドの終了を待たずにリターンする
    void cancel();

    // -------------------------------------------------------------------------
    // 状態取得
    // -------------------------------------------------------------------------

    /// @brief 現在の状態スナップショットを取得する（スレッドセーフ）
    DownloadStats getStats() const;

    /// @brief 現在のダウンロード状態を取得する（スレッドセーフ）
    DownloadState getState() const;

private:
    // -------------------------------------------------------------------------
    // 内部実装
    // -------------------------------------------------------------------------

    /// ワーカースレッドのエントリポイント
    void workerThread();

    /// 実際のダウンロード処理（ワーカースレッド内で実行）
    void doDownload();

    /// 一時停止ポイント - PAUSED 状態の間ブロックする
    /// @return true: 再開 / false: キャンセル
    bool waitIfPaused();

    // -------------------------------------------------------------------------
    // Observer 通知ヘルパー（ワーカースレッドから呼ぶ）
    // -------------------------------------------------------------------------
    void notifyProgress(int64_t downloaded, int64_t total, double percent);
    void notifyCompleted();
    void notifyError(const std::string& message);
    void notifyPaused();
    void notifyResumed();
    void notifyCancelled();

    // -------------------------------------------------------------------------
    // データメンバー
    // -------------------------------------------------------------------------

    DownloaderConfig              config_;
    CurlFactory                   curlFactory_;

    // Observer リスト
    mutable std::mutex            observerMutex_;
    std::vector<IDownloaderObserver*> observers_;

    // ダウンロード情報（スレッド間共有）
    mutable std::mutex            statsMutex_;
    std::string                   url_;
    std::string                   outputPath_;
    std::atomic<int64_t>          downloadedBytes_{0};
    std::atomic<int64_t>          totalBytes_{0};

    // 状態管理
    std::atomic<DownloadState>    state_{DownloadState::IDLE};

    // 一時停止制御
    mutable std::mutex            pauseMutex_;
    std::condition_variable       pauseCv_;
    std::atomic<bool>             pauseRequested_{false};
    std::atomic<bool>             cancelRequested_{false};

    // ワーカースレッド
    std::thread                   workerThread_;

    // curl 書き込みバッファ（低メモリ設計: チャンクサイズのみ使用）
    // ワーカースレッドのみがアクセスするため mutex 不要
    std::vector<char>             writeBuffer_;
};

} // namespace Downloader
