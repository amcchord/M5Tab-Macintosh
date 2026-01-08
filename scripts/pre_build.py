Import("env")

# Pre-build script for BigDashVesc
# This script runs before the build process

def before_build(source, target, env):
    print("BigDashVesc Pre-build: Preparing build environment...")

# Register the pre-build action
env.AddPreAction("buildprog", before_build)
