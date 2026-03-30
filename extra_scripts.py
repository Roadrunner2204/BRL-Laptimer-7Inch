# extra_scripts.py  (runs as pre: script — before source scanning)
#
# ARM Helium (MVE) assembly files in LVGL are only for Cortex-M cores.
# The Xtensa ESP32-S3 assembler cannot process them (chokes on C typedefs
# from included headers). We replace them with empty stubs before the build.

Import("env")
import os


def neutralize_helium_asm():
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pio_env     = env.subst("$PIOENV")

    helium_dir = os.path.join(
        libdeps_dir, pio_env, "lvgl", "src",
        "draw", "sw", "blend", "helium"
    )

    if not os.path.isdir(helium_dir):
        # LVGL may not be installed yet (first run) – nothing to patch
        return

    for fname in os.listdir(helium_dir):
        if fname.endswith(".S"):
            fpath = os.path.join(helium_dir, fname)
            with open(fpath, "r") as f:
                first = f.read(80)
            if "Neutralized" not in first:
                print("[extra_scripts] Neutralizing ARM Helium ASM: " + fpath)
                with open(fpath, "w") as f:
                    f.write(
                        "/* Neutralized by extra_scripts.py\n"
                        " * ARM Helium (MVE) is not supported on Xtensa ESP32-S3.\n"
                        " * This stub keeps the assembler happy. */\n"
                    )


neutralize_helium_asm()
