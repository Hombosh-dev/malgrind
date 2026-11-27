import { describe, expect, it } from 'vitest';
import {
  accessProblem,
  boardDetail,
  boardFamily,
  canOpenMethod,
  defaultMethodForDevice,
  probeDetail,
  probeTileStatus,
} from './boards';
import type { Probe, UsbInfo } from './types';

const usb = (location: string, serial: string): UsbInfo => ({
  vid: '1a86',
  pid: '7523',
  location,
  serial,
  product: '',
  manufacturer: ''
});

describe('accessProblem', () => {
  it('is null when the port can be opened', () => {
    expect(accessProblem({ access: { ok: true, group: 'dialout', in_group: true, fixable: false } })).toBeNull();
  });
  it('names the group the user is missing', () => {
    expect(accessProblem({ access: { ok: false, group: 'dialout', in_group: false, fixable: true } })).toContain('"dialout"');
  });
  it('still says something when the group is unknown or not fixable', () => {
    expect(accessProblem({ access: { ok: false, group: '', in_group: false, fixable: false } })).toMatch(/^No permission/);
  });
});

describe('boardDetail', () => {
  it('tells identical boards apart by name, socket and serial number', () => {
    expect(boardDetail({ name: 'usbserial-1410', usb: usb('3-3', '0001') })).toBe('usbserial-1410 · USB 3-3 · serial 0001');
    expect(boardDetail({ name: 'usbserial-1420', usb: usb('3-4', '') })).toBe('usbserial-1420 · USB 3-4');
    expect(boardDetail({ name: 'usbserial-1430', usb: usb('', '') })).toBe('usbserial-1430');
    expect(boardDetail({ name: 'usbserial-1440', usb: null })).toBe('usbserial-1440');
    expect(boardDetail({ name: '', usb: null })).toBe('');
  });
});

describe('boardFamily', () => {
  it('returns null for generic bridges without a hint or probed family', () => {
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, null)).toBeNull();
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, { family: '', family_name: '' })).toBeNull();
  });

  it('marks a USB family hint as tentative before and after a non-agent probe', () => {
    expect(boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, null)).toEqual({
      text: 'probably Acme MCU',
      tentative: true
    });
    expect(boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, { family: '', family_name: '' })).toEqual({
      text: 'probably Acme MCU',
      tentative: true
    });
    expect(boardFamily({ family_hint: 'acme', family_hint_name: '' }, null)).toEqual({
      text: 'probably acme',
      tentative: true
    });
  });

  it('shows the probed family as fact even when a hint existed', () => {
    expect(
      boardFamily({ family_hint: 'acme', family_hint_name: 'Acme MCU' }, { family: 'acme', family_name: 'Acme MCU' })
    ).toEqual({
      text: 'Acme MCU',
      tentative: false
    });
    expect(boardFamily({ family_hint: '', family_hint_name: '' }, { family: 'acme', family_name: '' })).toEqual({
      text: 'acme',
      tentative: false
    });
  });
});

describe('probe helpers', () => {
  it('formats probe details without repeating matching title and name', () => {
    expect(probeDetail({ name: 'ST-Link', title: 'ST-Link', usb: usb('1-2', 'ST01') })).toBe('USB 1-2 · serial ST01');
    expect(probeDetail({ name: 'J-Link', title: 'SEGGER J-Link', usb: usb('1-3', '') })).toBe('J-Link · USB 1-3');
  });

  it('reports probe tile status for checking, missing OpenOCD, ready, and target-needed states', () => {
    expect(probeTileStatus(null, '')).toEqual({ text: 'Checking OpenOCD & probe config…', tone: 'muted' });
    expect(probeTileStatus(null, 'failed')).toEqual({ text: 'failed', tone: 'bad' });
    expect(
      probeTileStatus(
        {
          openocd: { package: 'tool-openocd', found: false, exe: null, scripts: null },
          probe_config: null,
          error: 'OpenOCD (tool-openocd) was not found',
        },
        ''
      )
    ).toEqual({ text: 'OpenOCD (tool-openocd) was not found', tone: 'bad' });
    expect(
      probeTileStatus(
        {
          openocd: { package: 'tool-openocd', found: true, exe: '/bin/openocd', scripts: '/scripts' },
          probe_config: {
            package: 'tool-openocd',
            interface_desc: 'ST-Link',
            target_cfg: 'target/stm32f4x.cfg',
            openocd_args: [],
            sample_mode: 'auto',
            target_arch: null,
          },
          error: '',
        },
        ''
      )
    ).toEqual({ text: '● Ready · target/stm32f4x.cfg', tone: 'good' });
  });

  it('focuses no_flash for probes when OpenOCD is available even if target_cfg still needs to be picked', () => {
    const probeNeedingTarget: Probe = {
      kind: 'probe',
      busy: false,
      agent: false,
      denied: false,
      family: '',
      family_name: '',
      openocd: { package: 'tool-openocd', found: true, exe: '/bin/openocd', scripts: '/scripts' },
      probe_config: null,
      error: 'choose the chip target config for OpenOCD',
      methods: [
        { id: 'attach', available: false, why: 'requires a serial port' },
        { id: 'new_run', available: false, why: 'requires a serial port' },
        { id: 'no_flash', available: false, why: 'choose the chip target config for OpenOCD' },
      ],
    };
    expect(canOpenMethod({ kind: 'probe' }, probeNeedingTarget, probeNeedingTarget.methods[2])).toBe(true);
    expect(canOpenMethod({ kind: 'probe' }, probeNeedingTarget, probeNeedingTarget.methods[0])).toBe(false);
    expect(defaultMethodForDevice({ kind: 'probe' }, probeNeedingTarget)).toBe('no_flash');

    const probeMissingOcd: Probe = {
      ...probeNeedingTarget,
      openocd: { package: 'tool-openocd', found: false, exe: null, scripts: null },
    };
    expect(canOpenMethod({ kind: 'probe' }, probeMissingOcd, probeMissingOcd.methods[2])).toBe(false);
    expect(defaultMethodForDevice({ kind: 'probe' }, probeMissingOcd)).toBeNull();
    expect(defaultMethodForDevice({ kind: 'serial' }, probeNeedingTarget)).toBeNull();
  });
});
