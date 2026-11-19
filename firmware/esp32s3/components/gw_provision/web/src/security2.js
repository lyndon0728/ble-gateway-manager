// SPDX-License-Identifier: Apache-2.0
// SRP wire compatibility derived from ESP-IDF v5.5.6 tools/esp_prov/security/
// srp6a.py and components/protocomm/src/crypto/srp6a/esp_srp.c.
// Copyright 2022-2026 Espressif Systems (Shanghai) CO LTD (reference algorithm).
// Hash and authenticated encryption are supplied by pinned Noble libraries.
import { sha512 } from '../vendor/hashes/sha2.js';
import { gcm } from '../vendor/ciphers/aes.js';

export const utf8 = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
export const N = BigInt('0x' +
  'FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E' +
  '3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE' +
  '9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED5290770' +
  '96966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF69558171839' +
  '95497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6' +
  'E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C77' +
  '0988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF');
const G = 5n;
export function cat(...parts) {
  const result = new Uint8Array(parts.reduce((n, x) => n + x.length, 0));
  let at = 0; for (const part of parts) { result.set(part, at); at += part.length; }
  return result;
}
export const hex = b => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
export function bytes(hexString) {
  if (!/^(?:[0-9a-f]{2})*$/i.test(hexString)) throw Error('Invalid hexadecimal input');
  return Uint8Array.from(hexString.match(/../g) || [], x => parseInt(x, 16));
}
export function integer(data) { return BigInt('0x' + (hex(data) || '00')); }
export function binary(value, length = 0) {
  if (value < 0n) throw Error('Negative integer');
  let h = value.toString(16); if (h.length % 2) h = '0' + h;
  if (length && h.length > length * 2) throw Error('Integer overflow');
  return bytes(h.padStart(length * 2, '0'));
}
function mod(a) { return (a % N + N) % N; }
export function pow(base, exponent) {
  let result = 1n; base = mod(base);
  for (; exponent; exponent >>= 1n, base = mod(base * base)) if (exponent & 1n) result = mod(result * base);
  return result;
}
const digest = (...parts) => sha512(cat(...parts));
function equal(a, b) {
  if (a.length !== b.length) return false;
  let different = 0; for (let i = 0; i < a.length; ++i) different |= a[i] ^ b[i];
  return different === 0;
}
export function randomBytes(length) {
  if (!globalThis.crypto?.getRandomValues) throw Error('此浏览器缺少安全随机数，请使用完整的现代浏览器。');
  return globalThis.crypto.getRandomValues(new Uint8Array(length));
}
export function requestId() {
  const b = randomBytes(16); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
  const h = hex(b); return `${h.slice(0,8)}-${h.slice(8,12)}-${h.slice(12,16)}-${h.slice(16,20)}-${h.slice(20)}`;
}

export class SrpClient {
  constructor(username, password, ephemeralForTest) {
    if (!username || utf8.encode(username).length > 64 || !password || utf8.encode(password).length > 128) throw Error('账号或口令长度无效');
    this.username = username; this.password = password;
    for (let attempts = 0; attempts < 100; attempts++) {
      const r = ephemeralForTest ? new Uint8Array(ephemeralForTest) : randomBytes(32);
      if (r.length !== 32) throw Error('Invalid ephemeral');
      r[0] |= 128; this.a = integer(r); r.fill(0); this.A = pow(G, this.a);
      // IDF hashes the full received 384-byte A. Avoid shortened public A.
      if (binary(this.A).length === 384) return;
      if (ephemeralForTest) throw Error('Test public key has leading zero');
    }
    throw Error('SRP ephemeral generation failed');
  }
  challenge(salt, serverPublic) {
    if (salt.length !== 16 || serverPublic.length < 1 || serverPublic.length > 384) throw Error('Invalid SRP challenge');
    const B = integer(serverPublic);
    if (B <= 0n || B >= N) throw Error('Unsafe SRP server public key');
    const k = integer(digest(binary(N, 384), binary(G, 384)));
    const u = integer(digest(binary(this.A, 384), binary(B, 384)));
    if (u === 0n) throw Error('Unsafe SRP scrambling parameter');
    // Preserve fixed hash bytes and salt including leading zero bytes as IDF C does.
    const x = integer(digest(salt, digest(utf8.encode(`${this.username}:${this.password}`))));
    this.password = ''; const S = pow(B - k * pow(G, x), this.a + u * x);
    if (S === 0n) throw Error('Unsafe SRP shared secret');
    this.K = digest(binary(S));
    const hn = digest(binary(N)), hg = digest(binary(G, 384));
    const hx = Uint8Array.from(hn, (b, i) => b ^ hg[i]);
    this.M = digest(hx, digest(utf8.encode(this.username)), salt, binary(this.A, 384), serverPublic, this.K);
    this.expectedProof = digest(binary(this.A, 384), this.M, this.K);
    this.a = 0n;
    return this.M;
  }
  verify(proof) {
    if (!this.expectedProof || !equal(proof, this.expectedProof)) { this.destroy(); throw Error('设备认证失败，请检查设备和本地口令。'); }
    const key = this.K.slice(0, 32); this.destroy(); return key;
  }
  destroy() { this.password = ''; this.a = 0n; this.K?.fill(0); this.M?.fill(0); this.expectedProof?.fill(0); }
}

function vint(value) {
  const out = []; do { const low = value & 127; value = Math.floor(value / 128); out.push(low | (value ? 128 : 0)); } while (value);
  return new Uint8Array(out);
}
export function field(id, value) {
  return typeof value === 'number' ? cat(vint(id * 8), vint(value)) : cat(vint(id * 8 + 2), vint(value.length), value);
}
export function parse(data) {
  if (!(data instanceof Uint8Array) || data.length > 2048) throw Error('Invalid protobuf size');
  let at = 0; const result = new Map();
  function readVarint() {
    let n = 0, multiplier = 1;
    for (let i = 0; i < 5; i++) { if (at >= data.length) throw Error('Truncated protobuf'); const b = data[at++]; n += (b & 127) * multiplier; if (!(b & 128)) return n; multiplier *= 128; }
    throw Error('Protobuf integer too large');
  }
  while (at < data.length) {
    const tag = readVarint(), id = Math.floor(tag / 8), wire = tag & 7;
    if (!id || result.has(id)) throw Error('Duplicate protobuf field');
    if (wire === 0) result.set(id, readVarint());
    else if (wire === 2) { const n = readVarint(); if (n > data.length - at) throw Error('Truncated protobuf payload'); result.set(id, data.slice(at, at += n)); }
    else throw Error('Unsupported protobuf wire type');
  }
  return result;
}
export function sessionMessage(type, payload) { return cat(field(2, 2), field(12, cat(field(1, type), field(type === 0 ? 20 : 22, payload)))); }
export function sessionResponse(data, expectedType) {
  const outer = parse(data); if (outer.get(2) !== 2 || !(outer.get(12) instanceof Uint8Array)) throw Error('Security 2 required');
  const inner = parse(outer.get(12)); if (inner.get(1) !== expectedType) throw Error('Unexpected handshake response');
  const body = parse(inner.get(expectedType === 1 ? 21 : 23));
  if ((body.get(1) || 0) !== 0) throw Error('设备拒绝认证');
  return body;
}
export class SecureChannel {
  constructor(key, nonce) {
    if (key.length !== 32 || nonce.length !== 12 || new DataView(nonce.buffer, nonce.byteOffset).getUint32(8) !== 1) throw Error('Security 2 patch 1 nonce required');
    this.key = new Uint8Array(key); this.nonce = new Uint8Array(nonce); this.failed = false;
  }
  crypt(data, encrypt) {
    if (this.failed) throw Error('会话已失效，请重新登录后查询原事务。');
    const view = new DataView(this.nonce.buffer), n = view.getUint32(8);
    if (n === 0xffffffff) { this.destroy(); throw Error('Session counter exhausted'); }
    try {
      const cipher = gcm(this.key, this.nonce), out = encrypt ? cipher.encrypt(data) : cipher.decrypt(data);
      view.setUint32(8, n + 1); return out;
    } catch (error) { this.destroy(); throw error; }
  }
  encrypt(data) { return this.crypt(data, true); }
  decrypt(data) { return this.crypt(data, false); }
  destroy() { this.failed = true; this.key.fill(0); this.nonce.fill(0); }
}

export class ProvisionClient {
  constructor() { this.channel = null; this.busy = false; }
  async post(path, data) {
    const abort = new AbortController(), timer = setTimeout(() => abort.abort(), 15000);
    try {
      const response = await fetch(path, { method:'POST', credentials:'same-origin', cache:'no-store', headers:{'Content-Type':'application/octet-stream'}, body:data, signal:abort.signal });
      if (!response.ok) throw Error(`设备请求失败 (${response.status})，重新登录后查询原事务。`);
      const body = new Uint8Array(await response.arrayBuffer());
      if (body.length > 32784) throw Error('Response too large'); return body;
    } finally { clearTimeout(timer); }
  }
  async login(username, password) {
    this.channel?.destroy(); this.channel = null;
    const info = await (await fetch('/prov-info', { cache:'no-store' })).json();
    if (info.securityVersion !== 2 || info.securityPatchVersion !== 1) throw Error('设备不支持所需安全协议');
    const srp = new SrpClient(username, password);
    try {
      const first = await this.post('/prov-session', sessionMessage(0, cat(field(1, utf8.encode(username)), field(2, binary(srp.A, 384)))));
      const r0 = sessionResponse(first, 1), proof = srp.challenge(r0.get(3), r0.get(2));
      const second = await this.post('/prov-session', sessionMessage(2, field(1, proof)));
      const r1 = sessionResponse(second, 3), key = srp.verify(r1.get(2));
      this.channel = new SecureChannel(key, r1.get(3)); key.fill(0);
    } finally { srp.destroy(); }
  }
  async call(request) {
    if (this.busy) throw Error('请等待当前操作完成');
    if (!this.channel) throw Error('请先登录');
    this.busy = true;
    try {
      const result = this.channel.decrypt(await this.post('/prov-network', this.channel.encrypt(utf8.encode(JSON.stringify(request)))));
      const json = JSON.parse(decoder.decode(result)); result.fill(0);
      if (json.requestId !== request.requestId) throw Error('响应与请求不匹配');
      return json;
    } catch (error) { this.channel?.destroy(); this.channel = null; throw error; }
    finally { this.busy = false; }
  }
}
