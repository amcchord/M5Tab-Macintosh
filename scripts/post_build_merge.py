"""
Post-build script to create merged release binary.

This script runs after PlatformIO builds the firmware and creates
a properly merged binary with bootloader, partition table, and
application - ready for single-command flashing.

The merged binary is created in the project's 'release' directory.

To enable, add to platformio.ini:
    extra_scripts =
        post:scripts/post_build_merge.py
"""

Import("env")

import os
import subprocess
import sys

def create_merged_binary(source, target, env):
    """Create a merged binary after successful build."""
    
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    
    # File paths
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = os.path.join(build_dir, "firmware.bin")
    
    # Output directory and file
    release_dir = os.path.join(project_dir, "release")
    output_file = os.path.join(release_dir, "M5Tab-Macintosh.bin")
    
    # Verify all required files exist
    missing = []
    for f in [bootloader, partitions, firmware]:
        if not os.path.exists(f):
            missing.append(os.path.basename(f))
    
    if missing:
        print(f"[MERGE] Skipping merge - missing files: {', '.join(missing)}")
        return
    
    # Create release directory
    os.makedirs(release_dir, exist_ok=True)
    
    # Find esptool - check multiple locations
    esptool_cmd = None
    
    # Possible esptool locations
    possible_paths = [
        # PlatformIO's esptool
        os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py"),
        # Homebrew (macOS)
        "/opt/homebrew/bin/esptool.py",
        "/opt/homebrew/bin/esptool",
        "/usr/local/bin/esptool.py",
        "/usr/local/bin/esptool",
    ]
    
    # Check explicit paths first
    for path in possible_paths:
        if os.path.exists(path):
            esptool_cmd = path
            break
    
    # Try PATH as fallback
    if not esptool_cmd:
        for cmd in ["esptool", "esptool.py"]:
            try:
                subprocess.run([cmd, "--version"], capture_output=True, check=True)
                esptool_cmd = cmd
                break
            except (subprocess.CalledProcessError, FileNotFoundError):
                continue
    
    if not esptool_cmd:
        print("[MERGE] WARNING: esptool not found, skipping merge")
        print("[MERGE] Install with: pip install esptool")
        return
    
    print(f"[MERGE] Using esptool: {esptool_cmd}")
    
    # ESP32-P4 flash offsets
    bootloader_offset = "0x2000"
    partition_offset = "0x8000"
    app_offset = "0x10000"
    
    # Build esptool merge command
    # Use older syntax (merge_bin) for compatibility with PlatformIO's bundled esptool
    cmd = [
        sys.executable, esptool_cmd, "--chip", "esp32p4", "merge_bin",
        "-o", output_file,
        "--flash_mode", "qio",
        "--flash_freq", "80m",
        "--flash_size", "16MB",
        bootloader_offset, bootloader,
        partition_offset, partitions,
        app_offset, firmware
    ]
    
    # If esptool is not a .py file, don't use python to run it
    if not esptool_cmd.endswith(".py"):
        cmd = cmd[1:]  # Remove sys.executable
    
    print("")
    print("[MERGE] Creating merged release binary...")
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode == 0:
            # Verify the output
            size = os.path.getsize(output_file)
            print(f"[MERGE] Created: {output_file}")
            print(f"[MERGE] Size: {size:,} bytes ({size / 1024 / 1024:.2f} MB)")
            
            # Verify bootloader header
            with open(output_file, "rb") as f:
                f.seek(0x2000)
                header = f.read(4)
                if header[0] == 0xE9:
                    print("[MERGE] Bootloader header: VALID")
                else:
                    print(f"[MERGE] WARNING: Unexpected bootloader header: {header.hex()}")
        else:
            print(f"[MERGE] ERROR: {result.stderr}")
            
    except Exception as e:
        print(f"[MERGE] ERROR: {e}")

# Register post-build action
env.AddPostAction("$BUILD_DIR/firmware.bin", create_merged_binary)
