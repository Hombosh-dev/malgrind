# PlatformIO pre-script: compiles and links the malgrind Cortex-M/RP2040 agent into a PlatformIO
# raspberrypi project without touching the user's source files.
# Environment variables passed by malgrind (falling back to sibling ../../core and ../../platforms
# so both repository and packaged app resource layouts work identically):
#   MALGRIND_AGENT_DIR       directory containing this script and autostart.cpp (agent/rp2040)
#   MALGRIND_CORE_DIR        directory containing core/include and core/src
#   MALGRIND_PLATFORMS_DIR   directory containing platforms/cortexm
#   MALGRIND_RATE_HZ         optional sampling frequency in Hz (default 997)
#   MALGRIND_START_SAMPLING  1 to start sampling on boot, 0 to start sleeping (default 1)
Import("env")  # noqa: F821  (provided by SCons)
import os

fallback_agent_dir = (
    os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
)
agent_dir = os.path.abspath(os.environ.get("MALGRIND_AGENT_DIR") or fallback_agent_dir)
root_dir = os.path.abspath(os.path.join(agent_dir, "..", ".."))
core_dir = os.path.abspath(os.environ.get("MALGRIND_CORE_DIR") or os.path.join(root_dir, "core"))
platforms_dir = os.path.abspath(
    os.environ.get("MALGRIND_PLATFORMS_DIR") or os.path.join(root_dir, "platforms")
)

env.AppendUnique(  # noqa: F821
    CPPPATH=[
        os.path.join(core_dir, "include"),
        os.path.join(platforms_dir, "cortexm", "include"),
    ]
)

defines = []
rate_hz = os.environ.get("MALGRIND_RATE_HZ", "").strip()
if rate_hz:
    defines.append(("MALGRIND_RATE_HZ", rate_hz))
start_sampling = os.environ.get("MALGRIND_START_SAMPLING", "").strip()
if start_sampling:
    defines.append(("MALGRIND_START_SAMPLING", start_sampling))
if defines:
    env.AppendUnique(CPPDEFINES=defines)  # noqa: F821

env.BuildSources(  # noqa: F821
    os.path.join("$BUILD_DIR", "malgrind", "core"),
    os.path.join(core_dir, "src"),
    src_filter="+<profiler.cpp> +<command.cpp>",
)
env.BuildSources(  # noqa: F821
    os.path.join("$BUILD_DIR", "malgrind", "cortexm"),
    os.path.join(platforms_dir, "cortexm", "src"),
    src_filter="+<cortexm.cpp>",
)
env.BuildSources(  # noqa: F821
    os.path.join("$BUILD_DIR", "malgrind", "rp2040"),
    agent_dir,
    src_filter="+<autostart.cpp>",
)

env.Append(  # noqa: F821
    LINKFLAGS=[
        "-Wl,--wrap=isr_systick",
        "-Wl,-u,malgrind_rp2040_autostart_link",
    ]
)
