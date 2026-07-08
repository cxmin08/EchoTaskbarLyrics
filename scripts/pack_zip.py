# SPDX-License-Identifier: GPL-3.0
#
# pack_zip.py — 打包 echo-taskbar-lyrics 为 zip（正斜杠路径，兼容所有 zip 解析器）
# EchoMusic 的 installPluginFromZip 要求所有文件必须放在一个顶层目录内
# 例如: echo-taskbar-lyrics/manifest.json，根目录散放会报"未找到 manifest.json"
import os
import sys
import zipfile

if len(sys.argv) != 3:
    print("Usage: pack_zip.py <source-dir> <output-zip>", file=sys.stderr)
    sys.exit(2)

src_dir = sys.argv[1]
zip_path = sys.argv[2]

if not os.path.isdir(src_dir):
    print(f"Error: source directory not found: {src_dir}", file=sys.stderr)
    sys.exit(1)

# 使用源目录名作为 zip 内的根目录名
root_dir = os.path.basename(os.path.normpath(src_dir))

expected_exe = os.path.join(src_dir, "EchoTaskbarLyrics.exe")
if not os.path.exists(expected_exe):
    print("Warning: EchoTaskbarLyrics.exe not found; build the native target before release packaging.")

with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        # 排除 debug.log，避免调试日志进入分发包
        files = sorted(
            f for f in files
            if f.lower() != "debug.log" and not f.lower().endswith(".pyc")
        )
        for file in files:
            file_path = os.path.join(root, file)
            rel_path = os.path.relpath(file_path, src_dir).replace('\\', '/')
            # 所有文件放在 root_dir/ 下
            arcname = f"{root_dir}/{rel_path}"
            zf.write(file_path, arcname)

print(f"Packed {zip_path}")
