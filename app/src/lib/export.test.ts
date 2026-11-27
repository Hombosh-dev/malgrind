import { afterEach, describe, expect, it, vi } from 'vitest';
import { bytesToBase64, defaultExportFilename, serializeFlameSvg } from './export';

class FakeElement {
  readonly attrs = new Map<string, string>();
  readonly children: FakeElement[] = [];
  textContent: string | null = null;
  rect = { width: 0, height: 0 };
  viewBox?: { baseVal: { width: number; height: number } };
  ownerDocument: { createElementNS(ns: string, tag: string): FakeElement };

  constructor(
    readonly tagName: string,
    readonly namespaceURI: string,
    doc?: { createElementNS(ns: string, tag: string): FakeElement },
  ) {
    this.ownerDocument = doc ?? {
      createElementNS: (ns: string, tag: string) => new FakeElement(tag, ns, this.ownerDocument),
    };
  }

  get firstChild(): FakeElement | null {
    return this.children[0] ?? null;
  }

  setAttribute(name: string, value: string): void {
    this.attrs.set(name, value);
  }

  getAttribute(name: string): string | null {
    return this.attrs.get(name) ?? null;
  }

  appendChild(child: FakeElement): FakeElement {
    this.children.push(child);
    return child;
  }

  insertBefore(newNode: FakeElement, refNode: FakeElement | null): FakeElement {
    const idx = refNode ? this.children.indexOf(refNode) : -1;
    if (idx === -1) this.children.push(newNode);
    else this.children.splice(idx, 0, newNode);
    return newNode;
  }

  cloneNode(deep = false): FakeElement {
    const copy = new FakeElement(this.tagName, this.namespaceURI, this.ownerDocument);
    for (const [k, v] of this.attrs) copy.attrs.set(k, v);
    copy.textContent = this.textContent;
    copy.rect = { ...this.rect };
    copy.viewBox = this.viewBox ? { baseVal: { ...this.viewBox.baseVal } } : undefined;
    if (deep) {
      for (const child of this.children) copy.children.push(child.cloneNode(true));
    }
    return copy;
  }

  getBoundingClientRect(): { width: number; height: number } {
    return this.rect;
  }

  toXml(): string {
    const attrs = Array.from(this.attrs.entries())
      .map(([k, v]) => ` ${k}="${v.replace(/"/g, '&quot;')}"`)
      .join('');
    const body = this.children.map((c) => c.toXml()).join('') + (this.textContent ?? '');
    return `<${this.tagName}${attrs}>${body}</${this.tagName}>`;
  }
}

class FakeXMLSerializer {
  serializeToString(node: FakeElement): string {
    return node.toXml();
  }
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('bytesToBase64', () => {
  it('encodes empty, binary, and large buffers without overflowing the call stack', () => {
    expect(bytesToBase64(new Uint8Array())).toBe('');
    expect(bytesToBase64(new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x01, 0xff, 0x80]))).toBe(
      'iVBORw0KGgoAAf+A',
    );
    expect(bytesToBase64(new Uint8Array([0x00, 0xff]))).toBe('AP8=');
    expect(bytesToBase64(new Uint8Array([0x41]))).toBe('QQ==');

    const big = new Uint8Array(200_000);
    for (let i = 0; i < big.length; i += 1) big[i] = i & 0xff;
    const encoded = bytesToBase64(big);
    const decoded = atob(encoded);
    expect(decoded.length).toBe(big.length);
    for (let i = 0; i < 64; i += 1) expect(decoded.charCodeAt(i)).toBe(big[i]);
    expect(decoded.charCodeAt(big.length - 1)).toBe(big[big.length - 1]);
  });
});

describe('defaultExportFilename', () => {
  it('sanitizes unsafe characters and falls back cleanly', () => {
    expect(defaultExportFilename('water-sim 26 Nov', 'folded')).toBe('water-sim 26 Nov.folded');
    expect(defaultExportFilename('foo/bar:baz*qux?"<>|', '.speedscope.json')).toBe('foo-bar-baz-qux.speedscope.json');
    expect(defaultExportFilename('a\x00b\x1fc', 'svg')).toBe('a-b-c.svg');
    expect(defaultExportFilename('  ...  ', 'png')).toBe('malgrind-run.png');
    expect(defaultExportFilename('///', '.folded')).toBe('malgrind-run.folded');
  });
});

describe('serializeFlameSvg', () => {
  it('clones svg, ensures xmlns and explicit size, and injects readable flamegraph styles', () => {
    vi.stubGlobal('XMLSerializer', FakeXMLSerializer);

    const svg = new FakeElement('svg', 'http://www.w3.org/2000/svg');
    svg.setAttribute('class', 'd3-flame-graph');
    svg.setAttribute('width', '960');
    svg.setAttribute('height', '320');

    const g = svg.ownerDocument.createElementNS('http://www.w3.org/2000/svg', 'g');
    g.setAttribute('class', 'frame');
    const rect = svg.ownerDocument.createElementNS('http://www.w3.org/2000/svg', 'rect');
    rect.setAttribute('fill', '#e67e22');
    const fo = svg.ownerDocument.createElementNS('http://www.w3.org/2000/svg', 'foreignObject');
    const label = svg.ownerDocument.createElementNS('http://www.w3.org/1999/xhtml', 'div');
    label.setAttribute('class', 'd3-flame-graph-label');
    label.textContent = 'app_main';
    fo.appendChild(label);
    g.appendChild(rect);
    g.appendChild(fo);
    svg.appendChild(g);

    const serialized = serializeFlameSvg(svg as unknown as SVGSVGElement);

    // Original node must remain unmodified.
    expect(svg.getAttribute('xmlns')).toBeNull();
    expect(svg.children.length).toBe(1);

    expect(serialized).toContain('xmlns="http://www.w3.org/2000/svg"');
    expect(serialized).toContain('width="960"');
    expect(serialized).toContain('height="320"');
    expect(serialized).toContain('viewBox="0 0 960 320"');
    expect(serialized).toContain('<style>');
    expect(serialized).toContain('.d3-flame-graph rect');
    expect(serialized).toContain('.d3-flame-graph-label');
    expect(serialized).toContain('color: #1a1208');
    expect(serialized).toContain('app_main');
    expect(serialized.indexOf('<style>')).toBeLessThan(serialized.indexOf('<g'));
  });

  it('uses bounding rect or viewBox when pixel attributes are absent or percentage', () => {
    vi.stubGlobal('XMLSerializer', FakeXMLSerializer);

    const svg = new FakeElement('svg', 'http://www.w3.org/2000/svg');
    svg.setAttribute('width', '100%');
    svg.rect = { width: 640.2, height: 0 };
    svg.viewBox = { baseVal: { width: 500, height: 250.4 } };

    const serialized = serializeFlameSvg(svg as unknown as SVGSVGElement);
    expect(serialized).toContain('width="641"');
    expect(serialized).toContain('height="251"');
  });
});
