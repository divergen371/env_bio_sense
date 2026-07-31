# XIAO ESP32S3 アーキテクチャ実装計画 (Step 9: 独自DSPアルゴリズムの完全自作)

## 目的
外部ライブラリ（SparkFunやAdvancedOximeter）のアルゴリズムに依存せず、XIAO ESP32S3 の処理能力をフルに活かした**独自DSP（デジタル信号処理）ベースの心拍・SpO2解析アルゴリズム**を完全に自作します。

## 経緯（なぜ自作するのか）
当初は「Phase 1」として最新ライブラリ `jaikulk14/AdvancedOximeter` への乗り換えを予定していましたが、ソースコード（C++実装）を解析した結果、以下の致命的な問題が発見されました。
- ピーク検出のロジックに数学的なバグがあり（最大値ではなく最小値を判定してしまっている等）、まともに心拍を拾えない可能性が高い。
- 不応期（Refractory Period）の概念がなく、重複波のダブルカウント問題が解決されていない。

したがって、「他人の不完全なアルゴリズム」に頼るのをやめ、**SparkFunの優秀なI2Cセンサ制御部分（ハードウェアドライバ）だけを利用し、計算エンジン（ソフトウェアDSP）は全て自前で実装する（Phase 2へ直行する）** 方針に変更します。

## Proposed Changes

### 1. `platformio.ini` の巻き戻し
- `AdvancedOximeter` を削除し、再度 `SparkFun MAX3010x` ライブラリを利用するように戻します。

### 2. 段階的DSPエンジンの新設 (アジャイル開発)
#### [NEW] `include/utils/pulse_analyzer.h`
#### [NEW] `src/utils/pulse_analyzer.cpp`

技術検討書 (`dsp_algorithm_study.md`) の結論に基づき、バグの切り分けを容易にするため以下のステップで段階的に実装します。

**【Step A】 基本DSP ＋ 不応期付きピーク検出（←今回実装）**
- **IIR DCオフセット除去フィルタ**: 生データから数万レベルのベースライン変動を取り除き、0中心のAC成分（脈波）のみを抽出。
- **ローパスフィルタ**: 高周波ノイズを除去して波形を滑らかにする。
- **【最重要】不応期付きピーク検出 (Refractory Peak Detection)**:
  - ゼロクロス（波がプラスからマイナスに変わる点）と極大値を監視してピークを検出。
  - **一度ピークを検出したら、その後 300ms 間は絶対に次のピークを検出しない（ロック機構）。これによりダイクロティックノッチ（重複波）のダブルカウントを100%防止。**
- **Ratio-of-Ratios SpO2計算**: AC成分の振幅（Max - Min）とDC成分を用いて正確なR値を算出し、SpO2に変換。

**【Step B以降】 周波数解析・品質メトリクス（※今後の拡張）**
- Step A が安定稼働した後に、FFT/自己相関による周波数ベースの心拍抽出や、信号品質メトリクス（SQI）の実装を追加予定です。

### 3. MAX30102ドライバの改修
#### [MODIFY] [include/drivers/sensors/max30102_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/max30102_sensor.h)
#### [MODIFY] [src/drivers/sensors/max30102_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/max30102_sensor.cpp)
- SparkFun標準の `maxim_heart_rate_and_oxygen_saturation` 関数を完全に捨てます。
- `update()` ループ内でFIFOから生データを読み出すごとに、自作の `PulseAnalyzer` クラスにデータを流し込み、常に最新の綺麗なHR/SpO2を取得するストリーム処理に書き換えます。

## Verification Plan
1. `platformio.ini` を元に戻し、ビルドして書き込む。
2. 指を乗せ、シリアルモニタで HR が 60〜90 BPM の正しい範囲に収まるか確認する。
3. 以前のように `Amp` が十分あるのに HR が 120 などに跳ね上がる現象（ダブルカウント）が完全に消滅していることを確認する。
