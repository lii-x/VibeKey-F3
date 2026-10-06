# 已发布固件（供网页端 OTA 跨域下载）

本目录（）存放 **VibeKey-F3 已发布的正式版固件**（`.bin`），供网页端「固件升级 → 网络获取」跨域下载。

## 为什么固件要入库

固件原本只发布在 Gitee Releases，但实测发现浏览器**无法**从 Gitee 直接下载：

| 路径 | 浏览器可下载 | 说明 |
|---|---|---|
| Gitee 列表 API | 可以 | 直接返回 JSON，带 `Access-Control-Allow-Origin: *` |
| Gitee 固件直链 | **不行** | 两跳 302，首跳 `gitee.com` 的响应不带 CORS 头，跨域重定向后被浏览器拦截 |
| jsDelivr 代理 Gitee 源 | **不行** | 实测返回 404（jsDelivr 只代理 GitHub / GitLab / npm） |
| **jsDelivr 代理本仓库** | **可以** | `cdn.jsdelivr.net` 带 `access-control-allow-origin: *` |

所以链路必须是：Gitee 发布 → 同步到本仓库 → 网页端经 jsDelivr 下载。

网页端 `js/16-ota-net.js` 的下载通道表 `OTA_MIRRORS` 会自动依次尝试
jsDelivr → GitHub 直链 → Gitee 直链，本目录即是 jsDelivr 通道的数据来源。

## 文件说明

| 文件 | 版本 | 字节数 | SHA-256 |
|---|---|---|---|
| `vibekey-f3-v1.1.2.bin` | 1.1.2 | 1052880 | `f76bd95f3ff8f60b208f01142ba5585a5275caabdfff7c4217f6245358e17a4d` |
| `vibekey-f3-v1.1.1.bin` | 1.1.1 | 1048320 | `c6cabe6850bda9fdadc96b3661ae15f33e63e17d6d24a1ce7713867ea63cff49` |
| `vibekey-f3-v1.1.0.bin` | 1.1.0 | 1047928 | `7c3befa560032209568115ca998faf1fb17b1fd53603849ee7b825d067e16f16` |
| `vibekey-f3-v1.0.28.bin` | 1.0.28 | 1047200 | `d56de2df3342b05115eee52ac2465d85897e8072f3dd8740b368b10d43e49c62` |
| `vibekey-f3-v1.0.13.bin` | 1.0.13 | 1043424 | `d0b39c027bf472236462e6a1df116509471d233f40847e319d64ac2847fb478d` |

命名规则与 Gitee Releases 资产一致：`vibekey-f3-v<版本>.bin`，
以便网页端把 Gitee 列表里的 `file` 名直接拼成 jsDelivr 路径。

## 收录标准

只收录**已发布**的正式版，且必须同时满足：

1. 含型号标识 `VIBEKEYF3-FWIMG-7F3A9C21`（网页端预检 `containsModelTag` 的依据）
2. 不超过 OTA 分区上限 `0x580000`（5767168 字节）
3. 来自 Gitee Releases 的已发布资产，**不用本地未发布的构建产物**

由 `tools/sync_firmware.py` 自动拉取并校验后写入。

> 注意：`Firmware/project/build_*/main.bin` 是**未发布**的本地开发版本
> （与线上正式版哈希不同），它属于源码构建产物，遵循 `.gitignore` 不入库。

## 同步新版本

```bash
export GITEE_TOKEN=<你的 Gitee 令牌>
python tools/sync_firmware.py
```

脚本会拉取 Gitee 上所有 `.bin`、校验型号标识与大小、写入本目录并提交。
