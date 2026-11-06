# AGENTS.md

`env_bio_sense` — Seeed XIAO ESP32S3 を使った環境センサ＋生体光センサ（PPG）統合プラットフォーム。
このファイルは**他エージェント向けの引き継ぎ文書**。機能の説明は `README.md`、設計の正は `docs/` の憲章と仕様書にある。README と重複する内容はここに書かない。

## 最初に読む順

1. `docs/Project_Charter_XIAO_ESP32S3.md` — 基本理念・設計原則・コーディングポリシー・品質基準・完成形
2. `docs/Antigravity_XIAO_ESP32S3_アーキテクチャ仕様書.md` — 層構成と責務
3. 作業内容に応じて: `docs/audit_execution/`（監査記録）、`docs/SCD41_calibration_logic_review_instructions.md`、`docs/mb85rc256v_fram_sd_storage_integrated_work_instruction.md`、`docs/sd_remote_access/`、`docs/lc76g_gnss_integration/`、`docs/datasheet/`

**過去の状態を現在の実装とみなさない。** docs の記述より、コードと実機ログを優先して確認する。

## ビルド・テスト・実機

`pio` は PATH に無い。`/Users/atsushi/.platformio/penv/bin/pio` を使うか、`export PATH="$HOME/.platformio/penv/bin:$PATH"` する。再インストールはしない。

```bash
# ファームウェア
~/.platformio/penv/bin/pio run -e seeed_xiao_esp32s3
~/.platformio/penv/bin/pio run -e seeed_xiao_esp32s3 -t upload
~/.platformio/penv/bin/pio device monitor      # 115200

# ネイティブ単体テスト（PC 上）
~/.platformio/penv/bin/pio test -e native

# web 資産の再生成（web/ → include/generated/web_assets.h）
python3 scripts/generate_web_assets.py
```

`platformio.ini` の環境は8つ: `seeed_xiao_esp32s3` / `native` / `bmp585_test` / `bmp581_test` / `scd41_test` / `sgp41_test` / `sd_test` / `fram_test`。**通常の検証は `native` テストと `seeed_xiao_esp32s3` ビルド**で、他は特定センサの単体確認用。

## アーキテクチャ（憲章の設計原則）

- `main.cpp` は起動と更新のみ。ロジックを置かない
- **センサ同士は直接依存しない**。共通処理は `SensorManager` に集約する
- ドライバはアプリケーションを知らない。ライブラリ固有 API は Driver 層に閉じ込める
- ISR ではフラグ設定以外を行わない。`delay()` に依存しない。グローバル変数は最小限
- エラーでも全体停止しない。変更は既存コードへの影響を最小化する

```
src/
├── drivers/sensors/   sht45 / bmp5(base,581) / bme690 / scd41 / sgp41 / max30102 / lc76g
├── hal/               I2C・SPI・GPIO 等の境界
├── services/          sensor_manager / data_logger / storage / time / weather / wifi / web_server / ppg_session / archive / io_probe
├── storage/           FRAM・SD の管理
├── config/ utils/
include/               drivers, services, storage, hal, core, config, utils, generated
web/                   HTML/CSS/JS（ビルド時に web_assets.h へ埋め込む）
test/                  13ディレクトリ（native で実行）
scripts/               generate_web_assets.py / monitor_web_diag.py / test_web_io_probe.mjs
```

## 触る前に理解すべき中核設計

### 二層ストレージ（FRAM → SD）

測定データはまず FRAM（MB85RC256V）のリングバッファにジャーナリングし、後で SD へフラッシュする。破損や未挿入時も FRAM 側に留まる。

- FRAM 管理情報は**世代番号＋CRC の二重 checkpoint**。未知形式・破損時は自動初期化せず read-only へ退避する（勝手に初期化しない）
- SD 行は **書き込み → flush → close → 再 open → 内容照合の後**にだけ FRAM から消費する
- 処理中の対象ファイルと行 CRC を FRAM に残すため、再起動 replay で同じ行を二重追記しない
- 構造破損は I²C 読み出し失敗と区別し、同じ生データを二度確認してから `/fram_quarantine_v1.bin` へ保存・再読照合する。**証拠が確定するまで破損スロットも読み飛ばさない**
- FRAM v6 は 128byte slot を維持しつつ鮮度・状態・補償入力を保持。CSV は v7（71列固定、欠測は空欄）。旧 v5 の未 flush データは CSV v6 へ流し切ってから非破壊移行する

### 時刻

記録時点の monotonic 時刻・UTC・時刻源・同期状態・PPS age を**ひとつの snapshot** で取得する。GNSS 喪失時は UTC を継続しつつ `HOLDOVER` へ明示的に降格する。NTP epoch 秒と GNSS UTC を混同しない経路で変換する。

### I²C 競合と鮮度

全 I²C 処理をデバイス・操作別に識別して lock timeout / 通信エラーを集計（CSV v7 と Web API、異常増分は rate-limit して FRAM イベントへ）。SHT45 ヒーターはバスを長時間占有しない状態機械で動かす。SHT45・SGP41・BMP581・BME690 は**失敗または age 超過時に古い値を有効扱いしない**。

## 進め方

- 監査依頼なら、要件 → 実装箇所 → 証拠 → 判定の対応を示す。監査を勝手に修正依頼へ広げない
- 修正依頼なら、再現条件と受入条件を先に決めて小さく実装する
- 障害解析では、元ログの時刻・タスク・リセット理由・処理IDを抽出し、**観測 / 仮説 / 反証方法を分ける**。ログ不足なら原因を断定せず、取るべき最小ログを提示する
- 進捗判定は「処理IDごとの検証済み書込・消費件数」など直接証拠で行う。キュー総数の差分や隔離した破損スロットを成功件数にしない
- 非同期コールバック・ISR・ストレージ mutex の範囲と待ち時間を確認する。長い SD 処理をコールバックへ持ち込まない（既存のジョブ方式を調べる）
- API・レジスタを推測しない。導入バージョンとデータシートで確認する

## 検証と報告

- 変更に関連する既存テスト、対象ビルド、（Web 変更時は）構文・関連診断を実行し、**実行コマンドと結果を保存**する
- **実装済み / 静的検証済み / 実機検証済み / 未検証**を区別する。ビルド成功を実機受入完了と呼ばない
- 実機が使えなくても、実装と可能な検証を進め、再現手順・期待ログ・失敗判定を残す
- フラッシュ書き込み・校正値変更・データ消去は、現在の依頼範囲を確認してから

### 憲章の完了条件

ビルド、Upload、警告の増加なし、README 更新、動作・エラー処理の確認。**実機が使えない場合は未完了の条件を明示する。**

## 既知の過去事例（今回の原因として流用しない）

- `async_tcp` 中に長い SD 処理を置いて WDT に落ちた事例
- Flush の消費件数と UI 進捗表示が一致しなかった事例

いずれも過去の診断であり、現在の障害では**ログとコードで改めて確認する**。

## 現在の状態（引き継ぎ時点）

- 最終コミット: `7be1320`（2026-09-18）「feat: implement task stack monitoring and diagnostics with web UI and serial script」
- ブランチ: `main`、remote: `https://github.com/divergen371/env_bio_sense.git`
- **この文書の作成時点で未コミットの変更がある**（件数は増減するので `git status` で確認する）。主な対象:
  - 変更: `src/services/web_server_service.cpp`, `src/storage/storage_manager.cpp`, `include/services/web_server_service.h`, `include/storage/storage_manager.h`, `web/app.js`, `include/generated/web_assets.h`（生成物）, `docs/audit_execution/2026-09-18_task_stack_measurement_plan.md`
  - 未追跡: `docs/audit_execution/README.md`, `docs/audit_execution/env-bio-sense-audit.html`
  - 作業前に `git status` を確認し、無関係な差分を巻き込まない
- `include/generated/web_assets.h` は**生成物**。手で編集せず `scripts/generate_web_assets.py` で再生成する
- `.agents/` は `.gitignore` 対象（ローカル専用）。リポジトリに残る指示はこの `AGENTS.md` のみ
- `compile_commands.json`・`.cache/`・`.pio/` はローカル生成物

## 未確認・要整備

- ドキュメントの一部が Antigravity 時代の生成物で、現在の実装との差分が未検証（例: `docs/architecture_implementation/` の walkthrough と現行コードの一致）
- Web UI の自動テストは `scripts/test_web_io_probe.mjs` のみで、全体の回帰テストは無い
- `third_party/` の取り込み経緯と更新方針が文書化されていない
