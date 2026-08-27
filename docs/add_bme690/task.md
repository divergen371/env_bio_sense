# タスクリスト: BME690の追加

- [x] `platformio.ini` の更新
- [x] `include/core/sensor_types.h` の更新
- [x] `include/core/sensor_snapshot.h` の更新
- [x] `include/drivers/sensors/bme690_sensor.h` および `src/drivers/sensors/bme690_sensor.cpp` を作成する（非ブロッキングなI2Cポーリング処理とステートマシン）。
- [x] `include/services/sensor_manager.h` および `src/services/sensor_manager.cpp` を更新して BME690 を組み込み、スナップショットへデータを反映する。
- [x] `include/storage/storage_records.h` を更新して FRAM フォーマットバージョンを V5 にし、`SensorRecordV5` と `PersistentRecordV5` を作成、フラグ定数を追加する。
- [x] `src/storage/storage_manager.cpp` を更新して、V4からV5へのマイグレーション処理、V5フォーマットによるFRAM/SD記録、CSVフォーマットにBME690フィールドを追加する（サフィックス `_v5`）。の作成と実行
- [x] nativeテスト（`test/test_bme690_logic/`）の作成と実行
- [x] `README.md` の更新
