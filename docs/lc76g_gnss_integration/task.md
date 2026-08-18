# LC76G GNSS統合 タスクリスト

## Phase 0: 配線・生NMEA確認
- [x] 115200 bps / 9600 bpsでの生NMEA受信確認
- [x] 採用ボーレートの決定と記録
- [x] GGA/RMCの連続受信確認

## Phase 1: GNSSドライバ
- [x] `platformio.ini` に TinyGPSPlus 追加
- [x] `pins.h` のピン名修正
- [x] `sensor_types.h` / `sensor_snapshot.h` に GnssData/GnssStatus 追加
- [x] UART受信・パース・鮮度判定の実装
- [x] SensorManager とスナップショットへの統合

## Phase 2: PPS・時刻同期
- [x] PPS ISRの実装 (timestampとcounter保存のみ)
- [x] PPSイベント受け渡し処理
- [x] NMEA UTCとの秒対応確認ロジック
- [x] monotonic ↔ UTC アンカー変換処理
- [x] GNSS / NTP / holdover 状態遷移ロジック

## Phase 3: FRAM・SDログ
- [x] FRAM v4レコードの定義 (128バイトスロット)
- [x] v3からv4への移行処理 (未flushデータ保護・校正値保持)
- [x] CSV列の追加 (GNSS関連、UTC等)
- [x] UTCをレコードへ直接保存する処理

## Phase 4: 表示・運用統合
- [x] Overview画面へのGNSS状態追加
- [x] GNSS詳細表示画面の追加
- [x] NTPフォールバック設定への変更
- [x] 起動・障害診断ログの整備

## Phase 5: 総合試験
- [x] 自動テストの追加 (Nativeテスト含む)
- [x] 屋外Fix試験 (実機)
- [x] PPS 1000周期の集計と精度確認
- [x] 切断・復帰の耐障害性テスト
- [x] SD・表示・PPGとの同時動作負荷テスト
