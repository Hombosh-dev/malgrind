// Small per-user conveniences kept in the webview. Storage can be unavailable: never depend on it.
export function remembered(key: string): string {
  try {
    return localStorage.getItem(`malgrind.${key}`) ?? '';
  } catch {
    return '';
  }
}

export function remember(key: string, value: string): void {
  try {
    localStorage.setItem(`malgrind.${key}`, value);
  } catch {
    /* private window or blocked storage: the app works without it */
  }
}
