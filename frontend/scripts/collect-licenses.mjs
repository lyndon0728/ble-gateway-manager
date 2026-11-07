// First run: pnpm licenses list --prod --json > <inventory.json>
import fs from 'node:fs/promises'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..')
const inventory=JSON.parse((await fs.readFile(process.argv[2],'utf8')).replace(/^\uFEFF/,''))
const manifest=[],texts=[]
for(const [kind,packages]of Object.entries(inventory))for(const pkg of packages){
 manifest.push({name:pkg.name,versions:pkg.versions,license:kind,homepage:pkg.homepage||''})
 const found=new Set()
 for(const dir of pkg.paths){const files=(await fs.readdir(dir)).filter(n=>/^(licen[sc]e|copying|notice)(\.|$)/i.test(n));for(const filename of files){const full=path.join(dir,filename);if((await fs.stat(full)).isFile())found.add((await fs.readFile(full,'utf8')).replace(/\r\n/g,'\n').replace(/[\t ]+$/gm,'').trim())}}
 texts.push(`${pkg.name} (${pkg.versions.join(', ')}) — ${kind}\n${[...found].join('\n\n')||'License identifier supplied by the package metadata; see project homepage: '+(pkg.homepage||pkg.name)}`)
}
manifest.sort((a,b)=>a.name.localeCompare(b.name));texts.sort()
await fs.writeFile(path.join(root,'server/frontend-dependencies.json'),JSON.stringify({generatedFrom:'frontend/pnpm-lock.yaml',packages:manifest},null,2)+'\n')
await fs.writeFile(path.join(root,'frontend/public/third-party-licenses.txt'),'Frontend third-party licenses\nBuild/runtime dependency inventory; not every package contributes runtime code.\n\n'+texts.join('\n\n'+'='.repeat(80)+'\n\n')+'\n')
console.log(`Saved notices for ${manifest.length} frontend dependency entries; no local installation paths included.`)
