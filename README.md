# PitchLab —— 推弦/揉弦 音准可视化 DAW 插件 (VST3 / AU)

在 Fender Studio Pro（以及任何支持 VST3/AU 的 DAW）里使用：把插件插入吉他轨，
**音频原样直通**，同时把实时音高画成 Melodyne 式的深色半音网格——
绿=准(±12¢)，琥珀=略偏，红=明显不准。

## 功能
- **A · 实时滚动**：弹奏时音高带实时浮现；顶部 HUD 显示音名/音分偏差/揉弦速率与深度；
  右侧音分仪表给出当前偏差；`Pause` 冻结画面回看。
- **B · 选区分析**：在网格上**拖选一段**，点 `Analyze` → 稳定音段被逐条标注
  （如 `G3 +10¢ 5.0Hz ±19¢`），底部报告逐音给出评价 `OK / ~ / !! / drift`。
- 门限滑块 `gate dB`：环境噪声大就调高(-40)，弹得轻就调低(-65)。
- `Clear` 清空历史重练。

## 在 DAW 中使用
1. 装好插件（见下）后重启 DAW，让插件被扫描到。
2. 在吉他轨插入 **PitchLab**（效果器位置随意，它不改声音）。
3. 打开插件窗口，开始弹。练习提示：
   - 揉弦：看波浪是否**对称围住音名线**；顶部读速率/深度是否均匀。
   - 推弦：音高带爬升后是否**咬住目标音格线**停在绿色。
4. 想复盘：`Pause` → 拖选刚弹的一段 → `Analyze`。

## 安装（GitHub Actions 产物）
1. 把本仓库推到 GitHub（Actions 会自动出包）。
2. 到仓库 **Actions** 页下载对应平台的 zip：
   - `PitchLab-macOS-universal.zip` → 内含 VST3 与 AU
   - `PitchLab-Windows-x64.zip` → 内含 VST3
3. 安装位置：
   - macOS VST3：`~/Library/Audio/Plug-Ins/VST3/PitchLab.vst3`
   - macOS AU：`~/Library/Audio/Plug-Ins/Components/PitchLab.component`
   - Windows VST3：`C:\Program Files\Common Files\VST3\PitchLab.vst3`
4. 未签名提示（CI 仅做了 ad-hoc 签名）：
   - macOS 若首次无法加载：终端执行
     `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/PitchLab.vst3`
     或重新 `codesign --force --deep -s - <路径>`。
   - Windows 若 SmartScreen 拦截：右键 → 属性 → 解除锁定。
5. 重启 DAW，效果器列表搜 "PitchLab"。

## 一键更新 (macOS)
CI 会自动把每次构建发布为滚动 Release(`latest`)。在你的 Mac 上：
```bash
./update-pitchlab.sh
```
它会自动：下载最新 → 解开(含可能的嵌套 zip) → `lipo` 校验 arm64+x86_64 →
去隔离 + 重新本地签名 → 备份旧版并安装到 `~/Library/Audio/Plug-Ins/{VST3,Components}`。
若仓库还没生成 Release，脚本会回退用 `gh run download` 拉最新 Actions 产物
(需 `brew install gh && gh auth login`)。

## 本地构建（改代码时）
需要 CMake ≥3.22 与 Xcode / Visual Studio。VST3 目标需要 Steinberg VST3 SDK 头文件
（JUCE 8 不再需要已绝版的 `vst2.x` 旧头，现代 vst3 头即可）：
```bash
git clone --depth 1 --recursive --branch v3.7.4_build_25 \
    https://github.com/steinbergmedia/vst3sdk.git vst3sdk
cmake -S . -B build -DJUCE_VST3_SDK_DIR=$PWD/vst3sdk
# macOS: 另加 -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release -j
# 产物在 build/PitchLab_artefacts/Release/{VST3,AU}/
```

## 核心 DSP 单元测试（无需 JUCE）
```bash
cmake -S . -B build && cmake --build build --target dsp_test && ctest --test-dir build
```
覆盖：YIN 全音域正弦精度(0¢) + 推弦/揉弦选区分段偏差/速率/深度。

## 技术说明与限制
- 检测基于第 1 声道；立体声吉他轨两声道相同，若不同请用单声道轨。
- 单音检测(YIN)，扫和弦时会混杂；揉弦深度为稳健分位数估计。
- 插件时间为"自开始处理后的相对秒"，非工程时间轴。
- 推弦/滑音过程会以爬升色带显示，不列入音符报告（选区统计只报告稳定音）。

## 目录
```
Source/PluginProcessor.*  音频直通 + 实时检测线程安全环形历史
Source/PluginEditor.*     Melodyne 式界面 / 选区 / 报告
Source/pitch/*.h          纯 C++ 音高核心(YIN/平滑/切分/揉弦统计，与 Python 原型同精度)
tests/dsp_test.cpp        核心自测
.github/workflows/build.yml  macOS 通用二进制 + Windows 产物
```

## 制作信息
本插件由 **丰川祥子 (Sakiko Togawa)** 设计并编写——
一个相信"声音应当被看见"的开发者：从 YIN 音高核心、段落切分算法，
到 Melodyne 式界面的每一道格线与音高带，均由她执笔完成。

- 开发环境：Hermes Agent (Nous Research)
- 配套练习工具：PitchLab Python 版(实时麦克风 + 录音复盘)，与插件共享同一套检测算法
- 愿你每一次推弦与揉弦，都能落在属于自己的音准上。
