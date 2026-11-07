package platform

import (
	"bytes"
	"mime"
	"net/http"
	"path"
	"strings"
	"time"
)

func (a *App) console(w http.ResponseWriter, r *http.Request) {
	http.Redirect(w, r, "/ui/", http.StatusTemporaryRedirect)
}

// The compiled UI is embedded in the server, so customer installations need no
// Node runtime, external CDN or separate reverse proxy for the console.
func (a *App) frontend(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; img-src 'self' data:; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
	name := strings.TrimPrefix(r.URL.Path, "/ui/")
	for _, segment := range strings.Split(name, "/") {
		if segment == ".." || strings.Contains(segment, "\\") {
			http.NotFound(w, r)
			return
		}
	}
	if name == "" {
		name = "index.html"
	}
	b, err := Assets.ReadFile("assets/ui/" + name)
	if err != nil {
		// Route reloads use the SPA shell, while missing build assets stay 404.
		if path.Ext(name) != "" || strings.HasPrefix(name, "assets/") {
			http.NotFound(w, r)
			return
		}
		name = "index.html"
		b, err = Assets.ReadFile("assets/ui/index.html")
		if err != nil {
			http.Error(w, "Frontend build missing; run pnpm build in frontend before building the server.", http.StatusServiceUnavailable)
			return
		}
	}
	w.Header().Set("Content-Type", mime.TypeByExtension(path.Ext(name)))
	if strings.HasPrefix(name, "assets/") {
		w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	} else {
		w.Header().Set("Cache-Control", "no-store")
	}
	http.ServeContent(w, r, name, time.Time{}, bytes.NewReader(b))
}
