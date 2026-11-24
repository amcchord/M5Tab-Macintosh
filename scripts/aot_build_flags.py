"""
Compiler flags for the translated code (src/basilisk/aot/aot_rom_generated.cpp).

The firmware copies that code to PSRAM and runs it there, so it must not
depend on where it lies:

  -fno-jump-tables       a jump table holds the addresses the code was linked
                         at, and would lead back into the copy in flash
  -fno-toplevel-reorder  keeps the functions between the two markers that
                         bracket them, in the order they are written

tools/aot/check_blob.py checks the result after linking.
"""
Import("env")


def translated_code(env, node):
    return env.Object(
        node,
        CCFLAGS=env["CCFLAGS"] + ["-fno-jump-tables", "-fno-toplevel-reorder", "-fno-stack-protector"],
    )


env.AddBuildMiddleware(translated_code, "*/aot_rom_generated.cpp")
