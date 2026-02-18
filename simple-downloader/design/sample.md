`src/main.cpp` です。

```
downloader/
└── src/
    └── main.cpp   ← ここ
```

内容をまとめると：

**`ConsoleObserver` クラス（約70行）**
`IDownloaderObserver` を継承した具体的な実装例です。
- `onProgress` → プログレスバーをコンソールに描画（`\r` で同一行上書き）
- `onCompleted / onError / onPaused / onResumed / onCancelled` → ログ出力
- `isFinished()` → 完了・エラー・キャンセルのいずれかを検出するヘルパー

**`demoDownloadWithControls()` 関数（約60行）**
pause → resume → cancel の一連の流れを実演します。

```cpp
Downloader::Downloader downloader(config);
ConsoleObserver observer("Main");
downloader.addObserver(&observer);

downloader.startDownload(url, outputPath);   // (1) 開始

std::this_thread::sleep_for(2s);
downloader.pause();                          // (2) 一時停止

std::this_thread::sleep_for(2s);
downloader.resume();                         // (3) 再開

// 5秒以内に終わらなければ
downloader.cancel();                         // (4) キャンセル
```

**`main()` 関数**
コマンドライン引数で URL と保存先を指定できます。省略時はデフォルト URL（`httpbin.org` の 10MB エンドポイント）でデモが動きます。

```bash
# 引数あり
downloader.exe https://example.com/file.zip output.zip

# 引数なし（デフォルトURLでデモ）
downloader.exe
```