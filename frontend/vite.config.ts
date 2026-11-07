import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'
export default defineConfig({ plugins:[vue()], base:'/ui/', resolve:{alias:{'@':fileURLToPath(new URL('./src',import.meta.url))}}, build:{outDir:'../server/internal/platform/assets/ui',emptyOutDir:true,chunkSizeWarningLimit:1500}, server:{port:5173,proxy:{'/api':'http://127.0.0.1:18780','/openapi.json':'http://127.0.0.1:18780','/health':'http://127.0.0.1:18780'}} })
