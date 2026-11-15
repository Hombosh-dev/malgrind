import { Menu, MenuItem, Submenu } from '@tauri-apps/api/menu';

export interface MenuActions {
  newRun(): void;
  stop(): void;
  removeAgent(): void;
}

// The native menu bar: Run (New run, Stop) and Board (Remove agent).
export async function installMenu(actions: MenuActions): Promise<void> {
  const run = await Submenu.new({
    text: 'Run',
    items: [
      await MenuItem.new({ text: 'New run…', accelerator: 'CmdOrCtrl+N', action: actions.newRun }),
      await MenuItem.new({ text: 'Stop', accelerator: 'CmdOrCtrl+.', action: actions.stop }),
    ],
  });
  const board = await Submenu.new({
    text: 'Board',
    items: [await MenuItem.new({ text: 'Remove agent…', action: actions.removeAgent })],
  });
  await (await Menu.new({ items: [run, board] })).setAsAppMenu();
}
