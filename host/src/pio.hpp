#pragma once

// PlatformIO integration (filled in by the PlatformIO task; this comment records the spike that proved the mechanism).
//
// Agent build, found working on espressif32@6.9.0 / ESP-IDF 5.3.1. The user's project is not modified; all output goes
// to <project>/.pio/malgrind/. Environment of the `pio run` call:
//   PLATFORMIO_BUILD_DIR     <project>/.pio/malgrind
//   PLATFORMIO_EXTRA_SCRIPTS pre:<agent-dir>/agent/esp32/inject.py   (adds the `malgrind` IDF component and points the
//                                                                      build at MALGRIND_SDKCONFIG)
//   MALGRIND_AGENT_DIR       <agent-dir>/agent/esp32
//   MALGRIND_SDKCONFIG       <project>/.pio/malgrind/sdkconfig.<env>   (the user's sdkconfig.<env> with the keys of
//                                                                      agent/esp32/sdkconfig.agent replaced, see below)
// Command: pio run -d <project> -e <env> -t upload --upload-port <port>
// (`pio run` has no -O option; options reach the IDF builder only through the pre-script.)
//
// Findings that shape the code:
//  * sdkconfig merge: an agent key replaces BOTH forms of the user's line (`CONFIG_X=..` and `# CONFIG_X is not set`);
//    sdkconfig.agent lists `# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set` too, because DEFAULT/CUSTOM are one Kconfig
//    choice and a user's DEFAULT=y would otherwise win. Without it the console stayed at 115200.
//  * Never delete only part of .pio/malgrind: a stale CMake configuration keeps old link flags.
//  * PlatformIO's IDF builder refuses a project path containing a space (espidf.py checks BUILD_DIR).
//  * IDF's WHOLE_ARCHIVE is unusable under PlatformIO (it sorts link flags, so --whole-archive lands around everything);
//    the agent object is forced into the link with `-u malgrind_autostart_link` instead.
