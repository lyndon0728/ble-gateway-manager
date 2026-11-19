import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
const here = fileURLToPath(new URL('.', import.meta.url));
const executable = process.env.ESBUILD || fileURLToPath(new URL('../../../../.tools/esp-provision-web/esbuild/package/esbuild.exe', import.meta.url));
const version=spawnSync(executable,['--version'],{encoding:'utf8'});
if(version.error||version.stdout.trim()!=='0.25.5'){console.error('Build requires esbuild 0.25.5; set ESBUILD to its executable.');process.exit(1);}
const args = ['web/src/app.js','--bundle','--minify','--format=iife','--platform=browser','--target=es2020','--outfile=assets/app.js','--legal-comments=inline','--alias:@noble/hashes/crypto=./web/vendor/hashes/crypto.js'];
const result = spawnSync(executable,args,{cwd:here,stdio:'inherit'});
if (result.error || result.status !== 0) { console.error(result.error || 'Bundle failed');process.exit(1); }
const hash=createHash('sha256').update(readFileSync(here+'assets/app.js')).digest('hex');
writeFileSync(here+'assets/app.js.sha256',hash+'  app.js\n');
console.log('Offline browser bundle SHA-256:',hash);
