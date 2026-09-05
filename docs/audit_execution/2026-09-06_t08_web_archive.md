# T08 Web API・Archive・UX 実装記録

日付: 2026-09-06  
対象: P0-06、P1-12、P1-13、P2-04、P2-05、P2-06

## 結論

Web操作の認証・CSRF・パス検証、SDアクセスの排他、ZIPの公開前検証と元ファイル保護を実装した。ファイル一覧は再帰検索と複数選択操作へ変更し、日常状態・ファイル・保守操作を分離した。HTML／CSS／JavaScriptはC++から編集元を分離し、事前buildで決定的gzipへ変換して単一firmwareへ埋め込む。コードと自動検証は完了したが、実機でページ再読込み後に`async_tcp`のTask Watchdogが発火し、装置全体が再起動するP0-06を確認したため、Web実機Gateは不合格である。

## P0-06 Web一覧同期処理による再起動（2026-09-06）

APへ接続後、ブラウザのF5再読込みまたはWeb UI操作で通信が失われる症状は、APの強制解除ではなく装置全体の再起動だった。serial logには`Wi-Fi AP turned OFF`がなく、637.025秒にTask Watchdogが`async_tcp (CPU 1)`を名指ししてabortし、直後に`RTC_SW_CPU_RST`で再起動している。APが消えるのは再起動後の通常初期状態であり、ActionButton／P2-01の問題ではない。最後の通常sensor logからWatchdogまで約10.2秒である。

別起動でF5を再試行したところ、56.380秒に端末へIPを割り当てた後、64.771秒と65.276秒にAsyncTCPの`_tcp_poll(): throttling`が出て、70.601秒に同じ`async_tcp` Watchdog、同じbacktrace、同じfirmware ELF SHA-256 `b61230d32fcb1f5b`で再起動した。`throttling`はAsyncTCPのevent queueが一定量を超え、poll eventの追加を抑制した記録である。2回の独立再現により、電源断や偶発的なsensor障害ではなく、F5時のWeb処理が通信taskを長時間占有してqueueを滞留させる故障と判断する。

さらに、ページを再読込みせずファイル画面の「一覧更新」だけを押した試験でも、122.553秒に同じ`async_tcp` Watchdog、backtrace、ELF SHAで3回目の再起動を確認した。UIの`refreshFiles` click handlerは`loadFiles()`だけを呼び、これは`GET /api/files`へ直結する。これにより発火routeは`/api/files`へ確定した。route内の停止段階は計測されていないため、StorageManager mutex待ち、SD再帰走査、filter／sort、JSON応答生成のどれがWatchdog期限を消費したかは修正時の所要時間計測で確定する。

ブラウザ画面[`2026-09-06_webui_status_before_failure.png`](evidence/2026-09-06_webui_status_before_failure.png)では、障害前に状態APIが応答し、GNSS接続済み・記録buffer 10/480を表示している。[`2026-09-06_webui_files_stuck_loading.png`](evidence/2026-09-06_webui_files_stuck_loading.png)では、WebUI接続後にファイルtabを開いただけで一覧が`読込中…`から進まず、初期`boot()`が自動要求した`/api/files`も完了しないことを確認した。その後の[`2026-09-06_webui_files_fetch_failed.png`](evidence/2026-09-06_webui_files_fetch_failed.png)では`一覧取得失敗: Failed to fetch`となった。これは`/api/files`からHTTP error JSONが返ったのではなく、同時刻のserialで確認した装置再起動によりfetch接続自体が失われた表示と整合する。

確定した故障クラスは、AsyncTCPのcallback処理がWatchdog期限内に戻らないことである。現在の`AsyncTCP.cpp`は`_handle_async_event()`から戻った後にだけWatchdogをresetするため、callback内の同期I/Oまたはmutex待ちが長引くと今回の再起動になる。

発火routeは`/api/files`である。

- `web/app.js`の`boot()`は表示中のtabに関係なく、F5ごとに`loadStatus()`と`loadFiles()`を同時実行する。
- `/api/files`はAsyncTCP callback内で`StorageManager`を無期限lockし、SD全体を深さ5・最大512件まで再帰走査する。その後も検索用vector生成、sort、JSON生成を同じcallbackで同期実行する。
- ファイル数、directory数、SD応答時間、他のStorage処理とのmutex競合により、1回のrequestがWatchdog期限を超え得る。
- 状態の「更新」は5本の状態APIだけを呼ぶ。ファイル「一覧更新」単独では`/api/files`を発火routeとして確定した一方、状態「更新」単独の再現はまだ分離されておらず、F5直後なら同時開始された`/api/files`が残っている可能性がある。

修正はWatchdogの無効化・延長ではなく、次の非同期化を行う。

1. SD file indexの作成を低優先度の専用workerへ移し、件数または処理時間budgetごとにyieldしてStorageManager lockを短時間で解放する。
2. `/api/files`はcache済みindexのpageだけを即時返す。更新中は`202 Accepted`と進捗を返し、UIからpollする。
3. 初期表示では状態APIだけを読み、ファイルtabを初めて開いた時にだけ一覧を要求する。F5で不要なSD走査を開始しない。
4. 一覧更新buttonは走査を同期実行せず、workerへの更新要求だけをenqueueする。
5. route名、開始・終了、所要時間、mutex待ち時間、走査件数、失敗理由をcompact eventまたはrate-limitした診断へ残す。

修正後はF5、状態「更新」、ファイル「一覧更新」を各10回実施し、resetなし、各requestの応答、PPG drop増加なしを実機Gateとする。

## Web APIの安全性

- UI、API、downloadの全routeへDigest認証を適用した。APのSSID・パスワードとWeb管理者資格情報は`secrets.h`から設定し、固定の既知パスワードを廃止した。
- 起動ごとに128 bitのCSRF tokenを生成し、認証済み`/api/session`からだけ返す。状態を変更するPOSTは`X-CSRF-Token`の完全一致を必須とする。
- file pathを正規化し、`/data/ppg/`、`/data/system/`の許可階層と許可拡張子だけを受理する。`..`、backslash、percent encoding、重複slash、tmp、未知root、長すぎるpathを拒否する。
- 現在書込み中の環境CSV、activeなPPG session、Archive処理中の入力は削除・選択Archiveから保護する。現在CSVの直接取得前にはFRAMをflushし、安定した境界から共有mutex付きで読む。
- 自動ブラウザ時刻送信はClockが無効な場合だけ行い、サーバーはGNSS／NTPでdiscipline中の時計を上書きしない。非discipline時の明示的な手動送信は維持した。危険だった手動P0変更APIは削除した。
- `no-store`、`nosniff`、frame拒否、referrer抑止、self-only CSPを返す。UIは外部CDNへ依存しない。

## ZIPとSDデータ保護

- ZIPのopen、read、write、close、rename、removeを既存`StorageManager`と同じmutex規約へ統合した。
- 入力合計に2 MiBの余裕を加えた空き容量を事前確認し、一時ZIPへ作成する。出力名には時刻とsuffixを付け、既存ZIPを上書きしない。
- 一時ZIPは全entryの存在、期待件数・size、展開読取り、CRCを検証する。rename後の最終ZIPも同じ完全検証を再実施し、成功したものだけを`verified`として公開する。
- 最終検証に失敗したZIPは`.invalid.<suffix>`として隔離する。週次Archiveの元CSVは最終ZIP検証後にだけ削除し、削除失敗も状態へ記録する。
- `Completed`／`Failed`は5秒後に`Idle`へ戻し、同時開始は状態mutex下で拒否する。これにより再実行不能とstart raceを除去した。

## ファイル操作と画面構成

- SDの許可ファイルを再帰列挙し、検索、種類絞り込み、名前／容量順、50件ページ分割を追加した。最大列挙数は512件、複数選択は64件を上限とする。
- 表示ページ全選択、ページをまたぐ選択保持、選択解除、複数選択ZIP＋自動download、件数・合計容量確認付き一括削除を追加した。
- 画面を「状態」「ファイル」「メンテナンス」の3領域へ分離した。FRCとFactory ResetはAdvancedな折りたたみ領域へ置いた。
- 状態画面へFRAM、PPG session、drop／overflow、高度、AMeDAS、位置、I²Cを集約した。CSVはブラウザ内Canvasで簡易表示できる。

## Webソース分離

- 編集元を`web/index.html`、`web/app.css`、`web/app.js`へ分離した。inline handler、inline style、外部scriptは使わない。
- `scripts/generate_web_assets.py`が各ファイルをmtime 0のgzipへ変換し、`include/generated/web_assets.h`を生成する。PlatformIOのpre scriptでfirmware build前に同期する。
- 生成headerだけをPROGMEMへ埋め込むため、LittleFS imageを別配布せず従来どおり単一firmwareで動作する。
- asset参照は相対URLとし、実機配信に加えてローカルの`web/index.html`でもレイアウトを確認できる。ローカルではAPIが存在しないため状態値と操作は動作しない。

## 自動検証

- Web path／constant-time比較試験: 8件PASS
- 全native回帰試験: 97件PASS（11 suite）
- JavaScript構文検査: PASS
- HTML parser検査: PASS
- gzip 3資産の展開後byte一致: PASS
- system Python／PlatformIO Python生成物一致: PASS（SHA-256 `24d52b3717190578a07a7341fde14510974766f786c7156750e910fb07a5b55e`）
- `git diff --check`: PASS
- firmware build: PASS
- RAM: 98,264 / 327,680 bytes（30.0%）
- Flash: 1,314,441 / 3,342,336 bytes（39.3%）
- 残るwarningはMAX3010x libraryとWire libraryの`I2C_BUFFER_LENGTH`再定義1件で、T08変更由来ではない。

## 実機Gate

1. 更新firmwareを書き込み、設定済みAP資格情報で接続する。未認証routeが401、Digest認証後だけUI／API／downloadが使えることを確認する。
2. 変更POSTがtokenなし／誤tokenで403、`/api/session`の正しいtokenでだけ成功することを確認する。
3. `..`、percent encoding、backslash、未知root、tmp、許可外拡張子を各APIへ送り、拒否と無変更を確認する。
4. 記録中CSVとactive PPG sessionが削除／Archive対象にならず、確定後のネストsessionが一覧・選択・取得できることを確認する。
5. 1件、64件、CSV／PPG／JSON／ZIP混在で選択Archiveし、PCで全entryを展開して元ファイルと一致することを確認する。
6. 現在CSVのdownload中も行境界とCSV列数が正常で、PPGのdrop／overflowが増えないことを確認する。
7. PCとスマートフォンで検索、絞り込み、sort、page移動、ページ跨ぎ選択、一括取得、一括削除、確認表示を操作する。
8. ZIP write、finalize、rename、最終検証の各段階でSD抜去・低速化・電源断を注入し、未検証ZIPを公開せず元ファイルを保持することを確認する。
9. 成功／失敗後に状態が`Idle`へ戻り、続けてArchiveを再実行できることを確認する。
10. インターネット接続なし、ブラウザcacheなしでもUIが完全表示され、更新後に旧UIが残らないことを確認する。

## 残余リスク

- Digest認証はAP上の平文HTTPであり、同一無線区間を完全には秘匿しない。物理的に限定された保守用AP、十分長い個別パスワード、使用後のAP停止を前提とする。
- 512件を超える許可ファイルは一覧対象外となる。実運用件数を測り、必要ならcursor方式または日付directory単位のAPIへ拡張する。
- AsyncWebServerとArduino SDを使う実際の並行download／記録／Archive、および物理電源断はnative試験では再現できないため、T09の実機Gateで確認する。
