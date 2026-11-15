// The window is a shell: everything the app does is in the C++ sidecar (malgrind-cli) and the TypeScript UI.
fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .run(tauri::generate_context!())
        .expect("error while running malgrind");
}
