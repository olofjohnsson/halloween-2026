# Sourced by the build scripts: puts the NCS toolchain on PATH when west is not already available
# (e.g. plain VS Code tasks). Override with NCS_ROOT / NCS_TOOLCHAIN.
if ! command -v west >/dev/null 2>&1; then
    NCS_ROOT="${NCS_ROOT:-$HOME/ncs/v3.4.0}"
    T="${NCS_TOOLCHAIN:-$HOME/ncs/toolchains/fbf7391cab}"
    T="${T%/}"
    export PATH="$T/bin:$T/usr/bin:$T/usr/local/bin:$T/opt/bin:$T/opt/nanopb/generator-bin:$T/nrfutil/bin:$T/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$PATH"
    export LD_LIBRARY_PATH="$T/lib:$T/lib/x86_64-linux-gnu:$T/usr/local/lib:${LD_LIBRARY_PATH:-}"
    export PYTHONHOME="$T/usr/local"
    export PYTHONPATH="$T/usr/local/lib/python3.12:$T/usr/local/lib/python3.12/site-packages"
    export GIT_EXEC_PATH="$T/usr/local/libexec/git-core"
    export GIT_TEMPLATE_DIR="$T/usr/local/share/git-core/templates"
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
    export ZEPHYR_SDK_INSTALL_DIR="$T/opt/zephyr-sdk"
    export ZEPHYR_BASE="$NCS_ROOT/zephyr"
fi
