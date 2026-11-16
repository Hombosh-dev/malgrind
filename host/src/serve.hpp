#pragma once

// The sidecar protocol of the malgrind app: one JSON object per line on stdin and stdout.
//
//   request   {"id": N, "cmd": "...", ...arguments}   `id` only matches the reply; a run is named by the argument `run`
//   reply     {"id": N, "ok": true, "result": ...}  or  {"id": N, "ok": false, "error": "..."}
//   event     {"event": "...", ...}   (no id) log, target, notice, ended, build
//
// Commands (arguments -> result):
//   hello                                  -> {version, data_dir, pio}
//   ports                                  -> ["/dev/ttyUSB0", ...]
//   probe {port, project?}                 -> {busy, agent, denied, arch?, ncores?, cpu_mhz?, methods}   listens up to 2 s, records nothing;
//                                             methods = [{id: attach|new_run|no_flash, available, why}]; `project` (an id) only
//                                             decides whether new_run is possible with it
//   chip_id {port}                         -> {chip}                                    resets the board
//   boards                                 -> {ports: [{port, adapter, vid, pid, serial, location, access: {ok, group, in_group}}],
//                                              probes: [{name, vid, pid}]}   what USB and file permissions say, nothing is opened
//   setup                                  -> {pio, python, git (paths or null), pkexec (bool), user}   what the app needs from the machine
//   install_pio {}                         -> {pio}   python -m venv ~/.platformio/penv + pip install platformio; streams `build` events
//   grant_access {port}                    -> {ok_now, relogin}   one pkexec password prompt: an ACL on the port and the device's group
//   projects                               -> [Project + run_count + last_run]
//   project_add {source, dir?|url?|elf?, name?}   source "folder" (a PlatformIO project of the user's, trusted), "git" (a clone of
//                                          https://github.com/owner/repo, NOT trusted yet, streams `build` events), "none" (only a
//                                          board, optional ELF) -> Project {id, name, source, dir, url, env, elf, trusted, has_source, envs, problem}
//   project_update {project, name?, env?, trusted?} -> Project
//   project_remove {project}               -> {}      the record and a clone; never the user's own folder
//   project {dir}                          -> {dir, envs, default_env}
//   new_run {project, port, name, env?}    -> {run}   backs up the board's flash (first time), builds with the agent, uploads,
//                                             then records live; streams `build` events
//   attach {port, name, project?, elf?}    -> {run}   records a board that already runs the agent; `project` is an id
//   stop                                   -> {}      ends the live run, or closes an opened one
//   remove_agent {port, how?, project?}    -> {}      how "restore" (default): writes the board's backup back; "rebuild": builds
//                                             the project (an id) without the agent and uploads it
//   runs                                   -> [RunMeta...], the live one has "live": true
//   open {run}                             -> {run}   replays a stored run (refused while a run is live)
//   update {run, name?, notes?}            -> RunMeta
//   delete {run}                           -> {}      (refused for the run in use)
//   snapshot {core?, window_s, at_ms?}     -> {tree, stats}   {"tree": null} before the first frame
//   functions {core?, window_s, at_ms?, focus?} -> {total, functions, callers, callees}
//
// new_run, remove_agent, project_add, install_pio, grant_access and chip_id are slow and run on a worker thread; only one at a time ("busy: ...").

#include <filesystem>
#include <istream>
#include <ostream>

namespace malgrind::host {

struct ServeOptions {
    std::filesystem::path data_dir;  // where runs are stored
    std::filesystem::path agent_dir; // contains agent/esp32/ (the app's resources, or the repository)
    std::filesystem::path home;      // for finding PlatformIO and esptool
    std::filesystem::path backup_dir; // flash backups, <dir>/<board MAC>/<time>.bin (see backup.hpp)
    std::filesystem::path projects_dir; // projects and clones of GitHub projects (see projects.hpp)
};

// Serves requests from `in` until it ends (the app closed, or end of file), then closes the live run. Returns 0.
int serve(std::istream& in, std::ostream& out, const ServeOptions& options);

} // namespace malgrind::host
