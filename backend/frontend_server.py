"""Serve the built React application on port 3000 without a dev server."""

import logging
import os
from ipaddress import ip_address, ip_network
from pathlib import Path

from flask import Flask, abort, request, send_from_directory
from waitress import serve

from .api import DEFAULT_NETWORKS, ROOT


def create_frontend(config=None):
    app = Flask(__name__, static_folder=None)
    app.config.update(DIST_DIR=str(ROOT / "frontend" / "dist"), ALLOWED_NETWORKS=os.getenv("BMS_ALLOWED_NETWORKS", DEFAULT_NETWORKS))
    app.config.update(config or {})
    directory = Path(app.config["DIST_DIR"]).resolve()
    networks = [ip_network(n.strip()) for n in app.config["ALLOWED_NETWORKS"].split(",") if n.strip()]

    @app.before_request
    def local_only():
        try:
            if not any(ip_address(request.remote_addr or "") in n for n in networks):
                abort(403)
        except ValueError:
            abort(403)

    @app.after_request
    def headers(response):
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "same-origin"
        response.headers["Cache-Control"] = "public, max-age=31536000, immutable" if request.path.startswith("/assets/") and response.status_code == 200 else "no-cache"
        return response

    @app.get("/", defaults={"path": ""})
    @app.get("/<path:path>")
    def frontend(path):
        if any(part.startswith(".") for part in Path(path).parts):
            abort(404)
        if path and (directory / path).is_file():
            return send_from_directory(directory, path)
        if path.startswith("assets/") or Path(path).suffix:
            abort(404)
        return send_from_directory(directory, "index.html")

    return app


def main():
    logging.basicConfig(level=logging.INFO)
    if not (ROOT / "frontend" / "dist" / "index.html").exists():
        raise SystemExit("Frontend build is missing. Run npm ci and npm run build in frontend/ first.")
    host = os.getenv("BMS_BIND_HOST", "127.0.0.1")
    print(f"BMS frontend: http://{host}:3000", flush=True)
    serve(create_frontend(), host=host, port=3000, threads=4)


if __name__ == "__main__":
    main()
