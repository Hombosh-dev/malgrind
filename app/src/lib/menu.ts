import { Menu, MenuItem, Submenu } from '@tauri-apps/api/menu';

export interface MenuActions {
  newProject(): void;
  stop(): void;
  removeAgent(): void;
}

// The native menu bar: Run (New project, Stop) and Board (Remove agent).
export async function installMenu(actions: MenuActions): Promise<void> {
  const run = await Submenu.new({
    text: 'Run',
    items: [
      await MenuItem.new({ text: 'New project...', accelerator: 'CmdOrCtrl+N', action: actions.newProject }),
      await MenuItem.new({ text: 'Stop', accelerator: 'CmdOrCtrl+.', action: actions.stop }),
    ],
  });
  const board = await Submenu.new({
    text: 'Board',
    items: [await MenuItem.new({ text: 'Remove agent...', action: actions.removeAgent })],
  });
  await (await Menu.new({ items: [run, board] })).setAsAppMenu();
}
