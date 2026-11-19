import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { webcrypto, createDecipheriv, createCipheriv } from 'node:crypto';
import { bytes,hex,SrpClient,SecureChannel,N,binary,cat,field,parse,sessionMessage,sessionResponse,requestId,ProvisionClient } from '../web/src/security2.js';
globalThis.crypto ||= webcrypto;
const vectors=JSON.parse(readFileSync(new URL('./security2-vectors.json',import.meta.url)));
test('eight upstream Espressif SRP proofs and OpenSSL AES-GCM exchanges',()=>{
  for(const v of vectors){
    const srp=new SrpClient(v.username,v.password,bytes(v.ephemeral));
    assert.equal(hex(binary(srp.A)),v.clientPublic);
    assert.equal(hex(srp.challenge(bytes(v.salt),bytes(v.serverPublic))),v.clientProof);
    const key=srp.verify(bytes(v.serverProof));assert.equal(hex(key),v.key);
    const channel=new SecureChannel(key,bytes(v.nonce));
    assert.equal(hex(channel.encrypt(bytes(v.plaintext))),v.ciphertext);
    assert.equal(hex(channel.decrypt(bytes(v.encryptedReply))),v.reply);
    assert.equal(new DataView(channel.nonce.buffer).getUint32(8),3);
    channel.destroy();assert.throws(()=>channel.encrypt(new Uint8Array()));
  }
});
test('invalid SRP server values and forged server proof are rejected',()=>{
  const v=vectors[0];
  for(const bad of [new Uint8Array(384),binary(N),binary(N+1n)]){
    const client=new SrpClient(v.username,v.password,bytes(v.ephemeral));
    assert.throws(()=>client.challenge(bytes(v.salt),bad));
  }
  const client=new SrpClient(v.username,v.password,bytes(v.ephemeral));
  client.challenge(bytes(v.salt),bytes(v.serverPublic));assert.throws(()=>client.verify(new Uint8Array(64)));
  assert.ok(client.K.every(x=>x===0));
});
test('device C preserves leading-zero identity hash and salt bytes',()=>{
  const edges=JSON.parse(readFileSync(new URL('./security2-device-edge-vectors.json',import.meta.url)));
  for(const v of edges){const client=new SrpClient(v.username,v.password,bytes(v.ephemeral));assert.equal(hex(client.challenge(bytes(v.salt),bytes(v.serverPublic))),v.clientProof);assert.equal(hex(client.verify(bytes(v.serverProof))),v.key);}
});
test('tag corruption, replay, nonce overflow and obsolete fixed nonce scheme fail closed',()=>{
  const v=vectors[0];
  const channel=new SecureChannel(bytes(v.key),bytes(v.nonce));channel.encrypt(bytes(v.plaintext));
  const tampered=bytes(v.encryptedReply);tampered[0]^=1;assert.throws(()=>channel.decrypt(tampered));assert.ok(channel.failed);
  const replay=new SecureChannel(bytes(v.key),bytes(v.nonce));replay.encrypt(bytes(v.plaintext));replay.decrypt(bytes(v.encryptedReply));assert.throws(()=>replay.decrypt(bytes(v.encryptedReply)));
  const overflow=new SecureChannel(bytes(v.key),bytes(v.nonce));new DataView(overflow.nonce.buffer).setUint32(8,0xffffffff);assert.throws(()=>overflow.encrypt(new Uint8Array()));
  assert.throws(()=>new SecureChannel(bytes(v.key),new Uint8Array(12)));
});
test('protobuf matches published field numbers and rejects malformed/duplicate records',()=>{
  const command=sessionMessage(0,cat(field(1,new TextEncoder().encode('a')),field(2,new Uint8Array(384))));
  const outer=parse(command);assert.equal(outer.get(2),2);const inner=parse(outer.get(12));assert.equal(inner.get(1),0);assert.equal(parse(inner.get(20)).get(2).length,384);
  const response=cat(field(2,2),field(12,cat(field(1,3),field(23,cat(field(2,new Uint8Array(64)),field(3,new Uint8Array(12)))))));
  assert.equal(sessionResponse(response,3).get(2).length,64);
  for(const invalid of [bytes('10021002'),bytes('62ff'),bytes('0f'),new Uint8Array(2049)])assert.throws(()=>parse(invalid));
  assert.throws(()=>sessionResponse(response,1));
});
test('request IDs use browser secure randomness and RFC4122 variant',()=>{
  const ids=new Set(Array.from({length:100},()=>requestId()));assert.equal(ids.size,100);
  for(const id of ids)assert.match(id,/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
});
test('protected HTTP JSON uses a sequential nonce and matches the response request',async()=>{
  const original=globalThis.fetch,v=vectors[0],client=new ProvisionClient();
  client.channel=new SecureChannel(bytes(v.key),bytes(v.nonce));
  const request={apiVersion:'1.0',requestId:requestId(),operation:'GET_STATE',body:{}};
  globalThis.fetch=async(path,options)=>{
    assert.equal(path,'/prov-network');assert.equal(options.headers['Content-Type'],'application/octet-stream');
    const input=Buffer.from(options.body),decrypt=createDecipheriv('aes-256-gcm',bytes(v.key),bytes(v.nonce));
    decrypt.setAuthTag(input.subarray(-16));assert.deepEqual(JSON.parse(Buffer.concat([decrypt.update(input.subarray(0,-16)),decrypt.final()])),request);
    const nonce=bytes(v.nonce);new DataView(nonce.buffer).setUint32(8,2);
    const encrypt=createCipheriv('aes-256-gcm',bytes(v.key),nonce),response={requestId:request.requestId,code:'OK',body:{networkGeneration:'18446744073709551614'}};
    const ciphertext=Buffer.concat([encrypt.update(JSON.stringify(response)),encrypt.final(),encrypt.getAuthTag()]);
    return new Response(ciphertext,{status:200});
  };
  try{const result=await client.call(request);assert.equal(result.body.networkGeneration,'18446744073709551614');}
  finally{globalThis.fetch=original;client.channel?.destroy();}
});
test('lost encrypted response invalidates session; ciphertext is never automatically replayed',async()=>{
  const original=globalThis.fetch,v=vectors[0],client=new ProvisionClient();let sends=0;
  client.channel=new SecureChannel(bytes(v.key),bytes(v.nonce));
  globalThis.fetch=async()=>{sends++;throw Error('response lost after remote execution');};
  try{await assert.rejects(client.call({requestId:'stable-id',operation:'APPLY_ONLINE'}));assert.equal(client.channel,null);await assert.rejects(client.call({requestId:'stable-id',operation:'APPLY_ONLINE'}));assert.equal(sends,1);}
  finally{globalThis.fetch=original;}
});
