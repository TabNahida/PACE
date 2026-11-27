<!-- source-hash: 8d97409a4bf1 -->
# Windows ARM64 上 OpenSSL 握手问题的诊断

[English](windows-arm64-openssl.md) · [参与贡献](README.zh.md) · [CI 覆盖范围](ci.zh.md)

这是一次功能正确性调查，不是性能测量。经过验证的构建修正只作用于原生 Windows ARM64 和 OpenSSL 3.5.7，
其他平台仍使用上游的 xmake 包。

## 复现与捕获到的故障

[CI 运行 40020194516，Windows ARM64 任务 120642440161](https://github.com/TabNahida/PACE/actions/runs/40020194516/job/120642440161)
测试的是版本 `75c0d326ed63e8e7a5c7e5a7bd7643a45139b036`。
OpenSSL 报告的配置为 `VC-WIN64-ARM`、静态 `/MT`、`/O2` 和 `/Gs0`。
PE 架构检查确认这是 ARM64 可执行文件。
捕获到的可执行文件的 SHA-256 为
`5c0f825e368c1691ecf546f1f7372f86a0fee3bc398aae99d0deb391afd524c1`。

`pace_openssl_diagnostic` 直接链接 OpenSSL，不包含 PACE 的 socket、协程或 TLS 传输代码。它用内存 BIO 完成
客户端/服务端握手，要求同时通过证书链验证和 `localhost` 主机名验证，然后检查应用数据和双向的 `close_notify`。

| 独立进程 | 修正前的结果 |
|---|---|
| RAND、SHA256、AES-GCM、X25519 导入、P-256 密钥生成 | 通过 |
| 上下文、默认信任路径、证书和私钥加载 | 通过 |
| 使用 `BIO_read_ex` / `BIO_write_ex` 的传统自定义 BIO 回调 | 通过 |
| ML-KEM-768 生成、封装和解封装 | 通过 |
| 默认 TLS 握手 | 访问冲突 |
| 仅 TLS 1.2 / 仅 TLS 1.3 | 两者均访问冲突 |
| 仅 X25519 / 仅 P-256 的 TLS 1.3 | 两者均访问冲突 |

五种握手变体捕获到的都是同一个首次机会异常：

```text
exception=0xc0000005  rva=0x86f20
operation=0 (read)   address=0x879
x0=0x1              lr=image+0x86f1c
tls_parse_all_extensions+0x10
```

对应可执行文件的反汇编包含：

```asm
; tls_parse_all_extensions, RVA 0x86f10
sub sp, sp, #0x60
mov x15, #1
bl  __chkstk                 ; RVA 0x86f18: overwrites x30/LR
sub sp, sp, x15, lsl #4      ; RVA 0x86f1c
ldr x8, [x0, #0x878]         ; RVA 0x86f20: captured fault
; ...
str x30, [sp, #0x60]         ; RVA 0x86f48: saves LR too late
; ... successful return path ...
ldr x30, [sp, #0x60]
; restore saved registers
mov w0, #1
add sp, sp, #0x70
ret
```

栈探测调用发生在第一次保存调用者返回地址之前。因此，成功返回时会跳回函数内部的 `image+0x86f1c`，此时 `x0`
中是整数结果 `1`。接下来的加载读取 `1 + 0x878 = 0x879`，与捕获到的异常完全一致。所以诊断栈中反复出现的
`tls_parse_all_extensions+0xc` 并不说明源码层面存在递归调用。

## 范围受限的构建修正

本地的 `pace-openssl3-arm64` 包（`xmake/openssl_arm64.lua`，仅在为 Windows ARM64 启用 TLS 或 QUIC 构建时由
`xmake/packages.lua` 选用）继承上游 OpenSSL 3.5.7 的源码 URL、校验和和安装脚本。在调用安装脚本之前，它把唯一的
`/Gs0 /GF /Gy` 配置项改为 `/Gs4096 /GF /Gy`。如果上游配置发生了意料之外的变化，替换会直接报错。独立的包标识和
禁用的预编译包查找，保证不会复用旧的 `/Gs0` 依赖二进制。

`/Gs4096` 是 Microsoft 的常规栈探测阈值。大的栈帧仍会探测栈页；`/GS`、`/O2`、TLS 版本、证书验证、椭圆曲线和
后量子算法都没有改变。相关依据见 Microsoft 的
[栈检查选项参考](https://learn.microsoft.com/en-us/cpp/build/reference/gs-control-stack-checking-calls)和
[ARM64 序言/展开参考](https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling)。

## 原生验证

[CI 运行 40021430370，Windows ARM64 任务 120646677124](https://github.com/TabNahida/PACE/actions/runs/40021430370/job/120646677124)
在版本 `ea6ca4141d64044ed0084bc75f224c5dad5c8321` 上通过。依赖以新的包标识构建，其运行时编译器元数据确认使用了
`/Gs4096` 和 `/O2`，目标同样是 OpenSSL 3.5.7 的 `VC-WIN64-ARM`。

全部九个独立诊断进程都通过了，包括此前失败的每一种握手变体。全部 20 个普通原生测试目标也都通过，包括 TLS、
HTTP/2、HTTP 扩展测试、WebSocket 和 TLS 教程。独立的 Python/PACE WebSocket 互通测试同样通过。没有任何 TLS 测试
被跳过，也没有通过忽略失败来让测试通过。

成功的可执行文件的 SHA-256 为
`c18adb0e45790c0c3c5e93bfbce6b349fd6ffe2ab0458934dc12ac555d138ec3`。
其中 `tls_parse_all_extensions` 从 RVA `0x845e0` 开始：

```asm
sub sp, sp, #0x60
sub sp, sp, #0x10            ; no stack-probe call for this small frame
ldr x8, [x0, #0x878]
; ... register saves, with no BL/BLR before saving LR ...
str x30, [sp, #0x60]         ; RVA 0x84610: preserves caller's return address
```

由此可见，在失败的可执行文件中找到的机器码缺陷，在成功的可执行文件中已不存在。独立诊断程序不链接任何 PACE
生产代码的目标文件，所以同一时期 PACE TLS 传输的改动无法解释这个诊断程序前后结果的差异。

## 查看证据

`ci.yml` 的 `windows-arm64` 任务在普通测试之前用 `scripts/ci_openssl_diagnostic.py` 运行诊断程序，任何阶段失败都会
让任务失败。它把各阶段的 stdout/stderr 和 `summary.json` 上传到 `functional-validation-windows-arm64` 产物中，并把
可执行文件、PDB 和链接器映射文件上传到 `openssl-diagnostic-windows-arm64-symbols`。GitHub 产物会按工作流的保留期限
过期；故障信息和相关指令已经保存在上文中。

```sh
gh run download 40020194516 --repo TabNahida/PACE \
  --name openssl-diagnostic-windows-arm64-symbols --dir arm64-before
llvm-objdump -d --start-address=0x140086f10 --stop-address=0x140087118 \
  arm64-before/build/ci/windows/arm64/release/pace_openssl_diagnostic.exe
```

对于其他构建，请用它自己的链接器映射文件查找符号；这些地址只属于捕获到的那个二进制。诊断程序中默认密钥交换组的
握手仍是每个启用 TLS 的平台上普通 `xmake test` 的一部分。
