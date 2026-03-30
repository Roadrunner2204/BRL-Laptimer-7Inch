# extra_scripts.py
#
# Exclude ARM Helium assembly files from the LVGL build.
# lv_blend_helium.S contains ARM MVE (Helium) SIMD instructions that are
# incompatible with the Xtensa ESP32-S3 toolchain assembler.

Import("env")


def skip_helium_asm(node):
    """Return [] to drop ARM Helium .S files from the build."""
    path = str(node)
    if "helium" in path.lower() and path.endswith(".S"):
        return []
    return node


env.AddBuildMiddleware(skip_helium_asm)
