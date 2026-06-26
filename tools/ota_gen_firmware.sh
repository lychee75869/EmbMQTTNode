#!/bin/bash
# EmbMQTTNode OTA Firmware Packager
# =================================
# 编译 → 打包为 .bin → SHA256 校验 → 输出 MQTT OTA 指令 JSON
#
# 用法:
#   ./tools/ota_gen_firmware.sh 2.0.1                    # 默认 x86_64
#   ./tools/ota_gen_firmware.sh 2.0.1 aarch64-linux-gnu-  # ARM64 交叉编译
#
# 输出:
#   dist/embmqttnode_2.0.1.bin         固件二进制
#   dist/embmqttnode_2.0.1.bin.sha256  校验和文件
#   dist/ota_cmd_2.0.1.json            MQTT OTA 指令（可直接发布）

set -e

VERSION="${1:-1.0.0}"
CROSS_COMPILE="${2:-}"

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="$PROJECT_ROOT/dist"

echo "=== EmbMQTTNode OTA Firmware Packager ==="
echo "  Version:       $VERSION"
echo "  Cross Compile: ${CROSS_COMPILE:-(none, native build)}"
echo "  Project Root:  $PROJECT_ROOT"
echo ""

# 1. 编译
echo "[1/4] Building..."
cd "$PROJECT_ROOT/src"
make clean CROSS_COMPILE="$CROSS_COMPILE" > /dev/null 2>&1
make CROSS_COMPILE="$CROSS_COMPILE" BUILD_WITH_MODBUS=1
echo "  Build OK"

# 2. Strip + 准备输出目录
echo "[2/4] Preparing firmware binary..."
mkdir -p "$DIST_DIR"
cp embmqttnode "$DIST_DIR/embmqttnode_${VERSION}.bin"
${CROSS_COMPILE}strip --strip-unneeded "$DIST_DIR/embmqttnode_${VERSION}.bin" 2>/dev/null || true
SIZE=$(stat -c%s "$DIST_DIR/embmqttnode_${VERSION}.bin")
echo "  Firmware size: $SIZE bytes"

# 3. SHA256 校验和
echo "[3/4] Computing SHA256..."
SHA256=$(sha256sum "$DIST_DIR/embmqttnode_${VERSION}.bin" | awk '{print $1}')
echo "$SHA256  embmqttnode_${VERSION}.bin" > "$DIST_DIR/embmqttnode_${VERSION}.bin.sha256"
echo "  SHA256: $SHA256"

# 4. 生成 MQTT OTA 指令 JSON
echo "[4/4] Generating OTA command JSON..."
# 使用本地 HTTP 服务器地址（用户需根据实际情况修改）
HTTP_HOST="${OTA_HTTP_HOST:-192.168.1.100:8080}"
OTA_CMD_JSON="$DIST_DIR/ota_cmd_${VERSION}.json"

cat > "$OTA_CMD_JSON" << EOF
{
    "cmd":       "upgrade",
    "version":   "$VERSION",
    "url":       "http://$HTTP_HOST/embmqttnode_${VERSION}.bin",
    "checksum":  "sha256:$SHA256",
    "force":     false
}
EOF

echo ""
echo "=== Done ==="
echo ""
echo "Output files:"
echo "  Firmware:  $DIST_DIR/embmqttnode_${VERSION}.bin"
echo "  SHA256:    $DIST_DIR/embmqttnode_${VERSION}.bin.sha256"
echo "  OTA Cmd:   $OTA_CMD_JSON"
echo ""
echo "To serve firmware via HTTP (Python):"
echo "  cd $DIST_DIR && python3 -m http.server 8080"
echo ""
echo "To trigger OTA upgrade via MQTT:"
echo "  mosquitto_pub -h <broker> -t 'embmqttnode/<client_id>/ota/cmd' -f $OTA_CMD_JSON"
echo ""
echo "To monitor OTA status:"
echo "  mosquitto_sub -h <broker> -t 'embmqttnode/<client_id>/ota/status'"
