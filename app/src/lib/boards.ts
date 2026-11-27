// Small pure helpers of the board tiles.
import type { Device, Method, Probe } from './types';

export interface FamilyLabel {
  text: string;
  tentative: boolean;
}

// Why this user cannot use the device, or null when they can.
export function accessProblem(device: Pick<Device, 'access'>): string | null {
  if (device.access.ok) return null;
  return device.access.group
    ? `No permission to use this port: you are not in the group "${device.access.group}".`
    : 'No permission to use this port.';
}

// What tells two boards of the same kind apart: short name, where they are plugged in, and the adapter's serial number.
export function boardDetail(device: Pick<Device, 'name' | 'usb'>): string {
  return [
    device.name,
    device.usb?.location && `USB ${device.usb.location}`,
    device.usb?.serial && `serial ${device.usb.serial}`
  ]
    .filter(Boolean)
    .join(' | ');
}

// USB location & serial for a debug probe without repeating its title when name == title.
export function probeDetail(device: Pick<Device, 'name' | 'title' | 'usb'>): string {
  return [
    device.name && device.name !== device.title ? device.name : '',
    device.usb?.location && `USB ${device.usb.location}`,
    device.usb?.serial && `serial ${device.usb.serial}`,
  ]
    .filter(Boolean)
    .join(' | ');
}

// Status text and tone for a debug probe tile after inspecting it.
export function probeTileStatus(
  probe: Pick<Probe, 'openocd' | 'probe_config' | 'error'> | null,
  error: string
): { text: string; tone: 'muted' | 'good' | 'bad' } {
  if (error) return { text: error, tone: 'bad' };
  if (!probe) return { text: 'Checking OpenOCD & probe config...', tone: 'muted' };
  if (probe.openocd && !probe.openocd.found) {
    return { text: probe.error || `OpenOCD (${probe.openocd.package}) was not found`, tone: 'bad' };
  }
  if (probe.probe_config) {
    return { text: `Ready | ${probe.probe_config.target_cfg}`, tone: 'good' };
  }
  if (probe.error) {
    return {
      text: probe.error.toLowerCase().includes('unsupported') ? probe.error : probe.error,
      tone: probe.error.toLowerCase().includes('unsupported') ? 'bad' : 'muted',
    };
  }
  return { text: 'Ready to configure debug probe', tone: 'muted' };
}

// Whether a method card can open RunSettings (for `no_flash` on a probe, allow opening to select target_cfg unless blocked).
export function canOpenMethod(
  device: Pick<Device, 'kind'>,
  probe: Pick<Probe, 'openocd' | 'error' | 'methods'> | null,
  method: Pick<Method, 'id' | 'available'>
): boolean {
  if (method.available) return true;
  if (method.id !== 'no_flash' || device.kind !== 'probe' || !probe) return false;
  if (probe.openocd && !probe.openocd.found) return false;
  if ((probe.error ?? '').toLowerCase().includes('unsupported')) return false;
  return true;
}

// Auto-focused method when selecting a device tile (`no_flash` when selecting a debug probe).
export function defaultMethodForDevice(
  device: Pick<Device, 'kind'>,
  probe: Pick<Probe, 'openocd' | 'error' | 'methods'> | null
): Method['id'] | null {
  if (device.kind !== 'probe' || !probe) return null;
  const noFlash = probe.methods.find((m) => m.id === 'no_flash') ?? { id: 'no_flash' as const, available: false };
  return canOpenMethod(device, probe, noFlash) ? 'no_flash' : null;
}

// Probed family (from a live frame) is shown as fact; a USB family hint is shown as "probably <name>".
export function boardFamily(
  device: Pick<Device, 'family_hint' | 'family_hint_name'>,
  probe: Pick<Probe, 'family' | 'family_name'> | null
): FamilyLabel | null {
  const probed = probe?.family_name || probe?.family;
  if (probed) return { text: probed, tentative: false };
  const hinted = device.family_hint_name || device.family_hint;
  if (hinted) return { text: `probably ${hinted}`, tentative: true };
  return null;
}
