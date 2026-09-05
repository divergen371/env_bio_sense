# P0-05 StorageTaskスタック枯渇

日付: 2026-09-06  
状態: 短時間実機Gate PASS・30分負荷Gate待ち

## 実測症状

firmware uploadはhash検証まで成功したが、起動後約3.8～5.0秒で再起動を繰り返した。複数回とも次のいずれかが記録されている。

- `A stack overflow in task StorageTask has been detected`
- `Stack canary watchpoint triggered (StorageTask)`
- stack破壊後の`Double exception`
- 再起動理由`RTC_SW_CPU_RST`

Wi-Fi初期化時の`esp_wifi_get_mac`／`esp_wifi_set_ps`警告は毎回その前に出ているが、panicが明示する原因は`StorageTask`のstack overflowである。GNSSのPPS警告も本件の再起動原因ではない。

## 原因

`StorageTask`は5秒ごとに最初のFRAM→SD flushを実行する。旧stack容量は4,096 bytesだったが、ELF逆アセンブルで次の固定frameを確認した。

- `storageTask`: 368 bytes
- `StorageManager::flushPendingToSd`: 1,360 bytes
- `StorageManager::appendAndVerifyCsvLine`: 1,664 bytes
- 上記3段だけで合計3,392 bytes

この時点でSD／FAT、String、logger、FRAM checkpoint等の下位呼出しに残るのは約704 bytesしかない。CSV v7の768 byte行bufferと、追記前tail・追記後read-backの769 byte配列2本が同じcall chainへ載ったことが直接原因である。起動約5秒という再現時刻も最初のflush周期と一致する。

## 修正

- `StorageTask`のstackを4,096 bytesから8,192 bytesへ増やした。
- CSVのtail検査と追記後read-backは逐次処理なので、769 byte配列2本を1本の再利用bufferへ統合した。
- FRAM recordの二度読みも同じrecord変数へ再読し、不要なretry copyをstackから除去した。
- 最初のflush後と以後60秒ごとに`Minimum remaining stack: ... bytes`を出力し、実機の最小余裕を観測できるようにした。

修正後のELF固定frameは`storageTask` 368 bytes、`flushPendingToSd` 1,264 bytes、`appendAndVerifyCsvLine` 896 bytes、合計2,528 bytesである。call chainの固定消費を864 bytes削減し、task容量を4,096 bytes増やした。

## 自動検証

- 全native回帰試験: 97件PASS
- firmware build: PASS
- RAM: 98,264 / 327,680 bytes（30.0%）
- Flash: 1,314,465 / 3,342,336 bytes（39.3%）
- firmware.bin SHA-256: `7dce3f199c657a62d685a3f2967c4a3c7c215d9493381132777cef83bcb5f68d`
- `git diff --check`: PASS

## 実機Gate

1. 修正firmwareを書込み、起動後5秒を超えて再起動しないことを確認する。
2. `StorageTask`の最初のstack残量logを記録し、2,048 bytes以上あることを確認する。
3. FRAMに残っていたpending recordが既存SD transactionから再開され、重複・欠落なくflushされることを確認する。FRAMやSDの初期化は行わない。
4. PPG記録なしで30分、その後PPG記録とWeb downloadを同時に30分実行し、再起動せず、60秒ごとの最小stack残量が2,048 bytesを下回らないことを確認する。
5. PPG interrupted-session復旧を1回実行し、同じstack基準を満たすことを確認する。

2,048 bytes未満の場合はP0-05をcloseせず、PPG復旧／SD検証bufferのheapまたはmanager-owned scratchへの移動を追加検討する。

## 再書込み後の実測（2026-09-06）

修正firmwareの再書込み後に約117秒分のserial logを取得した。

- stack overflow、stack canary、panic、resetはいずれも0件。
- 最初のFRAM flush後、起動5.523秒時点の最小stack残量は3,200 bytes。
- 約60秒後の69.870秒時点でも最小stack残量は3,200 bytesで、基準2,048 bytesを1,152 bytes上回った。
- 旧FRAM record 75件のflushと、その後の通常record flushまで継続した。

したがって「最初の5秒flushで必ず再起動する」回帰は解消したと判定する。ただし、30分通常運転、PPG＋Web同時負荷、PPG interrupted-session復旧は未実施なのでP0-05全体はcloseしない。

同じlogでは旧record 75件の一括flush中に約29.3秒メインloopが停止した。これはstack修正とは別のP0-03並行処理Gate不合格として扱う。また、slot 332で一度だけ`FramWalStatus::IoError`（status 5）を検出したが、recordを消費せず保持し、次回flushで2件を正常に検証・消費したため、この事象による欠落は確認されていない。
