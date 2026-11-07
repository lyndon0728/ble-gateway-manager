// Physical-action form checks: run with `node scripts/test-gateway-inputs.cjs`.
const fs = require('node:fs')
const path = require('node:path')
const vm = require('node:vm')
const assert = require('node:assert/strict')
const crypto = require('node:crypto')
const ts = require('typescript')
const source = fs.readFileSync(path.join(__dirname, '../src/features/gateway/utils.ts'), 'utf8')
const compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText
const context = { exports: {}, require: () => ({ api: () => { throw Error('No network calls in input tests') }, newKey: () => crypto.randomUUID() }), btoa, atob, Date, Error, Uint8Array }
vm.runInNewContext(compiled, context)
const u = context.exports
let count = 0
function test(name, fn) { fn(); count++; process.stdout.write(`PASS ${name}\n`) }
test('16-bit UUID expansion', () => assert.equal(u.canonicalUuid('180F'), '0000180f-0000-1000-8000-00805f9b34fb'))
test('32-bit UUID expansion', () => assert.equal(u.canonicalUuid('12345678'), '12345678-0000-1000-8000-00805f9b34fb'))
test('128-bit UUID normalization', () => assert.equal(u.canonicalUuid('12345678-1234-5678-9ABC-DEF012345678'), '12345678-1234-5678-9abc-def012345678'))
test('invalid UUID is rejected', () => assert.throws(() => u.canonicalUuid('123')))
test('complete AD encoding', () => assert.equal(u.payload('02 01 06', 'HEX', 31, 0, true), 'AgEG'))
test('truncated AD is rejected', () => assert.throws(() => u.payload('03 01', 'HEX', 31, 0, true)))
test('odd hex length is rejected', () => assert.throws(() => u.payload('001', 'HEX', 31)))
test('noncanonical base64 is rejected', () => assert.throws(() => u.payload('AgEG=', 'BASE64', 31)))
test('nonzero bytes after AD terminator are rejected', () => assert.throws(() => u.payload('00 01', 'HEX', 31, 0, true)))
test('actual write MTU is enforced', () => assert.throws(() => u.payload('ab'.repeat(21), 'HEX', 20, 1)))
test('random-static address normalization', () => assert.equal(u.address('c0:00:11:22:33:44', 'RANDOM', true), 'C00011223344'))
test('private random identity rejected for automatic peer', () => assert.throws(() => u.address('40:00:11:22:33:44', 'RANDOM', true)))
test('zero identity rejected', () => assert.throws(() => u.address('00:00:00:00:00:00', 'PUBLIC')))
test('write retries preserve key and exact expiry; changed write gets a new key', () => {
  const intent = u.taskIntent()
  const a = intent({ gatewayId: 'gw', parameters: { value: 'AQ==' } }, 60)
  const b = intent({ gatewayId: 'gw', parameters: { value: 'AQ==' } }, 60)
  const c = intent({ gatewayId: 'gw', parameters: { value: 'Ag==' } }, 60)
  assert.equal(a.key, b.key); assert.equal(a.body.expiresAt, b.body.expiresAt); assert.notEqual(a.key, c.key)
})
test('a confirmed read followed by a new identical read gets a fresh task identity', () => {
  const intent = u.taskIntent(), input = { gatewayId: 'gw', type: 'GATT_READ', parameters: { connectionRef: 'a'.repeat(32), serviceUuid: '180f', characteristicUuid: '2a19' } }
  const first = intent(input)
  intent.clear() // The task-create API returned its durable task identity.
  const second = intent(input)
  assert.notEqual(first.key, second.key)
  assert.equal(JSON.stringify(first.body.parameters), JSON.stringify(second.body.parameters))
})
test('a lost read response retries the same task identity and expiry', () => {
  const intent = u.taskIntent(), input = { gatewayId: 'gw', type: 'GATT_READ', parameters: { connectionRef: 'b'.repeat(32) } }
  const first = intent(input), retry = intent(JSON.parse(JSON.stringify(input)))
  assert.equal(first.key, retry.key); assert.equal(first.body.expiresAt, retry.body.expiresAt)
})
test('artifact retry remains stable across reconstructed multipart objects; new content or explicit new upload gets a new key', () => {
  const intent = u.artifactUploadIntent()
  const input = { kind: 'FIRMWARE', sha256: 'a'.repeat(64), manifest: '{"version":"1.0.1"}\n', signature: 'signed-manifest', file: { name: 'firmware.bin', size: 24, type: 'application/octet-stream', lastModified: 100 } }
  const first = intent(input), retry = intent(JSON.parse(JSON.stringify(input)))
  assert.equal(first, retry)
  assert.notEqual(first, intent({ ...input, manifest: input.manifest + '\n' }))
  const beforeNewUpload = intent(input); intent.clear()
  assert.notEqual(beforeNewUpload, intent(input))
})
const legacyGateway = { model: 'LEGACY_GW', capabilities: ['GATT_READ', 'GATT_WRITE'] }
const gattForm = { type: 'GATT_READ', radio: 'NRF54', connectionRef: '', deviceAddress: 'AA:BB:CC:DD:EE:FF', addressType: 'PUBLIC', serviceUuid: '180f', characteristicUuid: '2a19', timeoutMs: 1000, writeMode: 'WITH_RESPONSE', verification: 'READBACK', value: '2a', encoding: 'HEX' }
test('legacy temporary read follows the current read schema without radio or connection reference', () => {
  const params=u.gattParameters(legacyGateway,gattForm)
  assert.equal(JSON.stringify(Object.keys(params).sort()),JSON.stringify(['addressType','characteristicUuid','deviceAddress','serviceUuid','timeoutMs']))
  assert.equal(params.deviceAddress,'AABBCCDDEEFF');assert.equal(params.serviceUuid,'0000180f-0000-1000-8000-00805f9b34fb')
})
test('legacy write supports its contract modes and enforces the Base64 4096-character limit', () => {
  const params=u.gattParameters(legacyGateway,{...gattForm,type:'GATT_WRITE',writeMode:'WITHOUT_RESPONSE',verification:'APPLICATION_REPLY'})
  assert.equal(params.writeMode,'WITHOUT_RESPONSE');assert.equal(params.verification,'APPLICATION_REPLY');assert.equal(params.valueBase64,'Kg==');assert.equal(params.radio,undefined)
  assert.throws(()=>u.gattParameters(legacyGateway,{...gattForm,type:'GATT_WRITE',value:'2a'.repeat(3073)}))
})
test('legacy gateway cannot use persistent connection, subscriptions or a GW32 connection reference', () => {
  assert.throws(()=>u.gattParameters({...legacyGateway,capabilities:['GATT_CONNECT']},{...gattForm,type:'GATT_CONNECT'}))
  assert.throws(()=>u.gattParameters(legacyGateway,{...gattForm,connectionRef:'a'.repeat(32)}))
})
test('GW32 requires signed profile and radio capabilities and keeps the actual MTU limit', () => {
  const gateway={model:'GW32I33V2',capabilities:['GW32_CONTROL_PROFILE_1','NRF54_BLE_GATT_CENTRAL','GATT_READ','GATT_WRITE']}
  assert.equal(u.gattParameters(gateway,gattForm).radio,'NRF54')
  assert.throws(()=>u.gattParameters({model:'GW32I33V2',capabilities:['GATT_READ']},gattForm))
  assert.throws(()=>u.gattParameters(gateway,{...gattForm,type:'GATT_WRITE',value:'2a'.repeat(21)},{attMtu:23}))
  assert.throws(()=>u.gattParameters(gateway,{...gattForm,type:'GATT_WRITE',verification:'APPLICATION_REPLY'}))
})
process.stdout.write(`${count} gateway input checks passed\n`)
