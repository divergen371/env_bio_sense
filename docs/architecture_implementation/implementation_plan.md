# XIAO ESP32S3 アーキテクチャ実装計画 (Step 9: 独自DSPアルゴリズムの完全自作)

## 目的
外部ライブラリ（SparkFunやAdvancedOximeter）のアルゴリズムに依存せず、XIAO ESP32S3 の処理能力をフルに活かした**独自DSP（デジタル信号処理）ベースの心拍・SpO2解析アルゴリズム**を完全に自作・高度化します。

---

## 経緯・実装進捗

- **【Step A】基本DSP ＋ 不応期付きピーク検出（実装・実機検証完了）**
  - 高速DCトラッキング（$\alpha = 0.1 \rightarrow 0.01$）、実効100Hzローパスフィルタ
  - ゼロクロス・ダウン絶対再武装 ＋ プラス領域・ヒステリシス評価による堅牢なピーク検出
  - Maxim公式二次多項式によるSpO2算出（実機で HR=61〜68 bpm, SpO2=95〜96% を確認）
  - 25Hz/100Hzネイティブ単体テストの全ケースパス

---

## Proposed Changes

### [Step B] 信号品質評価 (SQI) ＆ 周期領域解析 (スライディングDPT) の実装（次フェーズ）

Analog Devices / Maxim Integrated の最新論文 **"新たな離散周期変換手法によって生理学的な信号を処理する (Issue 230)"** および **"Guidelines for SpO2 Measurement"** ガイドラインに基づき、アルゴリズムを高度化します。

#### 1. スライディングDPT (Sliding Discrete Period Transform) による周期領域解析
- **概念とメリット**:
  - 生体信号の非定常性（心拍周期のリアルタイム変動）に対して通常のFFT（周波数インクリメント）をかけるとスペクトルがぼやける課題を解決。
  - 周波数ではなく「周期（サンプル周期の倍数）」を直接インクリメントして解析する **DPT（離散周期変換）** を採用。
  - サンプル到着ごとに10秒分（1000サンプル）の循環バッファ上でスライディング更新（コムフィルタ＋複素共振器構造）を行うことで、少ないサンプル数かつリアルタイム（低遅延・低メモリ）で高精度な周期スペクトルを抽出。
- **BPM/SpO2の直接抽出**:
  - スペクトル上の主周期ピーク位置から直接心拍数（BPM）を導出。
  - 赤色光/赤外光の周期スペクトルピーク振幅比から安定した $R$ 比を算出。

#### 2. PI（灌流指数: Perfusion Index）による信号品質判定 (SQI)
- **計算式**:
  $$PI_{\text{IR}} = \left( \frac{AC_{\text{IR}}}{DC_{\text{IR}}} \right) \times 100\%$$
- **判定基準**:
  - $PI < 0.2\%$ の場合、末梢血流低下・指の浮き・過度圧迫と判定し、**SpO2およびHRの更新を安全に停止**してノイズによる異常値の混入をブロック。

#### 3. $R < 0.4$ 時のクランプ処理（多項式の頂点反転対策）
- Maxim公式二次多項式 $SpO_2 = -45.060 \times R^2 + 30.354 \times R + 94.845$ の $R \approx 0.337$ 付近での頂点折り返しを防ぐため、$R < 0.4$ 領域では **強制的かつ安全に $SpO_2 = 100.0\%$ へクランプ**。

#### 4. 二次多項式キャリブレーション方程式の厳密な範囲制限
- $R$ 比の有効範囲を $0.02 \le R \le 1.84$ に制限し、体動ノイズや瞬間的な光漏れによる範囲外データを棄却。

---

## 変更対象ファイル

#### [MODIFY] [pulse_analyzer.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/utils/pulse_analyzer.h)
#### [MODIFY] [pulse_analyzer.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/utils/pulse_analyzer.cpp)
#### [MODIFY] [test_pulse_analyzer.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/test/test_pulse_analyzer/test_pulse_analyzer.cpp)

---

## Verification Plan

### 1. 単体テスト (`pio test -e native`)
- スライディングDPTによる周期抽出が正弦波・ノッチ混入波で正しく動作すること。
- $PI < 0.2\%$ の低灌流サンプルを入力した際に、SpO2が更新されず保持されること。
- $R = 0.2$ や $0.3$ などの極小値サンプルを入力した際に、$SpO_2 = 100.0\%$ にクランプされること。

### 2. 実機検証 (`pio run -t upload`)
- スライディングDPTによる周期スペクトルの追従とリアルタイム心拍抽出。
- 指を浮かせたり過度に圧迫して $PI$ が低下した際に、誤った低SpO2が出力されないことを確認。
