# ShmFrameTagger 2.1

ShmFrameTagger 用于 VTD 共享内存图像的**防重复帧**。

它作为第三个写入方，按固定频率在 VTD 写入共享内存的每一帧 RDB 图像的**右上角**写入一个 4 位递增数字（`0000`–`9999`）。这样即使 VTD 卡顿、没有输出新图像，读取方每次读到的图像也都不同，而且可以直接用肉眼确认。

程序没有界面，在后台运行即可。

---

## 1. 包内容

```
ShmFrameTagger-2.1-linux-x86_64/
├── ShmFrameTagger      可执行程序（单文件，无需任何依赖库）
└── README.md           本说明
```

## 2. 系统要求

| 项目 | 要求 |
|---|---|
| 架构 | x86_64 Linux |
| 系统 | glibc ≥ 2.17 即可，例如 Ubuntu 16.04 / 18.04 / 20.04 / 22.04 / 24.04、CentOS 7 及以上 |
| 运行位置 | 与 VTD 的图像生成器（IG）**在同一台机器上**（共享内存只在本机有效） |
| 权限 | 对 VTD 图像共享内存有读写权限（VTD 默认创建的共享内存权限为 777，普通用户即可） |
| 依赖 | 无（不需要 OpenCV，不需要图形界面） |

## 3. 快速开始

```bash
tar xzf ShmFrameTagger-2.1-linux-x86_64.tar.gz
cd ShmFrameTagger-2.1-linux-x86_64
./ShmFrameTagger --rate 400
```

**推荐**（让数字在任何时刻都几乎一直存在）：授予一次实时优先级权限，然后加 `--realtime` 运行：

```bash
sudo setcap cap_sys_nice+ep ./ShmFrameTagger      # 只需执行一次（重新解压或替换程序后需再执行）
./ShmFrameTagger --rate 400 --realtime
```

不授予权限也可以运行，程序会提示并以普通优先级继续工作，只是系统繁忙时数字偶尔会缺失约 1 毫秒。

先启动 VTD 或先启动本程序都可以。程序会自动发现并接管 key 为 `0x8100`–`0x81FF` 的所有图像共享内存；VTD 重启、共享内存被删除或重建时也会自动重新连接。

按 `Ctrl+C` 停止。

## 4. 命令行参数

```
./ShmFrameTagger [--rate <Hz>] [--poll-us <微秒>] [--realtime] [--range <起始key> <结束key>] [--stats <秒>] [key ...]
```

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--rate <Hz>` | `200` | 写入频率，每写入一次数字加 1。读取方的读取频率应**不高于**此频率。 |
| `--poll-us <微秒>` | `50` | 两次写入之间检查 VTD 是否覆盖了数字的间隔。VTD 写入新帧覆盖数字后，程序会立即补画。值越小补画越及时，CPU 占用越高。`0` = 持续检查（占满一个 CPU 核），`-1` = 关闭检查。 |
| `--realtime` | 关闭 | 以实时优先级（SCHED_FIFO 50）运行，保证 VTD 覆盖数字后在几微秒内补画，不受系统负载影响。需要 root 权限或预先执行 `sudo setcap cap_sys_nice+ep ./ShmFrameTagger`；权限不足时给出提示并以普通优先级继续运行。 |
| `--range <起始> <结束>` | `0x8100 0x81ff` | 自动接管的共享内存 key 范围。 |
| `--stats <秒>` | `10` | 状态输出间隔，`0` = 不输出。 |
| `key ...` | 无 | 只处理指定的 key，例如 `./ShmFrameTagger 0x08130 0x08131`。 |

示例：

```bash
./ShmFrameTagger --rate 400 --realtime      # 推荐配置
./ShmFrameTagger 0x08130 0x08131            # 只处理两路
./ShmFrameTagger --poll-us 100              # 降低 CPU 占用
```

## 5. 运行输出

```
ShmFrameTagger: 400 Hz, keys 0x08100-0x081ff, overwrite check every 50 us, real-time priority, Ctrl+C to stop
[0x08130] attached (3110836 bytes)          ← 发现并接管一路共享内存
[0x08131] attached (3110836 bytes)
[stats] 2 segment(s)  digits 3974  400.0 cycles/s  max late 0.30 ms  writes 7948  restored after VTD overwrite 1200
[0x08131] detached                          ← 该共享内存已被删除（例如 VTD 退出）
```

| 字段 | 含义 |
|---|---|
| `segment(s)` | 当前接管的共享内存数量 |
| `digits` | 当前画面上显示的四位数字 |
| `cycles/s` | 实际写入频率 |
| `max late` | 最近一个统计周期内定时的最大延迟 |
| `writes` | 累计写入次数（每个缓冲每次写入计 1 次） |
| `restored after VTD overwrite` | VTD 写新帧覆盖数字后，程序重新补画的次数（约等于 VTD 的出帧数） |

如果某路图像无法写入（例如像素格式不支持），状态输出中会额外显示一行原因。

## 6. 程序写入的内容

- 位置：显示画面的**右上角**，紧贴边缘，大小 **25×9 像素**。
- 内容：数字计数的后四位（`0000`–`9999`），5×7 点阵字体，白色数字加 1 像素黑色描边。
- 程序启动时从 `0000` 开始，每写入一次加 1，到 `9999` 后回到 `0000`。
- 只改写这 25×9 像素：数字以外的像素保持 VTD 原画面，不会留下旧数字残影，也没有底框。
- **不修改**共享内存头、缓冲信息、锁 / 标志位（LOCK / TC / IG），也不等待、不阻塞 VTD。
- VTD 读回的图像在内存中是自下而上存放的，程序已按此处理：在常规查看器（上下翻转后显示）中数字位于右上角，方向为正。
- 仅支持每通道 8 位的像素格式（RGB8、RGBA8、8 位灰度）；其他格式不写入。

## 7. 工作原理

1. 每 0.5 秒扫描一次系统共享内存，接管范围内含有 RDB 图像的共享内存。
2. 按 `--rate` 的频率，在每个缓冲（双缓冲的两个缓冲都处理）的右上角写入新的数字。
3. 两次写入之间，每隔 `--poll-us` 检查一次数字区域：VTD 写入新帧覆盖数字时，把它写入的像素记为新背景，区域稳定（3 微秒内无新写入）后立即补画数字。
   VTD 发布新帧号约 0.2 毫秒后才拷贝像素，因此程序在检测到新帧号后会对该缓冲连续检查（不休眠，最长 5 毫秒），直到数字补画完成，数字缺失的时间只有几微秒。
4. VTD 卡顿、不写新帧时，数字仍按写入频率持续变化，因此读取方连续两次读到的图像不会相同。

## 8. 性能参考

**真实 VTD**（2 路 960×540 RGB8），`--rate 400 --realtime`，以每秒约一千万次的频率连续读取最新一帧的数字区域：

| 指标 | 结果 |
|---|---|
| 读到无数字的比例 | 0.003 % |
| VTD 每次覆盖数字后补画所需时间 | 中位数 5 微秒，最长 13 微秒 |
| 本程序 CPU 占用 | 单核约 6 % |
| 不加 `--realtime` 时 | 读到无数字的比例 0.03 %（系统繁忙时偶尔缺失约 1 毫秒） |

**模拟 VTD**（11 路：2 × 3840×2160 + 9 × 2560×1440，RGB8，每路 60 Hz），`--rate 400`：本程序 CPU 占用单核约 10 % – 13 %。
VTD 降到 16 Hz、读取方以 30 Hz 读取时：连续读到相同图像 0 次，读到无数字图像 0 次（不运行本程序时 150 次中有 109 次相同）。

说明：VTD 写入新帧时会覆盖数字，程序在几微秒内补画；这段极短的时间内读取到的图像可能没有数字。

## 9. VTD 侧要求

VTD 的 IG 需要把图像写入 key 在 `0x8100`–`0x81FF` 范围内的共享内存，例如 IG 配置中：

```xml
<RDBInterface name="MyRDBInterface" printDebugInfo="0" ignoreShmFlags="1">
    <ShmLayoutImage key="0x08130" />
</RDBInterface>
...
<Readback readbackThreadAffinity="3" synchronized="1" rdbInterface="MyRDBInterface">
    <Image outputSlot="color"/>
</Readback>
```

## 10. 注意事项

- **同一时间只运行一个实例。** 多个实例会同时改写同一位置。
- 读取方的读取频率应不高于 `--rate`，否则两次读取之间可能尚未写入新数字。
- 如果下游算法对图像内容敏感，处理时请忽略右上角 25×9 像素。
- 程序重启后数字从 `0000` 重新开始。

### 后台运行

```bash
nohup ./ShmFrameTagger --rate 400 --realtime > tagger.log 2>&1 &
# 停止：
kill $(pgrep -x ShmFrameTagger)
```

### 开机自动运行（systemd 示例）

将路径和用户替换为实际值后保存为 `/etc/systemd/system/shm-frame-tagger.service`：

```ini
[Unit]
Description=VTD SHM frame tagger
After=network.target

[Service]
User=vtd
ExecStart=/opt/ShmFrameTagger-2.1-linux-x86_64/ShmFrameTagger --rate 400
Restart=always
# 实时优先级（使用 systemd 时不需要 setcap）
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now shm-frame-tagger
journalctl -u shm-frame-tagger -f      # 查看输出
```

## 11. 常见问题

**启动后一直没有 `attached` 输出**
当前没有 key 在 `0x8100`–`0x81FF` 范围内的共享内存。请确认 VTD 已运行并按第 9 节配置；可用 `ipcs -m` 查看现有共享内存。

**`shmat failed: Permission denied`**
当前用户对该共享内存没有写权限。请用创建它的同一用户运行，或调整共享内存权限。

**状态输出中出现 `pixel format ... not supported`**
该路图像不是每通道 8 位的格式（例如浮点或 16 位格式），程序不会写入。

**启动时提示 `warning: --realtime failed (Operation not permitted)`**
没有实时优先级权限。执行一次 `sudo setcap cap_sys_nice+ep ./ShmFrameTagger` 后再运行，或用 root 运行。不处理也能正常工作。

**画面上看不到数字**
确认查看器显示的是同一路共享内存，并注意数字很小（25×9 像素），位于画面右上角边缘。
