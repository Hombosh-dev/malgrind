const FLAME_SVG_STYLE = [
  'svg { background: #ffffff; color: #1a1208; }',
  '.d3-flame-graph rect { stroke: #eeeeee; fill-opacity: 0.8; }',
  '.d3-flame-graph .frame:hover rect { stroke: #474747; stroke-width: 0.5; }',
  '.d3-flame-graph-label {',
  '  pointer-events: none;',
  '  white-space: nowrap;',
  '  text-overflow: ellipsis;',
  '  overflow: hidden;',
  '  font-size: 12px;',
  "  font-family: system-ui, -apple-system, 'Segoe UI', Verdana, sans-serif;",
  '  margin-left: 4px;',
  '  margin-right: 4px;',
  '  line-height: 1.5;',
  '  padding: 0;',
  '  font-weight: 400;',
  '  color: #1a1208;',
  '  text-align: left;',
  '}',
  '.d3-flame-graph .fade { opacity: 0.6 !important; }',
  ".d3-flame-graph .title { font-size: 20px; font-family: system-ui, -apple-system, 'Segoe UI', Verdana, sans-serif; fill: #1a1208; }",
].join('\n');

const SVG_NS = 'http://www.w3.org/2000/svg';

function parsePx(value: string | null): number {
  if (!value || value.includes('%')) return 0;
  const n = Number.parseFloat(value);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

function svgDimensions(svg: SVGSVGElement): { width: number; height: number } {
  const attrW = parsePx(svg.getAttribute('width'));
  const attrH = parsePx(svg.getAttribute('height'));
  const rect = typeof svg.getBoundingClientRect === 'function' ? svg.getBoundingClientRect() : null;
  const viewBox = svg.viewBox?.baseVal;

  const width =
    attrW ||
    (rect && Number.isFinite(rect.width) && rect.width > 0 ? rect.width : 0) ||
    (viewBox && Number.isFinite(viewBox.width) && viewBox.width > 0 ? viewBox.width : 0) ||
    800;
  const height =
    attrH ||
    (rect && Number.isFinite(rect.height) && rect.height > 0 ? rect.height : 0) ||
    (viewBox && Number.isFinite(viewBox.height) && viewBox.height > 0 ? viewBox.height : 0) ||
    400;

  return { width: Math.ceil(width), height: Math.ceil(height) };
}

export function bytesToBase64(bytes: Uint8Array): string {
  const chunkSize = 0x4000; // keep argument counts well below JS engine call-stack limits
  const parts: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    parts.push(String.fromCharCode(...bytes.subarray(i, i + chunkSize)));
  }
  return btoa(parts.join(''));
}

export function defaultExportFilename(runNameOrId: string, extension: string): string {
  const stem = runNameOrId
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]+/g, '-')
    .replace(/\s+/g, ' ')
    .replace(/^[.\s-]+|[.\s-]+$/g, '');
  const safeStem = stem || 'malgrind-run';
  const ext = extension.replace(/^\.+/, '');
  return ext ? `${safeStem}.${ext}` : safeStem;
}

export function serializeFlameSvg(svg: SVGSVGElement): string {
  const { width, height } = svgDimensions(svg);
  const clone = svg.cloneNode(true) as SVGSVGElement;
  clone.setAttribute('xmlns', SVG_NS);
  clone.setAttribute('width', String(width));
  clone.setAttribute('height', String(height));
  if (!clone.getAttribute('viewBox')) clone.setAttribute('viewBox', `0 0 ${width} ${height}`);

  const doc = svg.ownerDocument ?? document;
  const style = doc.createElementNS(SVG_NS, 'style');
  style.textContent = FLAME_SVG_STYLE;
  clone.insertBefore(style, clone.firstChild);

  return new XMLSerializer().serializeToString(clone);
}

function loadSvgImage(markup: string, width: number, height: number): Promise<HTMLImageElement> {
  const svgBytes = new TextEncoder().encode(markup);
  const src = `data:image/svg+xml;base64,${bytesToBase64(svgBytes)}`;
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.width = width;
    img.height = height;
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error('Failed to render flamegraph SVG to image'));
    img.src = src;
  });
}

export async function svgToPngBase64(svg: SVGSVGElement, scale = 2): Promise<string> {
  const { width, height } = svgDimensions(svg);
  const outWidth = Math.max(1, Math.round(width * scale));
  const outHeight = Math.max(1, Math.round(height * scale));
  const markup = serializeFlameSvg(svg);
  const img = await loadSvgImage(markup, outWidth, outHeight);

  const canvas = (svg.ownerDocument ?? document).createElement('canvas');
  canvas.width = outWidth;
  canvas.height = outHeight;
  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('Could not create 2D canvas context');

  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, outWidth, outHeight);
  ctx.drawImage(img, 0, 0, outWidth, outHeight);

  const blob = await new Promise<Blob>((resolve, reject) => {
    canvas.toBlob((b) => (b ? resolve(b) : reject(new Error('Could not encode PNG image'))), 'image/png');
  });
  return bytesToBase64(new Uint8Array(await blob.arrayBuffer()));
}
