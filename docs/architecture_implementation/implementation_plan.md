# XIAO ESP32S3 アーキテクチャ実装計画 (指の圧力・波形品質検知 - アプローチA 改良版)

## 目的
ソフトウェア（波形解析）を用いて、MAX30102の測定中の「指の押し付け圧」が不適切であることを検知し、ユーザーが圧力を変更した際に「自動でLED電流を再キャリブレーションする」自己修復システム（アプローチA・改良版）を実装します。

## User Review Required
> [!IMPORTANT]
> ソフトウェアのみで圧力調整とキャリブレーションの矛盾を解決するためのロジック（連続DC監視と再キャリブレーション）をご提案します。この内容で進めてよろしいでしょうか？

## Proposed Changes (自己修復型キャリブレーションと振幅判定)

### 1. DCベースラインの連続監視と自動再キャリブレーション
#### [MODIFY] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- 現在は `NoFinger` 状態から指を乗せた最初の1回しかキャリブレーションを行っていません。
- **改良**: `Measuring`（測定中）状態であっても、毎サンプルの IR 値（DCベースライン）を監視します。
- ユーザーが押し付け圧を変えた結果、IR値が `50,000 未満` または `150,000 超` にズレた場合、直ちに `Measuring` を中断して `Calibrating` 状態に移行します。
- これにより、「圧を変える → ベースラインが崩れる → 自動でLED再調整 → 適正な光量で測定再開」というループが完成します。

### 2. AC振幅の適正閾値の修正
#### [MODIFY] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- ログでいただいた数値を元に、適正なAC振幅の範囲を調整します。
- 振幅が `1,000 未満`（弱すぎる/強すぎて血流停止）または `15,000 超`（ブレすぎ/ノイズ）の場合に `signalPoor = true` とします。

## Verification Plan
1. 指を乗せると通常通りキャリブレーションが走り、測定が開始される。
2. その状態で指の押し付け圧を大きく変える（強くする、弱くする）。
3. 圧を変えた瞬間にシリアルログで `Calibrating` が再実行されることを確認する。
4. 圧が極端に悪い場合は `Weak Sig / Adjust Prs` の警告が出続け、適正な圧になると数値（HR/SpO2）が表示されることを確認する。
