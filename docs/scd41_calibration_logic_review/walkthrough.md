# SCD41 Calibration Logic Update Walkthrough

SCD41のキャリブレーションロジックの修正が完了しました。主な変更内容は以下の通りです。

## 1. ASC（自動自己校正）の有効化とデフォルト化
- `scd41_sensor.cpp` において、`configure()` 時に `setAutomaticSelfCalibration(1)` を呼び出し、ASCをデフォルトで有効化するように変更しました。

## 2. FRC（手動校正）APIの改修と安全性向上
- 従来 `performManualCalibration` だったメソッドを `performForcedRecalibration` に変更しました。
- 以下の事前条件チェックを追加しました:
  - センサー起動後3分間 (`180,000ms`) 経過していること
  - 初回有効データが受信済みであること
  - 気圧データ（大気圧）が直近15秒以内に設定されていること
  - 基準値 (`reference_ppm`) が `400` 〜 `5000` の範囲内であること
- FRC実行中のI2Cアクセスの競合を防ぐため、`calibrationInProgress_` フラグによる排他制御を導入しました。この間、`update()` 呼び出しは即座にリターンします。
- 生の取得データ（`rawWord`）から補正値へのデコード処理（`rawWord - 0x8000`）を適切に行い、結果を `Scd41FrcResult` 構造体に格納するよう実装しました。

## 3. Web UI / API の変更
- `web_server_service.cpp` において、「7日間校正が行われていない」場合の警告バッジ表示を完全に削除しました（`SensorManager` からの該当ロジックも削除済み）。
- 手動校正 (FRC) 用の UI を「高度な設定 / 外部基準がある場合のみ使用」という位置付けに変更し、デフォルトの `400ppm` 入力を削除しました。
- FRCを実行するためには、「外部の信頼できる基準器の値であること」を確認するチェックボックス (`confirm_external_reference`) のチェックを必須としました。
- FRC完了時、補正値（ppm）と生の16ビットワードなどをJSONレスポンスで返すようにAPI（`/api/scd41/calibrate`）を改修しました。

## 4. Factory Reset の UI/API 追加 と ログ拡充
- 指示書にあった「工場出荷状態（Factory Reset）の Advanced / Maintenance 操作への追加」を実装しました。Web UI に Factory Reset ボタンを追加し、実行には確認チェックボックスを必須としました（API: `/api/scd41/factory_reset`）。
- リセット直前に、リセット前の設定値（TempOffset と ASC）をログへ記録する処理を追加しました。
- FRC 失敗時の詳細なエラー理由（例: `MEASUREMENT_UPTIME_TOO_SHORT`, `PRESSURE_STALE`, `REFERENCE_OUT_OF_RANGE` 等）を Web API の JSON レスポンスに含めるようにしました。
- FRC 成功直後の数サンプルについて、`event=SCD41_POST_FRC` をログ出力する機能を追加しました。

## 5. テストコードの追加
- FRCの補正値デコード（0xFFFFの失敗扱い、0x7FCEの負の値変換など）や、3分以上の稼働時間、気圧データの鮮度などの事前条件ロジックを検証するユニットテストを `test/test_scd41_logic/test_scd41_logic.cpp` に追加しました。

## 6. 次のステップ
- **ビルドと実機検証**: 環境の依存関係によりサンドボックス内での `pio` ビルドが失敗したため、ユーザー側の環境で `pio run -e seeed_xiao_esp32s3` によるビルドと、実機への書き込みをお願いします。
- Web UI から新しいキャリブレーションインターフェースを確認し、チェックボックスなしでの送信が拒否されることなどを確認してください。
- 蓄積された履歴を消去したい場合は、Web UI の Advanced セクションから Factory Reset を実行してください。
