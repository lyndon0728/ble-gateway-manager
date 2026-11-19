import test from 'node:test';
import assert from 'node:assert/strict';
import { readVerification, checkedOffset, encodeChunk } from '../web/src/ota.js';

const manifest = { kind:'FIRMWARE', component:'GATEWAY', imageFormat:'ESP_IDF_APP_BIN', sizeBytes:8193, version:'0.2.0', keyId:'fixture' };
const signature = Buffer.alloc(64, 1).toString('base64');
const pack = m => JSON.stringify({ manifest: JSON.stringify(m), signature });

test('local package preserves exact signed manifest and rejects unsuitable image selection', () => {
  const raw = JSON.stringify(manifest, null, 2) + '\n';
  assert.deepEqual(readVerification(JSON.stringify({manifest:raw,signature}),8193),{manifest:raw,signature});
  assert.throws(()=>readVerification(pack(manifest),8192),/长度/);
  assert.throws(()=>readVerification(pack({...manifest,sizeBytes:0x280001}),0x280001),/容量/);
  assert.throws(()=>readVerification(pack({...manifest,sizeBytes:1.1}),1.1),/长度/);
  assert.throws(()=>readVerification(pack({...manifest,component:'NRF54'}),8193),/ESP32/);
  assert.throws(()=>readVerification(pack({...manifest,imageFormat:'FACTORY_BIN'}),8193),/ESP32/);
  assert.throws(()=>readVerification(JSON.stringify({manifest:raw,signature:'unsigned'}),8193),/signature/);
  assert.throws(()=>readVerification(JSON.stringify({manifest:' '.repeat(8193),signature}),8193),/manifest/);
});

test('resumption rejects another upload, invalid offsets and changed file size', () => {
  const status = {uploadId:'u1',offset:4096,sizeBytes:8193};
  assert.equal(checkedOffset(status,'u1',8193),4096);
  for(const changed of [{uploadId:'u2'},{offset:-1},{offset:8194},{offset:1.5},{offset:'4096'},{sizeBytes:8192}])
    assert.throws(()=>checkedOffset({...status,...changed},'u1',8193),/不匹配/);
});

test('base64 firmware chunks preserve every byte and bound request sizes', () => {
  const bytes=Uint8Array.from({length:4096},(_,i)=>i%256);
  assert.deepEqual(Buffer.from(encodeChunk(bytes),'base64'),Buffer.from(bytes));
  assert.ok(JSON.stringify({operation:'OTA_WRITE',body:{uploadId:'x'.repeat(64),offset:0,data:encodeChunk(bytes)}}).length<16384);
  assert.throws(()=>encodeChunk(new Uint8Array(0)),/长度/);
  assert.throws(()=>encodeChunk(new Uint8Array(4097)),/长度/);
});
