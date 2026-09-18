# タスクstack再測定と修正順序

日付: 2026-09-18  
対象: P0-03、P0-06、P0-05

## 測定上の訂正

従来の`IoProbe health`にある`stack_free`は`IoProbeTask`自身の最小残量であり、全worker共通の値ではない。`1,188 bytes`だけからStorageTaskやFileIndexTaskのstack不足とは判定しない。

## 計測対象

- `loopTask`
- `async_tcp`
- `StorageTask`
- `FileIndexTask`
- `WeatherTask`
- `DisplayTask`
- `IoProbeTask`

firmwareは各タスクのhigh-water markが更新された時だけ`TaskStack`を出力する。Webの`GET /api/tasks`またはメンテナンス画面の「タスク余裕を測定」から、その時点までの最小残量とheapを取得できる。

## 実機測定順序

1. 起動後5分間、AP OFFで通常計測する。
2. APを起動し、状態画面を5分間表示する。
3. ファイルtabを開き、約500 entryの一覧生成が完了するまで待つ。
4. 一覧更新を3回行い、各回の完了後にタスク余裕を取得する。
5. Flushは現実装のまま再試験しない。既に`async_tcp` Watchdogを再現しており、先に非同期job化する。
6. Flush非同期化後、FRAM pending 1件、10件、満杯近傍を別々に測る。
7. SCD41 FRC／Factory Resetも非同期化後に単独実行し、I²C競合とstackを測る。
8. 最後に一覧生成、環境保存、PPG記録、Web状態更新を同時実行する。

各段階で`/api/tasks` JSON、`TaskStack`行、`IoProbe health`、reset有無を保存する。

## 判定基準

以下は本プロジェクトの暫定目安であり、ESP32-S3の保証値ではない。high-water markは起動後の累積最小残量（このSDKではbytes）であり、瞬間使用量やCPU使用率ではない。段階別比較には前後の値を保存し、独立測定には再起動する。未実行経路の余裕は保証できない。

- 2,048 bytes以上: 合格候補。長時間複合負荷で再確認する。
- 1,024～2,047 bytes: 注意。大きなlocal buffer、再帰、JSON生成を調査する。
- 1,024 bytes未満: 修正対象。stack増量だけでなく処理分割とbuffer配置を優先する。
- heap最小50 KiB未満: file cache、応答buffer、同時poll数を再評価する。
- `async_tcp`ではstack量にかかわらず、500 msを超える同期I/Oを禁止する。

## 修正の流れ

1. Web FlushをStorageTask jobへ変更し、APIは`202 Accepted`を即時返す。
2. `forceFlush()`による`wifiActive`書換えを廃止し、1 record単位で排他を解放する。
3. SCD41 FRC／Factory Resetをmaintenance jobへ移し、Web callbackからdelayとI²C sequenceを除去する。
4. file indexの差分更新・走査対象制限を比較検討する。永続化を採用する場合は、電源断・SD交換・外部変更時の再構築契約を先に決める。
5. 上記実装後に複合負荷でstack／heapを再測定する。
6. 低残量タスクだけ、local buffer削減、PSRAM／heap移動、stackサイズ調整を行う。
7. 各タスクの暫定余裕基準に加え、既存のWeb反復操作・保存完全性・PPG継続性Gateを満たし、Watchdog／resetなしを確認してP0-06のcloseを判断する。

## 2026-09-18 実機結果

入力: `env_task_stack.log`（1,435行、SHA-256 `88fdf501338b035acd12aba0ae905b74a4ba9199b2dfa3a575d40e3905a22817`）

約940秒の計測でWatchdog、abort、reset、AP停止はなかった。取得できたhigh-water markは次のとおり。

| Task | 最小空きstack | 判定 |
|---|---:|---|
| `async_tcp` | 12,724 bytes | 十分 |
| `loopTask` | 4,148 bytes | 十分 |
| `FileIndexTask` | 4,128 bytes | 十分 |
| `WeatherTask` | 2,500 bytes | 暫定基準を満たす |
| `DisplayTask` | 1,420 bytes | 注意、処理変更時に再確認 |
| `IoProbeTask` | 1,188 bytes | 診断taskのみ注意 |
| `StorageTask` | 4,612 bytes | 再起動後ログで取得、十分 |

従って、既知のWatchdog再起動はstack枯渇ではなく、`async_tcp`上の同期I/Oによるものとの判断を維持する。`async_tcp`自身には12 KiB以上の余裕がある。

file indexは約500 entryを3回走査し、それぞれ54.578秒、52.256秒、52.321秒だった。全て完了したが、最大Storage mutex保持は1.655秒、最大main loop gapは1.945秒へ増加した。1 entry単位へ分割しても、個々のSD APIが長時間mutexを保持し、FRAM appendを待たせる経路が残る。

heap最小値は通常計測時182 KiB、Weather通信後102 KiB、初回index公開後59.4 KiB、既存cacheを保持した2回目の再構築中29.5 KiBまで低下した。これは暫定50 KiB基準を下回る。値は起動後の履歴最小でありリーク確定ではないが、旧cache、新cache、Web応答用copyの同時保持がpeakを押し上げている可能性が高い。現在値と最大連続空きblockも追加計測する。

### この結果から確定した修正順序

1. Web Flushを非同期・1 record単位にし、`async_tcp`同期I/Oと`wifiActive`書換えを除去する。
2. FRAM WALとSD filesystemの排他を分離するか、Storage owner taskへ要求をqueue化し、file index中も環境recordのappendを待たせない。
3. file index更新中に旧cacheと全件新cacheを二重保持しない構造へ変更し、Web応答でも全件vectorをcopyしない。
4. cache更新後の現在heap、最大連続空きblock、断片化を追加し、29.5 KiBの内訳を確認する。
5. SCD41 FRC／Factory Resetをmaintenance jobへ移す。
6. `DisplayTask`と`IoProbeTask`は機能修正後に再測定し、1 KiB未満へ低下した場合だけbuffer移動またはstack調整を行う。
7. 複合負荷Gateへ進み、各機能の最大経路で再測定する。

## 一括削除とWeekly archiveの追加所見

「表示中を全選択」から「選択を削除」を実行したログでは、`async_tcp`の最小空きstackは12,568 bytes、再起動後の`StorageTask`は4,612 bytesだった。一方、AsyncTCP throttling後に`async_tcp (CPU 1)`のTask Watchdogが発火し、`RTC_SW_CPU_RST`となった。したがって一括削除もstack不足ではなく、`/api/files/delete`が最大64件について`SD.open -> close -> remove -> exists`をWeb callback内で同期反復することが直接原因である。

削除は専用Storage jobとして受け付け、APIは`202 Accepted`を返す。1ファイル境界で排他を解放し、進捗・削除済み・拒否・失敗をjob statusとして保持する。再起動時に処理済み範囲を誤って再削除しないよう、削除は存在確認を含む冪等な1件単位とする。完了時にfile index cacheを無効化する。

Weekly ZIPは現実装では「月曜日の03:00以上06:00未満（JST）」「Clock設定済み」「ArchiveManagerがIdle」を同時に満たした時だけ直前週を調べる。この3時間を電源OFF、時刻未確定、別archive処理中などで逃すと、その週を後から作成するcatch-up経路がない。`weekly_20260817_20260823.zip`以降がない症状と整合する。

Weekly schedulerは曜日・3時間窓への依存を廃止し、時刻確定後に「完了済み週のうち、ZIPがなく対象CSVが残る最古の週」を低頻度で1週ずつqueueへ入れる方式へ変更する。ZIP名の存在だけでなく、検証済み完了記録を確認する。起動遅延、複数週欠落、時刻未確定からの復帰、既存ZIP、失敗再試行、SD容量不足をGateへ追加する。

修正順序は、Web Flush、一括削除、SCD41 maintenanceを共通job受付へ移し、その後にWeekly catch-up schedulerとfile index cache無効化を実装する。
