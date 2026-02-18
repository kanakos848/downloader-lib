```mermaid
sequenceDiagram
    actor       Client  as Client<br/>(main.cpp)
    participant DL      as Downloader
    participant Worker  as WorkerThread
    participant Curl    as ICurlHandle
    participant File    as OutputFile
    participant Obs     as IDownloaderObserver

    %% ================================================================
    %% ⑥ cancel シナリオ
    %% ================================================================
    rect rgb(255, 220, 220)
        Note over Client,Obs: ⑥ cancel シナリオ

        Client ->> DL: startDownload(url, path)
        DL ->> Worker: thread 起動
        activate Worker
        DL -->> Client: return true

        loop ダウンロード中
            Curl ->> Worker: writeCallback / progressCallback
            Worker ->> Obs: onProgress(...)
        end

        Client ->> DL: cancel()
        activate DL
        Note right of DL: cancelRequested_ = true<br/>pauseCv_.notify_all()<br/>(pause 中でも起こす)
        DL -->> Client: (即時 return)
        deactivate DL

        Note right of Worker: 次の progressCallback で<br/>cancelRequested_ を検出
        Curl ->> Worker: progressCallback(...)
        Worker -->> Curl: return 1 (中断シグナル)
        Curl -->> Worker: perform() = ABORTED

        Note right of Worker: cancelRequested_ を再確認
        Worker ->> DL: state_ = CANCELLED
        Worker ->> Obs: onCancelled()
        deactivate Worker
    end

    %% ================================================================
    %% ⑦ エラーシナリオ
    %% ================================================================
    rect rgb(255, 200, 200)
        Note over Client,Obs: ⑦ エラーシナリオ（ネットワーク障害 / HTTP 4xx・5xx）

        Client ->> DL: startDownload(badUrl, path)
        DL ->> Worker: thread 起動
        activate Worker

        Worker ->> Curl: perform()
        activate Curl
        Note right of Curl: 接続失敗 or<br/>HTTP 404/500 を受信
        Curl -->> Worker: NETWORK_ERROR or OK(httpCode=404)
        deactivate Curl

        alt ネットワークエラー
            Worker ->> DL: state_ = ERROR
            Worker ->> Obs: onError("Network error: ...")
        else HTTP 4xx / 5xx
            Worker ->> DL: state_ = ERROR
            Worker ->> Obs: onError("HTTP error: 404")
        end
        deactivate Worker
    end

    %% ================================================================
    %% ⑧ レジューム（中断 → 再開）シナリオ
    %% ================================================================
    rect rgb(220, 255, 240)
        Note over Client,Obs: ⑧ レジューム（前回中断 → 新セッションで再開）

        Note over File: 前回の部分ファイルが残存<br/>（例: 5 MB / 10 MB）

        Client ->> DL: startDownload(url, path)  ← 2回目の呼び出し
        activate DL
        DL ->> Worker: thread 起動
        activate Worker
        DL -->> Client: return true
        deactivate DL

        Worker ->> File: 既存ファイルサイズ取得
        File -->> Worker: resumeFrom = 5,242,880 bytes

        Worker ->> Curl: setResumeFrom(5242880)
        Note right of Curl: HTTP Request:<br/>Range: bytes=5242880-
        Worker ->> File: open (追記モード)

        Curl ->> Worker: HTTP 206 Partial Content 受信
        Note right of Worker: downloadedBytes_ を<br/>resumeFrom から開始

        loop 残り 5 MB をダウンロード
            Curl ->> Worker: writeCallback(chunk)
            Worker ->> File: append write
            Worker ->> Obs: onProgress(downloaded, total, %)
        end

        Worker ->> Obs: onCompleted()
        deactivate Worker
    end
