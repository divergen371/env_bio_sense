# T08 Web API・Archive・UX 実装記録

日付: 2026-09-06  
対象: P1-12、P1-13、P2-04、P2-05、P2-06

## 結論

Web操作の認証・CSRF・パス検証、SDアクセスの排他、ZIPの公開前検証と元ファイル保護を実装した。ファイル一覧は再帰検索と複数選択操作へ変更し、日常状態・ファイル・保守操作を分離した。HTML／CSS／JavaScriptはC++から編集元を分離し、事前buildで決定的gzipへ変換して単一firmwareへ埋め込む。コードと自動検証は完了し、実機AP・SD・障害注入によるGateを残す。

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
