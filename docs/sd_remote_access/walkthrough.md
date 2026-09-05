# SDカードリモートアクセス運用ガイド

更新日: 2026-09-06

## 接続

1. 本体のActionButtonを3秒長押しして、オンデマンドWi-Fi APを起動する。
2. `src/config/secrets.h`で設定した`WEB_AP_SSID`へ接続する。AP passwordは`WEB_ADMIN_PASSWORD`と共通である。
3. `http://192.168.4.1`または`http://env.local`を開く。
4. 認証画面へ`WEB_ADMIN_USER`と`WEB_ADMIN_PASSWORD`を入力する。

固定の既定passwordは使用しない。`src/config/secrets.h.example`を複製した後、12文字以上、推奨16文字以上のランダムpasswordへ変更する。`src/config/secrets.h`はGit管理対象外である。

## 画面

- 「状態」: FRAM使用量、PPG保存状態、高度、AMeDAS、現在地、I²C errorを確認する。
- 「ファイル」: SD rootと`/data/ppg`、`/data/system`を再帰表示する。検索、種類絞込み、名前／容量順、50件pageを利用できる。
- 「メンテナンス」: 時刻、Flush、compact event、I²C詳細、BMP581校正、SCD41 FRC、Factory Resetを扱う。破壊的操作は確認を必要とする。

## ファイル操作

- 表示中の操作可能ファイルを一括選択できる。選択はpageを移動しても保持する。
- 1件の取得は直接downloadする。複数件は装置内で1本のZIPを作成し、全entryの展開CRCとrename後の再読込み検証に成功してからdownloadする。
- 選択削除は対象件数と合計容量をまとめて確認し、サーバーが件別に削除／保護結果を返す。
- 現在書込み対象の環境CSV、記録中PPG session、Archive処理中の入力は削除・Archive対象から除外する。
- 現在の環境CSVを取得する場合、FRAMを明示的にFlushしてからファイルを開く。Wi-Fi AP中は通常Flushが停止しているため、その境界以降はdownload完了まで対象CSVが変化しない。
- PPG記録中の`raw.ppg.tmp`等は一覧へ公開せず、確定後または復旧後のファイルだけを操作対象とする。

## 安全性

- 全画面、API、downloadでDigest認証を要求する。
- POST操作は起動ごとに変わる128 bit CSRF tokenを要求する。
- ファイルpathは許可文字、拡張子、root、長さをサーバー側で検査し、`..`、encoded separator、未知directory、`.tmp`を拒否する。
- UIは外部CDNを使わない。APがInternetへ接続されていなくても、一覧と簡易CSV graphが動作する。
- HTML、CSS、JavaScriptの編集元は`web/`へ分離した。PlatformIOのpre-buildで決定的gzip dataを`include/generated/web_assets.h`へ生成し、単一firmware配布を維持する。
- stale UIを避けるため、Web assetとAPIへ`Cache-Control: no-store`を付ける。

## 終了

ActionButtonを再度3秒長押ししてAPを停止する。環境WALの通常SD Flushが再開し、AMeDAS／NTPのSTA接続も再び利用可能になる。
