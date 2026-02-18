```mermaid
sequenceDiagram
    %% ================================================================
    %% 登場人物
    %% ================================================================
    actor       Client      as Client<br/>(main.cpp)
    participant DL          as Downloader
    participant Worker      as WorkerThread
    participant Curl        as ICurlHandle<br/>(CurlHandle)
    participant File        as OutputFile
    participant Obs         as IDownloaderObserver

    %% ================================================================
    %% [1] 初期化フェーズ
    %% ================================================================
    rect rgb(230, 245, 255)
        Note over Client,Obs: ① 初期化フェーズ
        Client ->> DL: new Downloader(config)
        activate DL
        Note right of DL: state_ = IDLE<br/>curl_global_init()
        Client ->> DL: addObserver(&observer)
        DL -->> Client: (登録完了)
    end

    %% ================================================================
    %% [2] ダウンロード開始
    %% ================================================================
    rect rgb(230, 255, 230)
        Note over Client,Obs: ② startDownload
        Client ->> DL: startDownload(url, outputPath)
        activate DL
        Note right of DL: state_ = DOWNLOADING<br/>cancelRequested_ = false<br/>pauseRequested_  = false
        DL ->> Worker: std::thread 起動
        activate Worker
        DL -->> Client: return true
        deactivate DL
    end

    %% ================================================================
    %% [3] ダウンロードループ（ワーカースレッド内）
    %% ================================================================
    rect rgb(255, 255, 220)
        Note over Worker,Obs: ③ ダウンロードループ（ワーカースレッド内）

        Worker ->> File: 既存ファイルサイズ確認
        File -->> Worker: resumeFrom = N bytes

        Worker ->> Curl: curlFactory_() で生成
        activate Curl
        Worker ->> Curl: setUrl / setResumeFrom<br/>setWriteCallback / setProgressCallback
        Worker ->> File: open (追記モード)
        activate File

        loop 1024バイト × チャンク数
            Curl ->> Worker: progressCallback(dltotal, dlnow)
            Worker ->> Obs: onProgress(downloaded, total, percent%)
            activate Obs
            Obs -->> Worker: (return)
            deactivate Obs

            Curl ->> Worker: writeCallback(data, 1024)
            Worker ->> File: write(data, 1024)
            File -->> Worker: (書き込み完了)
            Worker -->> Curl: return 1024 (継続)
        end

        Curl -->> Worker: perform() = OK
        deactivate Curl
        Worker ->> File: flush & close
        deactivate File

        Note right of Worker: httpCode 確認
        Worker ->> Obs: onProgress(total, total, 100.0%)
        Worker ->> DL: state_ = COMPLETED
        Worker ->> Obs: onCompleted()
        deactivate Worker
    end

    %% ================================================================
    %% [4] デストラクタ（RAII）
    %% ================================================================
    rect rgb(240, 230, 255)
        Note over Client,Obs: ④ RAII デストラクタ
        Client ->> DL: (スコープアウト / delete)
        DL ->> DL: cancel() 内部呼び出し
        DL ->> Worker: thread.join() 完了待機
        Worker -->> DL: (終了)
        DL -->> Client: (デストラクタ完了)
        deactivate DL
    end
