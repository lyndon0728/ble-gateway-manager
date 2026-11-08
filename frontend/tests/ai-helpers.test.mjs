import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs/promises'
import ts from 'typescript'

const source=(await fs.readFile(new URL('../src/features/ai/helpers.ts',import.meta.url),'utf8')).replace("import {api} from '@/lib/api'","const api=async()=>{throw new Error('not invoked in pure helper test')}")
const {outputText}=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}})
const helpers=await import('data:text/javascript;base64,'+Buffer.from(outputText).toString('base64'))

test('import return links cannot redirect outside authorized console entry pages',()=>{
 for(const link of ['https://example.test','//example.test','javascript:alert(1)','/principals','/system/ai','/targets/../principals','/targets/a#fragment']){
  assert.equal(helpers.safeReturnTo(link),'/device-models')
 }
 assert.equal(helpers.safeReturnTo('/targets/target_demo?version=1'),'/targets/target_demo?version=1')
 assert.equal(helpers.safeReturnTo('/gatt-connections?gatewayId=gw_demo'),'/gatt-connections?gatewayId=gw_demo')
})
test('packet encoding rejects incomplete bytes and preserves all byte values',()=>{
 for(const value of ['','F','GG','01 ZZ'])assert.throws(()=>helpers.hexToBase64(value))
 const raw=Array.from({length:256},(_,i)=>i.toString(16).padStart(2,'0')).join(' ')
 assert.equal(helpers.base64ToHex(helpers.hexToBase64(raw)),raw.toUpperCase())
})
test('editing an AI draft does not mutate the fetched publication or omit required sections',()=>{
 const model={modelName:'Temperature',parserSpec:{fields:[{name:'temperature'}]},tests:[{expected:[{temperature:20}]}]}
 const draft=helpers.normalizeContent(model)
 draft.parserSpec.fields[0].name='humidity';draft.tests[0].expected[0].temperature=21
 assert.equal(model.parserSpec.fields[0].name,'temperature')
 assert.equal(model.tests[0].expected[0].temperature,20)
 assert.deepEqual(draft.sources,[]);assert.equal(draft.connectionSpec,null)
})
test('malformed model field structures remain reviewable without entering unsafe field controls',()=>{
 for(const parserSpec of [{},{fields:{}},{fields:[null]}])assert.ok(helpers.draftStructureError({parserSpec}))
 assert.ok(helpers.draftStructureError({mappingSpec:{fields:[1]}}))
 assert.equal(helpers.draftStructureError({parserSpec:{fields:[]}}),'')
})
test('invalid advanced review sections are rejected before replacing the editable draft',()=>{
 for(const key of ['tests','sources','issues'])for(const value of [{},[null],[1],[[]]])assert.throws(()=>helpers.normalizeContent({[key]:value}),/对象数组/)
})
