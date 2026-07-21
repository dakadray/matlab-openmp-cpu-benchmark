# MATLAB + C++/OpenMP/MEX CPU Benchmark

这个小项目用于比较不同桌面 CPU 在一个结构有限元风格工作流里的实际表现，目标机器包括：

- Intel Core i9-12900K
- Intel Core Ultra 9 285K
- AMD Ryzen 9 9950X

核心分工：

- MATLAB 负责构建 benchmark 流程、施加固定边界、调用 `Kff \ Ff` 解线性方程，并分别记录求解时间。
- C++ MEX 负责 3D 八节点实体单元刚度矩阵计算、OpenMP 并行单元循环、全局稀疏刚度矩阵装配，并把刚度计算/装配分段计时传回 MATLAB。
- 默认使用 `Threads="all"`，会把 OpenMP 线程数设置成系统检测到的逻辑处理器数量，让 Intel 大小核和 AMD 全部逻辑线程都参与 MEX 刚度矩阵阶段。

## 机器准备

需要安装：

1. MATLAB，建议 R2020b 或更新版本。
2. MATLAB 可用的 C++ MEX 编译器。
   - Windows: Visual Studio Build Tools 或 MinGW-w64。
   - Linux: `g++`。
3. Git，用于在远程租用电脑上下载代码。

在 MATLAB 里先确认编译器：

```matlab
mex -setup C++
```

为了让大小核都尽量参与，建议测试前：

- Windows 电源模式设为“最佳性能”或“高性能”。
- BIOS 中确认 E-core/P-core 都启用。
- 测试时不要同时运行大型后台任务。
- 三台机器使用同一 MATLAB 版本、同一 benchmark profile 和同一电源设置。

## 一键运行

在 MATLAB 当前目录切到本项目根目录，然后按机器选择入口。

本地 Y9000P 跑常规组，不跑 `crazy`。可以分别跑 P-only 和全核：

```matlab
runCpuBenchmark_Y9000P_P    % Intel hybrid: P-core only
runCpuBenchmark_Y9000P_all  % Intel P+E all-core
```

旧入口 `runCpuBenchmark_Y9000P` 仍然可用，等价于 `runCpuBenchmark_Y9000P_all`。

32 GB 租用服务器跑全部组，包括 32 GB-oriented `crazy`。Intel 大小核机器可以分别跑 P-only 和全核；AMD 机器直接跑全核：

```matlab
runCpuBenchmark_Server_P    % Intel hybrid: P-core only
runCpuBenchmark_Server_all  % Intel P+E all-core, or AMD all-core
```

Y9000P 入口会依次运行：

```text
smoke, quick, standard, stress
```

所有 Server 入口都会依次运行：

```text
smoke, quick, standard, stress, crazy
```

所有入口都固定每个规模重复 3 次。服务器入口只保留 `runCpuBenchmark_Server_P` 和 `runCpuBenchmark_Server_all`，本地入口保留 `runCpuBenchmark_Y9000P_P` 和 `runCpuBenchmark_Y9000P_all`；旧的 `runCpuBenchmark_Y9000P` 只是全核别名。

`runCpuBenchmark_Server_P` 和 `runCpuBenchmark_Y9000P_P` 会在 Windows 上用 CPU Set `EfficiencyClass` 识别 Intel P-core，并把 MATLAB 进程限制到最高 `EfficiencyClass` 的逻辑处理器；如果系统不是可识别的 Intel 大小核平台，它会报错而不是静默跑全核。

如果 `runCpuBenchmark_Server_all` 显示的 `Selected logical processors` 明显少于任务管理器里的逻辑处理器数量，说明 MATLAB 进程仍被 Windows、租机平台或任务管理器 affinity 限制。先确认已经 `git pull` 到最新版本；如果最新版本仍然如此，需要在系统或平台层解除进程 affinity 限制。

Windows 可能把空闲 P-core 标记为 parked。benchmark 会把 parked processors 也纳入 P-only/全核选择，并在 `parkedFlags`、`pParkedFlags` 中返回诊断信息；否则会错误地只测到当前未停放的那一小部分核心。

或直接指定：

```matlab
bench = benchmarkOpenMpCpu('Profile', 'standard', 'Threads', 'all');
```

第一次运行会自动编译：

```matlab
buildCpuBenchmarkMex
```

结果会保存在：

```text
results/*_detail.csv
results/*_summary.csv
results/*.mat
```

最重要的列：

- `meanStiffnessSeconds`: C++ MEX 刚度矩阵计算 + 稀疏装配平均时间。
- `meanElementSeconds`: C++ OpenMP 单元刚度矩阵计算和 triplet 写入平均时间。
- `meanSortSeconds`: C++ 稀疏装配前的 triplet 排序平均时间。
- `meanSolveSeconds`: MATLAB `Kff \ Ff` 解方程平均时间。
- `meanTotalSeconds`: 刚度矩阵阶段 + 约束切片 + 解方程阶段平均时间。
- `actualMexThreads`: MEX 实际 OpenMP 并行区线程数。
- `meanRelativeResidual`: 解方程平均相对残差，正常应在 `1e-8` 以下。

CSV 中仍保留 `median*` 列，方便以后排查异常波动；默认购买决策先看 `mean*`。

## 规模选择

默认 profile:

```matlab
benchmarkOpenMpCpu('Profile', 'standard', 'Threads', 'all')
```

包含网格：

```text
8x8x8, 12x12x12, 16x16x16, 20x20x20
```

`stress` profile 包含四组大规模样本，用于正式购买决策时单独压测：

| 网格规模 | 单元数 | 全局自由度 | 求解自由度 | C++ 原始 triplet 数 |
|---:|---:|---:|---:|---:|
| `25 x 25 x 25` | 15,625 | 52,728 | 50,700 | 9,000,000 |
| `30 x 30 x 30` | 27,000 | 89,373 | 86,490 | 15,552,000 |
| `35 x 35 x 35` | 42,875 | 139,968 | 136,080 | 24,696,000 |
| `40 x 40 x 40` | 64,000 | 206,763 | 201,720 | 36,864,000 |

`40 x 40 x 40` 会明显考验内存、散热和 MATLAB 稀疏直接求解性能，建议在三台候选 CPU 上都使用相同电源策略和相同 MATLAB 版本。

`crazy` profile 面向 32 GB 租用服务器，进一步拉高内存和求解器压力：

| 网格规模 | 单元数 | 全局自由度 | 求解自由度 | C++ 原始 triplet 数 |
|---:|---:|---:|---:|---:|
| `42 x 42 x 42` | 74,088 | 238,521 | 232,974 | 42,674,688 |
| `44 x 44 x 44` | 85,184 | 273,375 | 267,300 | 49,065,984 |
| `46 x 46 x 46` | 97,336 | 311,469 | 304,842 | 56,065,536 |
| `48 x 48 x 48` | 110,592 | 352,947 | 345,744 | 63,700,992 |
| `50 x 50 x 50` | 125,000 | 397,953 | 390,150 | 72,000,000 |

`crazy` 默认只在服务器入口中启用。

可选 profile:

```matlab
benchmarkOpenMpCpu('Profile', 'smoke', 'Threads', 'all')    % 编译和冒烟测试
benchmarkOpenMpCpu('Profile', 'quick', 'Threads', 'all')    % 较快比较
benchmarkOpenMpCpu('Profile', 'standard', 'Threads', 'all') % 推荐正式比较
benchmarkOpenMpCpu('Profile', 'stress', 'Threads', 'all')   % 更重，注意内存
benchmarkOpenMpCpu('Profile', 'crazy', 'Threads', 'all')    % 32 GB 服务器压测
```

也可以手动指定规模：

```matlab
bench = benchmarkOpenMpCpu( ...
    'Sizes', [8 8 8; 12 12 12; 16 16 16; 20 20 20; ...
        25 25 25; 30 30 30; 35 35 35; 40 40 40], ...
    'Repeats', 3, ...
    'Threads', 'all');
```

## 三台机器怎么比较

本地机器运行：

```matlab
runCpuBenchmark_Y9000P_P
runCpuBenchmark_Y9000P_all
```

32 GB 租用服务器运行：

```matlab
runCpuBenchmark_Server_P
runCpuBenchmark_Server_all
```

把三台机器的 `results/*_summary.csv` 放到同一个 `results` 目录后运行：

```matlab
compareCpuBenchmarkResults
```

它会按规模列出：

- 刚度矩阵阶段耗时
- MATLAB 解方程耗时
- 两者合计
- 同规模下相对最快机器的比例

## 远程租用电脑下载

如果这个仓库发布为公开 GitHub 仓库，远程机器不需要登录 GitHub，直接：

```bash
git clone https://github.com/dakadray/matlab-openmp-cpu-benchmark.git
```

然后在 MATLAB 里进入仓库目录运行：

```matlab
runCpuBenchmark_Server_P    % Intel 仅大核
runCpuBenchmark_Server_all  % Intel 大小核全开 / AMD 全核
```

## 说明

这个 benchmark 不是 SPEC，也不是纯理论峰值测试。它更接近你关心的工作负载：

- 有大量小型单元矩阵计算。
- 有全局稀疏矩阵装配。
- 有 MATLAB 稀疏直接求解。
- 可以观察 Intel 大小核调度、OpenMP 线程扩展和 MATLAB 求解器阶段的综合差异。

因此结果适合作为购买工作站/台式机时的直接参考之一。正式决策时建议每台机器至少跑两遍 `standard`，确认温度、功耗和后台任务没有导致明显波动。
