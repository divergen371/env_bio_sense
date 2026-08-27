# 修正内容の確認 (Walkthrough)

## 実装内容
BME690の追加実装が完了しました。

1. **BME690Sensorドライバーの実装**
   - [NEW] `include/drivers/sensors/bme690_sensor.h`
   - [NEW] `src/drivers/sensors/bme690_sensor.cpp`
   - Boschの公式API(`BME690_SensorAPI`)を利用し、I2Cアクセスおよび非ブロッキング状態機械による測定処理を実装しました。

2. **SensorManagerの更新**
   - [MODIFY] `include/services/sensor_manager.h`
   - [MODIFY] `src/services/sensor_manager.cpp`
   - `Bme690Sensor`インスタンスを追加し、初期化および頻繁なポーリング更新 (`update()`)、スナップショットへのデータ格納を実装しました。

3. **ストレージ形式のV5への移行とCSV保存**
   - [MODIFY] `include/storage/storage_records.h`
   - [MODIFY] `src/storage/storage_manager.cpp`
   - `FRAM_FORMAT_VERSION` を 5 に引き上げ、`PersistentRecordV5` にBME690のデータ（温度・湿度・気圧・ガス抵抗・ガスインデックス・ステータス）を追加しました。
   - 既存のV4フォーマットのFRAM未フラッシュ分が残っている場合は移行を中断するフェイルセーフ処理を追加しました。
   - V5としてCSVファイルに書き込む際のヘッダーと出力フォーマットを更新し、ファイル名のサフィックスを `_v5.csv` としました。

## 次のステップ
コードの実装および修正が完了しました。
なお、AIからコンパイルを試行しましたが、Python環境(encodingsモジュール関連)のエラーが発生したためビルド確認が行えていません。
実際の環境でのビルド（コンパイル）および実機への書き込み、テストの実行をお願いいたします。
