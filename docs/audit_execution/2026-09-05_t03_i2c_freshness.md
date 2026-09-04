# T03 I²C規約・センサー鮮度 実装記録

実施日: 2026-09-05

## 実装した内容

- 共通I²C lockへデバイス種別と操作種別を付与し、lock timeoutと通信エラーを飽和カウンタで集計するようにした。
- SHT45の初期化、serial取得、通常測定を共通mutex配下へ移した。
- SHT45ヒーターを同期実行から、command送信、センサー内部待機、結果取得、15秒cooldownの非ブロッキング状態機械へ変更した。待機・cooldown中は直前値を有効値として返さない。
- SHT45、SGP41、BMP581、BME690は、通信失敗または規定age超過時に以前の値を有効として返さない。
- SHT45とSGP41は連続3失敗後にRetryWaitへ入り、60秒後に再初期化する。初期化失敗後も恒久停止しない。
- BMP581のI²C読出しで受信長、`Wire.available()`、`Wire.read()`の失敗を検査する。失敗時は即Warning／stale化し、連続3失敗で再初期化する。
- BME690とFRAMの短いI²C read/writeも受信長と通信失敗をデバイス別カウンタへ記録する。

## 自動検証

- SHT4x CRC既知vector: PASS
- SHT4x正常frame decode: PASS
- SHT4x CRC破損拒否: PASS
- SHT4x湿度の物理範囲clamp: PASS
- PlatformIO通常build 8環境: 8/8 PASS
- ESP32-S3 main firmware: PASS
  - RAM: 55,640 bytes / 327,680 bytes（17.0%）
  - Flash: 1,224,681 bytes / 3,342,336 bytes（36.6%）

## 残るGate

- FRAM、SHT45、SCD41、SGP41、BMP581、BME690、MAX30102、OLEDを実機で同時駆動し、ヒーター実行中もSCD41停止、FRAM失敗、PPG FIFO欠落が増えないことを確認する。
- I²C線の一時遮断またはfault injectionで、デバイス別・操作別カウンタ、値の無効化、RetryWait、復帰が一致することを確認する。
- 集計カウンタをCSV v7／Web診断へ公開する処理はT04で行う。
- SGP41の`SRAW_VOC`、`SRAW_NOX`、補償入力の保存はT04で行う。
- SCD41の復旧後隔離と復旧storm抑止はT05で行う。

上記の実機同時競合試験と障害注入を終えるまでは、T03を監査closeとは扱わない。
