# PlatformIO pre-script: adds the malgrind agent component to an ESP-IDF project without touching the project's files.
# The caller (malgrind) passes the paths in the environment:
#   MALGRIND_AGENT_DIR  directory holding the `malgrind` IDF component (this file's directory)
#   MALGRIND_SDKCONFIG  sdkconfig the build reads and writes (the user's one plus sdkconfig.agent, in .pio/malgrind)
Import("env")  # noqa: F821  (provided by SCons)
import os

board = env.BoardConfig()  # noqa: F821
components = [os.environ["MALGRIND_AGENT_DIR"]]
# The IDF builder passes the project's own src_dir as an extra component; our setting would replace its list, so repeat it.
src_dir, project_dir = env.subst("$PROJECT_SRC_DIR"), env.subst("$PROJECT_DIR")  # noqa: F821
if src_dir != os.path.join(project_dir, "main"):
    components.append(src_dir)
board.update("build.cmake_extra_args", "-DEXTRA_COMPONENT_DIRS=" + ";".join(components))
board.update("build.esp-idf.sdkconfig_path", os.environ["MALGRIND_SDKCONFIG"])
