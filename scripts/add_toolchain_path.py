"""
Pre-build script to add the RISC-V toolchain to the PATH.
The pioarduino platform installs toolchains but doesn't add them to PATH properly.
"""
import os
import glob
Import("env")

# Get the home directory
home = os.path.expanduser("~")
packages_dir = os.path.join(home, ".platformio", "packages")

# Find all toolchain-riscv32-esp packages (prefer newer versions)
toolchain_patterns = [
    os.path.join(packages_dir, "toolchain-riscv32-esp@14*", "bin"),
    os.path.join(packages_dir, "toolchain-riscv32-esp@src-*", "bin"),
    os.path.join(packages_dir, "toolchain-riscv32-esp", "bin"),
]

# Find all matching toolchain paths
toolchain_paths = []
for pattern in toolchain_patterns:
    toolchain_paths.extend(glob.glob(pattern))

# Find the first valid toolchain path with the compiler
toolchain_bin = None
for path in toolchain_paths:
    if os.path.isdir(path):
        compiler = os.path.join(path, "riscv32-esp-elf-g++")
        if os.path.isfile(compiler):
            toolchain_bin = path
            break

if toolchain_bin:
    print(f"Adding toolchain path: {toolchain_bin}")
    # Prepend to PATH
    current_path = env.get("ENV", {}).get("PATH", os.environ.get("PATH", ""))
    new_path = toolchain_bin + os.pathsep + current_path
    env["ENV"]["PATH"] = new_path
else:
    print("WARNING: Could not find RISC-V toolchain!")
    print("Searched patterns:")
    for pattern in toolchain_patterns:
        print(f"  - {pattern}")
    # List available packages for debugging
    print("\nAvailable packages:")
    for pkg in os.listdir(packages_dir):
        if "riscv" in pkg.lower():
            print(f"  - {pkg}")
