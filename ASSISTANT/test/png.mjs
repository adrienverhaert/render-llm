// Minimal, dependency-free PNG encoder used only by the test fake-editor to
// produce a real, valid, non-black PNG for assistant.camera_preview so tests
// can assert on actual width/height/pixel content instead of a stub file.

import zlib from 'node:zlib';

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const typeBuf = Buffer.from(type, 'ascii');
  const lenBuf = Buffer.alloc(4);
  lenBuf.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([typeBuf, data]);
  const crcBuf = Buffer.alloc(4);
  crcBuf.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([lenBuf, body, crcBuf]);
}

/** Build a valid 8-bit RGB PNG of `width`x`height`, with a mild gradient around (r,g,b). */
export function makeSolidPng(width, height, r, g, b) {
  const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

  const ihdrData = Buffer.alloc(13);
  ihdrData.writeUInt32BE(width, 0);
  ihdrData.writeUInt32BE(height, 4);
  ihdrData[8] = 8; // bit depth
  ihdrData[9] = 2; // color type: RGB
  ihdrData[10] = 0;
  ihdrData[11] = 0;
  ihdrData[12] = 0;
  const ihdr = chunk('IHDR', ihdrData);

  const rowBytes = width * 3;
  const raw = Buffer.alloc((rowBytes + 1) * height);
  for (let y = 0; y < height; y++) {
    const rowStart = y * (rowBytes + 1);
    raw[rowStart] = 0; // filter type 0 (none)
    for (let x = 0; x < width; x++) {
      const px = rowStart + 1 + x * 3;
      raw[px] = Math.min(255, r + (x % 40));
      raw[px + 1] = g;
      raw[px + 2] = Math.max(0, b - (y % 30));
    }
  }
  const idat = chunk('IDAT', zlib.deflateSync(raw));
  const iend = chunk('IEND', Buffer.alloc(0));
  return Buffer.concat([sig, ihdr, idat, iend]);
}

/**
 * Read back width/height/mean-channel-value from a PNG generated with
 * makeSolidPng (bit depth 8, color type 2 = RGB, filter 0 = none only).
 * This is a real chunk walk + zlib.inflateSync, not a byte-offset guess, so
 * "not completely black" checks are against actual decoded pixels.
 */
export function inspectSolidPng(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG (bad signature)');
  const width = buf.readUInt32BE(16);
  const height = buf.readUInt32BE(20);
  const bitDepth = buf[24];
  const colorType = buf[25];
  if (bitDepth !== 8 || colorType !== 2) {
    throw new Error(`inspectSolidPng only understands 8-bit RGB, got depth=${bitDepth} colorType=${colorType}`);
  }

  const idatParts = [];
  let offset = 8;
  while (offset < buf.length) {
    const len = buf.readUInt32BE(offset);
    const type = buf.toString('ascii', offset + 4, offset + 8);
    const data = buf.subarray(offset + 8, offset + 8 + len);
    if (type === 'IDAT') idatParts.push(data);
    if (type === 'IEND') break;
    offset += 12 + len; // length + type + data + crc
  }

  const raw = zlib.inflateSync(Buffer.concat(idatParts));
  const rowBytes = width * 3;
  let sum = 0;
  let n = 0;
  for (let y = 0; y < height; y++) {
    const rowStart = y * (rowBytes + 1) + 1; // skip the per-row filter-type byte
    for (let x = 0; x < rowBytes; x++) {
      sum += raw[rowStart + x];
      n++;
    }
  }
  return { width, height, meanChannel: n ? sum / n : 0 };
}
