"""Make the active PlatformIO platform's ESP RISC-V toolchain available to SCons."""
import os

Import("env")

toolchain = env.PioPlatform().get_package_dir("toolchain-riscv32-esp")
if not toolchain:
    raise RuntimeError("PlatformIO did not resolve the ESP RISC-V toolchain")
compiler_dir = os.path.join(toolchain, "bin")
if not os.path.isfile(os.path.join(compiler_dir, "riscv32-esp-elf-g++")):
    raise RuntimeError(f"Missing ESP RISC-V compiler in {compiler_dir}")
env.PrependENVPath("PATH", compiler_dir)
print(f"Adding toolchain path: {compiler_dir}")
