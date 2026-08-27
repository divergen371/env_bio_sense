# 実装計画: BME690の追加

本計画は、`docs/bme690_addition_work_instruction.md` で指定された要件に基づき、既存のプロジェクトにBME690センサーを追加するための手順を定めます。

## 1. 目的

既存のI2Cバス（アドレス`0x76`）にBME690を追加します。既存のセンサー、PPG、FRAM、SD保存、OLED表示への影響を最小限に抑えつつ、BME690の温度、湿度、気圧、生ガス抵抗値を5秒周期で非ブロッキングに取得し、V5フォーマットのFRAMおよびCSVへ記録します。

## 2. 確認事項 (User Review Required)

> [!WARNING]
> **FRAMフォーマット移行 (V4からV5へ)**
> 新しいファームウェアではFRAMのフォーマットバージョンをV5に引き上げます。
> 移行時、未フラッシュのV4レコードが残っている場合は、自動での初期化を行わず「Degraded」状態となります。
> **新しいファームウェアを書き込む前に、必ず現在のファームウェアでデータをSDカードへフラッシュさせてください。**

> [!IMPORTANT]
> **CSVファイル名の変更**
> V4ヘッダーとの混在を防ぐため、新しいCSVファイルのサフィックスを `_v5` とします（例：`/log_YYYYMMDD_v5.csv`）。

## 3. 変更内容

---

### 依存関係

#### [MODIFY] `platformio.ini`
- Bosch公式の `BME690_SensorAPI` (バージョン `v1.1.0` 固定) を `lib_deps` に追加します。

---

### データ構造

#### [MODIFY] `include/core/sensor_types.h`
- `SensorId::Bme690` を追加。
- `Bme690Data` 構造体を追加（温度、湿度、気圧、ガス抵抗、ステータスなどのフィールドを保持）。

#### [MODIFY] `include/core/sensor_snapshot.h`
- `SensorSnapshot` に `Bme690Data bme690` を追加。
- `SystemStatus` に `DeviceState bme690State` を追加。

---

### センサードライバー

#### [NEW] `include/drivers/sensors/bme690_sensor.h`
#### [NEW] `src/drivers/sensors/bme690_sensor.cpp`
- `ISensor` を継承した `Bme690Sensor` クラスを新規作成。
- 既存の `Wire` と `hal::I2cLockGuard` を使用して独自のI2Cコールバックを実装。
- 5秒周期の非ブロッキング強制モード（forced-mode）状態機械を実装。
- 測定値の有効性判定（`GASM_VALID`, `HEAT_STAB` 等）およびNaN/無効値ハンドリングを実装。

---

### サービス層

#### [MODIFY] `include/services/sensor_manager.h`
#### [MODIFY] `src/services/sensor_manager.cpp`
- `SensorManager` に `bme690_` メンバーを追加し、`begin()` で独立して初期化。
- `update()` ループ内で BME690 の状態機械を進める。
- `snapshot_.bme690` へのデータ反映。BME690は他の代表環境データ（SHT45やBMP581）を上書きしません。

---

### ストレージ層

#### [MODIFY] `include/storage/storage_records.h`
- `FRAM_FORMAT_VERSION` を `5` に変更。
- `SensorRecordV4` および `PersistentRecordV4` を `V5` にリネーム（または置換）し、BME690の各フィールド（計18バイト）を追加。
- `SensorValidFlags` に `VALID_BME690_TPH` と `VALID_BME690_GAS` を追加。
- `RECORD_SLOT_SIZE` は 128 バイトのままであるため、追加後も収まることを `static_assert` で担保。

#### [MODIFY] `src/storage/storage_manager.cpp`
- `loadSuperblock()` で旧バージョン（V4）を検出し、保留レコード数（pending）を確認する処理を実装。pendingが0ならV5へ移行。
- `formatCsvLine()` で使用するバッファサイズを 512 バイトへ拡張。
- CSVヘッダーおよびデータ行にBME690専用の列（温度、湿度、気圧、ガス抵抗、各ステータス等）を追加。
- CSVファイル名に `_v5` を付与して旧ファイルへの追記を防止。

#### [MODIFY] `src/storage/fram_storage.cpp`
- エラーログ出力時のスキャン範囲の表記を `0x50 - 0x51` に修正。

---

### ドキュメント・テスト

#### [MODIFY] `README.md`
- BME690の追加とアドレス(`0x76`)、CSVフォーマットの変更点を追記。

#### [NEW] `test/test_bme690_logic/test_bme690_logic.cpp` (等)
- BME690のステータス遷移やCSV出力のための有効性判定ロジックの native テストを追加。

## 4. 検証計画

- `pio test -e native` を実行し、既存テストと新規BME690ロジックテストがパスすることを確認します。
- ビルドを行い、コンパイルエラーや静的アサート（128バイト制限）に引っかからないことを確認します。
- （ユーザーによる実機検証）I2Cスキャンで `0x76` が認識されること、CSVが `_v5` として保存されデータが正常に記録されること、異常時（未接続等）に他センサーが停止しないことを確認いただきます。
