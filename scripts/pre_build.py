Import("env")

# Pre-build script for M5Tab-Macintosh
# This script runs before the build process

def before_build(source, target, env):
    print("M5Tab-Macintosh Pre-build: Preparing build environment...")

# Register the pre-build action
env.AddPreAction("buildprog", before_build)
