#!/bin/sh
# EmbMQTTNode OTA Firmware Signing Tool (v1.2.9)
# ===============================================
# 对固件文件生成 SHA256 数字签名（独立 .sig 文件），配合设备侧
# ota_verify_signature（RSA/EC 公钥）+ node.conf 的 ota_public_key
# 实现 fail-closed 固件验签（P0-5 修复）。
#
# 用法:
#   ./tools/sign_firmware.sh <私钥PEM> <固件文件> [输出.sig]
#
# 示例:
#   # 1. 生成密钥对（只需一次；私钥务必离线保管，绝不下发到设备/源站）
#   openssl genrsa -out ota_root_key.pem 2048
#   openssl rsa -in ota_root_key.pem -pubout -out ota_pub.pem
#
#   # 2. 固件出包后签名（.sig 与固件放在同一 HTTP(S) 目录）
#   ./tools/sign_firmware.sh ota_root_key.pem dist/embmqttnode_2.0.1.bin
#
#   # 3. 设备侧 node.conf 配置公钥（未配置 = fail-closed 拒绝一切升级）
#   ota_public_key = /etc/embmqttnode/ota_pub.pem
#
# 签名算法: openssl dgst -sha256 -sign（RSA PKCS#1 / EC 同样适用；
# 设备侧用 EVP 通用验签，Ed25519 私钥亦可用本脚本）
#
# 输出: <固件文件>.sig（原始签名字节；设备按 固件URL + ".sig" 拉取）

set -e

usage() {
    echo "Usage: $0 <private_key.pem> <firmware_file> [output.sig]" >&2
    exit 1
}

[ $# -ge 1 ] && [ $# -le 3 ] || usage

KEY_FILE="$1"
FW_FILE="$2"
SIG_FILE="${3:-$FW_FILE.sig}"

[ -n "$KEY_FILE" ] || usage
[ -n "$FW_FILE" ]  || usage

[ -f "$KEY_FILE" ] || { echo "ERROR: private key not found: $KEY_FILE" >&2; exit 1; }
[ -f "$FW_FILE" ]  || { echo "ERROR: firmware not found: $FW_FILE" >&2; exit 1; }

# 确认私钥可解析（提前报错，避免产出半截 .sig）
openssl pkey -in "$KEY_FILE" -noout > /dev/null 2>&1 || {
    echo "ERROR: cannot parse private key: $KEY_FILE" >&2
    exit 1
}

openssl dgst -sha256 -sign "$KEY_FILE" -out "$SIG_FILE" "$FW_FILE"

SIG_SIZE=$(wc -c < "$SIG_FILE" | tr -d ' ')
FW_SHA=$(openssl dgst -sha256 -r "$FW_FILE" | awk '{print $1}')

echo "=== Firmware signed ==="
echo "  Firmware:   $FW_FILE"
echo "  Signature:  $SIG_FILE ($SIG_SIZE bytes)"
echo "  SHA256:     $FW_SHA"
echo ""
echo "Deploy: upload $FW_FILE and $SIG_FILE to the same HTTP(S) directory."
echo "Device fetches: <firmware URL> and <firmware URL>.sig"
