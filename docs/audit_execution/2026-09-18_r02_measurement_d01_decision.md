# R02実機計測結果とD01保存処理方針

日付: 2026-09-18  
対象: P0-03（関連: P0-01、P0-05、P0-06、P1-06、P1-14、P2-02）

## 証拠

- 入力: `pasted-text.txt`（1,048行）
- SHA-256: `9cc5930121c71d9c6c34e18b4e2e195175357e854c2f80f8613ec32c8f6bf6a7`
- ログは起動全体ではなく、起動時flushの途中（約33.8秒）から約133.1秒までを収録している。
- `storage.flush` id=6: 開始 `10,039,163 us`、終了まで `107,153,656 us`、正常に180 recordを検証・消費。
- 同じmutexを待った`loopTask`の`storage.append`は `107,124,295 us`、内lock待ちは `107,058,416 us`。
- health集計は `hold_max_us=107153591`、`loop_gap_max_us=107220181`。診断ringの欠落は0。
- ログ中で完全に読めたCSV行処理142件は、最小476.641 ms、中央値489.928 ms、p95 497.811 ms、最大540.586 ms、平均490.090 ms。
- 段階別の代表値は、append用open中央値157.716 ms、既存末尾確認中央値155.234 ms、検証reopen中央値158.013 ms、本文write中央値71 us、flush中央値11.800 ms。遅延の主体は本文転送ではなくSDのopen／再openである。
- flush直後の `slot=354 status=5` は `FramWalStatus::IoError`。破損確定ではなく一時I²C読出し失敗で、recordを消費していないためデータ保護契約は守られた。
- StorageTask最小stack残量は3,168 bytes。従来の2,048 byte基準は満たすが、長時間・PPG・Web同時負荷Gateは未完了。

## 因果判定

P0-03の直接原因は、`flushPendingToSd()`がbacklog全件について、FRAM読出し、SD transaction journal、CSV write／flush／close／再open検証、FRAM consumeまでを単一のStorage mutex保持区間で繰り返すことにある。

この107秒停止から、次の派生が確認できる。

- main loopのappendとloop heartbeatが約107秒停止する。
- SCD41更新がstaleとなり、flush解放直後に不要な復旧へ入る。
- 起動時GNSS同期判定が進まず、NTP fallback開始がflush解放後へ遅れる。
- ActionButton判定もmain loop上にあるため、この区間はAPを有効化する長押しを処理できない。

一方、このログには`web.*`、`async_tcp` Watchdog、reset行がない。したがって、Storage待ちがWeb APIを止め得る共通原因であることは示すが、P0-06の発火routeを`/api/files`へ確定する証拠には使わない。

## D01決定

1. WAL record 1件を最小の完全性単位とする。`SD write -> flush/close -> reopen/read-back -> FRAM consume -> transaction finish`の順序は削らない。
2. 通常のStorageTask flushは1呼出し1 recordに制限し、record境界でStorage mutexを解放する。
3. backlog中は100 ms間隔で次の1 recordを試行する。実測SDでは各回がおおむね0.5～0.6秒となるため、main loopの待機上限をまず1秒未満へ縮小する。
4. PPG updateを各flush試行の前に実行し、4,096 sample（約40.96秒）のPSRAM ringを継続して排出できる構成を維持する。
5. 手動`forceFlush`は現段階では既存契約を維持する。AsyncTCP callbackからの全件flush、file scan、download I/OはM03でjob／cache化するまでP0-06未解決として扱う。
6. SD open／再openを削る高速化は、電源断耐性と重複回復契約を弱めるため採用しない。必要なら実SD速度、SPI周波数、ファイル設計を別の性能改善として測定する。

## 実装と次のGate

- `flushPendingToSd(maxRecords)`を追加し、通常StorageTaskは`maxRecords=1`を使用する。
- flush試行周期を5秒から100 msへ変更し、backlogを新規5秒recordより十分速く解消する。
- native／firmware build後、実機で180件相当のbacklogを用い、`hold_max_us < 1,000,000`、`loop_gap_max_us < 1,000,000`、Sequence欠落0、FRAM未保存消費0を確認する。
- PPG記録中はring drop 0、StorageTask stack残量2,048 bytes以上を追加条件とする。
- P0-06は、静的asset 1要求ずつの連続serial計測と、`status`／`files`分離試験を続行する。

## 自動確認

- `pio test -e native`: 104 / 104 PASS。
- `node scripts/test_web_io_probe.mjs`: PASS。
- `node --check web/app.js`: PASS。
- `pio run -e seeed_xiao_esp32s3`: PASS。RAM 104,128 bytes / 31.8%、Flash 1,328,825 bytes / 39.8%。
- ELF SHA-256: `03e38f856f0c917b09cd997f097e0a993f4895ac502509522e856d4227748c1d`。
- firmware.bin SHA-256: `853203b9536ee4b676cc7d1b72b91962d25606a8c69b6f812ac34c01650036fe`。
- 既存のMAX3010x `I2C_BUFFER_LENGTH`再定義警告は残る。今回変更によるbuild errorはない。

## 書込み後の実機確認

入力ログSHA-256: `5e821f8c87d481c525dac27f5ea4592c21aeeac1b8d16bb4783faf1dac0a68f1`（543行）

- 起動時は `pending=0`、`high_water=180`。したがって180件backlogの再試験ではなく、通常運転で新規生成されたrecordの確認である。
- 6件がそれぞれ `Verified and consumed 1 FRAM records` として処理され、複数recordを単一flushで処理していない。record境界での分割は実機でも有効。
- 完了行を回収できた5 flushは、最小947.791 ms、中央値1,153.273 ms、最大1,207.593 ms、平均1,125.695 ms。以前の107.154秒連続保持は再現していない。
- `loop_gap_max_us=893004`で、M02の「main loop停止1秒未満」は通常記録条件でPASS。
- `hold_max_us=1185588`のため、Storage mutex保持1秒未満という追加GateはFAIL。今回のSDでは1 recordの完全なwrite／再open検証自体が最大約1.21秒かかる。
- append 7件は中央値65.715 ms、最大81.815 ms。flushとの競合時に107秒待つ事象は再現していない。
- reset、Watchdog、FRAM unreadable、未保存consume、診断ring欠落は観測されなかった。
- SCD41は初回正常sampleを15秒以内に得られず1回復旧し、OLEDはI²C lock取得に複数回失敗した。P1-06／P1-15は未合格のまま、`/api/i2c`内訳の回収が必要。
- PPS interval／UTC mismatch警告が大量発生しており、P1-01／P1-02は明確に継続している。

判定: M02のrecord分割とloop停止1秒未満は通常記録条件でPASS。ただしbacklog、PPG同時記録、1.21秒Storage保持、SCD41／OLED競合が残るためP0-03はcloseしない。次段ではmain loopからFRAM append自体をqueue化し、StorageTaskでflushより優先して処理する。

## P0-06 AP強制解除の追加原因と修正

`TimeManager::update()`はGNSS未同期が60秒続くと、利用者向けAPの状態を確認せずNTP fallbackを開始していた。開始時に`WiFi.disconnect(true, true)`と`WiFi.mode(WIFI_STA)`、成功・失敗cleanupで`WiFi.mode(WIFI_OFF)`を実行するため、AP接続中でもradio modeを上書きしていた。Web更新は直接APを停止していないが、操作時刻とNTP fallbackが重なると「更新ボタンがAPを解除した」ように見える。

修正:

- `TimeManager::update()`へ`accessPointActive`を渡し、AP稼働中はSTA開始・disconnect・WIFI_OFFを行わない。
- APがNTP試行中に有効化された場合はNTP状態機械を停止し、APのradio modeを変更しない。
- 通常のF5では`/api/files`を自動取得せず、状態APIだけを取得する。ファイル一覧はファイルtabを開いた時だけ要求する。

自動確認:

- Web request isolation test: PASS。
- JavaScript構文: PASS。
- ESP32-S3 build: PASS。RAM 104,128 bytes / 31.8%、Flash 1,329,017 bytes / 39.8%。
- firmware.bin SHA-256: `bcf7f1aa40e6457d144ec710808dd2df64b81b54acdfd706045277002db0309c`。

## P0-06 `/api/files`発火route確定とM03実装

2026-09-18の「一覧更新」再現ログ（702行、SHA-256 `5e83d6f0e312dccfb4b079697e209f93449cfcd17d6eed75ebdf87a20b0d3be6`）により、残っていた再起動経路を確定した。

- 静的asset、session、status、location、weather、altitude、PPG応答はおおむね2～3 msで完了した。
- `web.files id=898`は`async_tcp`上でSD走査へ入り、0.64秒時点37 entry、1.64秒時点66、2.64秒時点87、3.89秒時点108まで進行した。
- その間にAsyncTCPのthrottlingが発生し、要求開始約10秒後にTask Watchdogが`async_tcp (CPU 1)`をabort、`RTC_SW_CPU_RST`で再起動した。
- したがって、この再現におけるAP消失はAP modeの解除ではなく、`/api/files`がAsyncTCP callback内で同期SD全件走査したことによる装置全体の再起動である。

M03修正:

- SD走査をCore 0の低優先度`FileIndexTask`へ移し、AsyncTCP callbackはworker起動とRAM cacheのsnapshotだけを行う。
- cache未生成時は`202 Accepted`と再試行間隔を返し、更新時は旧cacheを提供したまま`refreshing=true`を返す。
- SD mutexは全走査中保持せず、directory entry取得・metadata取得・close単位で解放し、entry間にyieldを入れる。
- UIは`202`または`refreshing=true`の間だけpollし、完了世代を自動表示する。
- 走査失敗時は既存cacheがあれば保持し、初回失敗だけ`503`とする。

自動確認はnative 104/104、Web request isolation、JavaScript構文、ESP32-S3 buildがPASS。RAM 104,128 bytes / 31.8%、Flash 1,331,473 bytes / 39.8%。firmware.bin SHA-256は`b7135f3d6ef42740bfaf29c650d77ccd71551e838267c1170e5c8ff1e042b477`、ELF SHA-256は`26250c04a2bba13de1d03753f38f6211316dc0fbac66a6c343ca8e6a96f02301`。

残件: P0-06はM03実装済み・実機Gate待ちとする。新firmwareで初回ファイルtab、一覧更新、F5を各10回実施し、resetなし、状態API継続、計測・保存継続、一覧世代更新を確認するまでcloseしない。
