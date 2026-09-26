#!/usr/bin/env bash
# 编译并打包 demo 的 HAP，安装到已连接设备并启动。
#
# 用法:
#   ./build-and-run.sh              # 打包 + 安装 + 启动
#   ./build-and-run.sh --no-deploy  # 只打包
#
# 依赖: DevEco Studio（含 HarmonyOS SDK）、hdc。
#   DEVECO=/Applications/DevEco-Studio.app/Contents   # 可覆盖
#
# 说明: 安装需要签名 HAP。请在 DevEco Studio 中先配置自动签名（见 README），
#       否则只会产出未签名 HAP，无法安装到设备。
#
# demo 直接依赖仓库根目录的 backdrop 库源码（demo/entry/oh-package.json5 中
# `"backdrop": "file:../../backdrop"`），所以这里只需构建 demo 工程。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$ROOT/demo"
DEVECO="${DEVECO:-/Applications/DevEco-Studio.app/Contents}"
TOOLCHAINS="$DEVECO/sdk/default/openharmony/toolchains"
BUNDLE="com.example.liquidglass"

# 多台设备时用 HDC_TARGET 指定，例如 HDC_TARGET=192.168.1.10:5555
HDC_TARGET="${HDC_TARGET:-}"
if [[ -n "$HDC_TARGET" ]]; then
  hdc() { command hdc -t "$HDC_TARGET" "$@"; }
fi

export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-$DEVECO/sdk}"
export PATH="$TOOLCHAINS:$PATH"

DEPLOY=1
[[ "${1:-}" == "--no-deploy" ]] && DEPLOY=0

echo "==> [1/2] 打包 HAP"
cd "$PROJECT"
"$DEVECO/tools/ohpm/bin/ohpm" install --all >/dev/null
"$DEVECO/tools/node/bin/node" "$DEVECO/tools/hvigor/bin/hvigorw.js" \
  --no-daemon --mode module -p module=entry@default -p product=default \
  -p buildMode=debug -p requiredDeviceType=phone assembleHap

HAP="$PROJECT/entry/build/default/outputs/default/entry-default-signed.hap"
[[ -f "$HAP" ]] || HAP="$PROJECT/entry/build/default/outputs/default/entry-default-unsigned.hap"
ls -lh "$HAP"

if [[ "$DEPLOY" == "0" ]]; then
  echo "==> 跳过部署 (--no-deploy)"
  exit 0
fi

echo "==> [2/2] 安装并启动"
hdc install -r "$HAP"
hdc shell aa force-stop "$BUNDLE" || true
hdc shell aa start -a EntryAbility -b "$BUNDLE"
