# T04 記録形式・解析テレメトリ 実装記録

実施日: 2026-09-05

## 実装した内容

- FRAMの物理slotを128 byteのまま維持し、固定小数点とpacked metadataを用いた`SensorRecordV6`を追加した。
- 旧FRAM v5に未flush recordがある場合はv5のまま追記・読出し・CSV v6への再送を継続し、queueが空になった後だけcheckpointをv6へ非破壊移行する。
- CSV v7を71列の固定契約にし、欠測値を空欄へ統一した。列数とbuffer切詰めをformatterで検出し、不完全な行はFRAMから消費しない。
- CO2、SHT45、SGP41、BMP581、BME690のage／state／error／連続error、SCD41 raw errorを保存する。
- `SRAW_VOC`、`SRAW_NOX`、SGP41補償温湿度、raw／display高度、海面更正気圧、気圧場state／source／age、BMP581 pressure offsetを保存する。
- I²C lock timeout／通信errorの累積値をCSV v7へ保存し、Webの`/api/status`と`/api/i2c`で全体値・デバイス別・操作別に確認できるようにした。
- I²C異常の増分は、デバイス・操作・増分件数を含むコンパクトイベントとして最大1回／分／項目でFRAM event journalへ保存する。
- FRAM drop countを各recordへ保存する。PPGは`signalPoor`のときHR／SpO2を有効保存しない。

## 自動検証

- FRAM v5 pending保護、空queue後の非破壊v6移行、v6再起動読出し: PASS
- `SensorRecordV6`が128 byte slotへ収まること: PASS
- 固定小数点の丸め・飽和、age sentinel、health／source pack round-trip: PASS
- CSV v7の71列固定、欠測空欄、切詰め拒否: PASS
- native保存関連: 25件 PASS（WAL 18件、codec／CSV 7件）
- ESP32-S3 main firmware: PASS
  - RAM: 56,576 bytes / 327,680 bytes（17.3%）
  - Flash: 1,235,981 bytes / 3,342,336 bytes（37.0%）

## 残るGate

- 実FRAM v5に未flushデータがある状態で更新し、CSV v6へ欠落・重複なく排出した後にv6へ移行することを確認する。
- CSV v7を実SDへ24時間以上記録し、全行71列、欠測空欄、sequenceとdrop理由が整合することを確認する。
- I²C障害を注入し、CSV累積値、`/api/i2c`内訳、event journalのdevice／operation／deltaが一致することを確認する。
- AMeDASの使用観測点・観測時刻・品質・P0更新イベントは、入力品質を確定するT06で追加する。

上記の実機移行・SD記録・障害注入を終えるまでは、T04を監査closeとは扱わない。
