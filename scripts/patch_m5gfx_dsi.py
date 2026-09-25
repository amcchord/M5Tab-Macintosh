"""Backport M5GFX's revision-neutral DSI initialization to pinned 0.2.23.

Upstream: m5stack/M5GFX@bfb84f20aa5aeadfb32ab7f494f1b7ed27c39104.
Patch only this environment's installed library, and fail if it drifts.
Both IDF versions used here accept zero as their default PHY clock.
"""
from pathlib import Path


def patch_source(source: str) -> str:
    declaration = "esp_lcd_dsi_bus_config_t bus_config;"
    assignment = "    bus_config.phy_clk_src = static_cast<typeof(bus_config.phy_clk_src)>(MIPI_DSI_PHY_CLK_SRC_DEFAULT);\n"
    if "esp_lcd_dsi_bus_config_t bus_config = {};" in source and assignment not in source:
        return source
    if source.count(declaration) != 1 or source.count(assignment) != 1:
        raise RuntimeError("M5GFX DSI source changed; review the compatibility patch")
    return source.replace(declaration, "esp_lcd_dsi_bus_config_t bus_config = {};").replace(assignment, "")


if "Import" in globals():
    Import("env")  # noqa: F821
    if env["PIOENV"].startswith("esp32p4_pioarduino"):
        path = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / "M5GFX/src/lgfx/v1/platforms/esp32p4/Bus_DSI.cpp"
        source = path.read_text()
        patched = patch_source(source)
        if source != patched:
            path.write_text(patched)
            print("[M5GFX] Applied revision-neutral DSI clock initialization")
