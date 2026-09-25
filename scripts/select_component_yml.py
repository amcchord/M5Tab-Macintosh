"""
Pre-build script that selects the correct idf_component.yml for the current
PlatformIO environment.

Each env (Tab5 vs Waveshare) needs a different set of ESP-IDF managed
components. Rather than force every build to download every component, we
keep per-env manifests next to platformio.ini and copy the right one into
`idf_component.yml` at build time.

Expected layout:
    idf_component_tab5.yml        -> minimal (no extra components)
    idf_component_waveshare.yml   -> Waveshare BSP + codec + touch + panel

The script is idempotent: it only writes when the content actually differs.
"""
import os
import shutil

Import("env")  # noqa: F821 - provided by SCons/PlatformIO

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
ENV_NAME = env.get("PIOENV")  # noqa: F821

TARGET = os.path.join(PROJECT_DIR, "idf_component.yml")

if ENV_NAME.startswith("waveshare_p4_101"):
    SOURCE_NAME = "idf_component_waveshare.yml"
elif ENV_NAME.startswith("esp32p4_pioarduino"):
    SOURCE_NAME = "idf_component_tab5.yml"
else:
    SOURCE_NAME = None

if SOURCE_NAME is None:
    print(f"[component-yml] Unknown env '{ENV_NAME}', leaving idf_component.yml untouched")
else:
    source_path = os.path.join(PROJECT_DIR, SOURCE_NAME)
    if not os.path.isfile(source_path):
        print(f"[component-yml] WARNING: {source_path} not found")
    else:
        needs_copy = True
        if os.path.isfile(TARGET):
            try:
                with open(source_path, "rb") as fsrc, open(TARGET, "rb") as fdst:
                    needs_copy = fsrc.read() != fdst.read()
            except OSError:
                needs_copy = True
        if needs_copy:
            shutil.copyfile(source_path, TARGET)
            print(f"[component-yml] Selected {SOURCE_NAME} for env {ENV_NAME}")
        else:
            print(f"[component-yml] idf_component.yml already matches {SOURCE_NAME}")
