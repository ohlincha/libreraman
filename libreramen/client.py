"""Client for the local HTTP/JSON API of the running LibreRamen application (standard library only).

The application writes ``api.json`` ({"url", "port", "token", ...}, readable by the user only) when its API
starts; this client reads it (or $LIBRERAMEN_API_FILE, or $LIBRERAMEN_API_URL + $LIBRERAMEN_API_TOKEN):

    from libreramen.client import Client
    c = Client()                       # finds the running application
    print(c.status())
    job = c.measure(exposure_s=1, averages=3, sample="CCl4")    # needs "Allow remote measurements" in the GUI
    sp = c.spectrum(job["spectra"][-1])                         # {"shift": [...], "intensity": [...], ...}
    print(c.peaks(sp["id"]))
    c.stop()                           # always allowed: stop and laser off

Errors are raised as ApiError (with .status: 401 token, 403 remote measurements not allowed, 409 busy, ...).
"""
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

__all__ = ["ApiError", "Client", "default_api_file"]


class ApiError(RuntimeError):
    def __init__(self, message: str, status: int = 0, body=None):
        super().__init__(message)
        self.status = status
        self.body = body


def default_api_file() -> Path:
    """Where the application writes its API file (QStandardPaths::GenericConfigLocation/LibreRamen/api.json)."""
    env = os.environ.get("LIBRERAMEN_API_FILE")
    if env:
        return Path(env)
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Preferences"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "LibreRamen" / "api.json"


class Client:
    def __init__(self, url: str | None = None, token: str | None = None, api_file: str | os.PathLike | None = None,
                 client_name: str = "python", timeout: float = 30.0):
        if url is None and token is None and api_file is None:
            url, token = os.environ.get("LIBRERAMEN_API_URL"), os.environ.get("LIBRERAMEN_API_TOKEN")
            if not (url and token):
                url = token = None
        if url is None or token is None:
            path = Path(api_file) if api_file else default_api_file()
            try:
                info = json.loads(path.read_text(encoding="utf-8"))
            except OSError:
                raise ApiError(f"LibreRamen is not running, or its API is switched off (no API file {path}). "
                               "Start the LibreRamen application.") from None
            url = url or info["url"]
            token = token or info["token"]
        self.url = url.rstrip("/")
        self.token = token
        self.client_name = client_name
        self.timeout = timeout

    # ---- low level
    def request(self, method: str, path: str, body: dict | None = None, timeout: float | None = None):
        data = json.dumps(body).encode() if body is not None else (b"{}" if method == "POST" else None)
        req = urllib.request.Request(self.url + path, data=data, method=method, headers={
            "Authorization": f"Bearer {self.token}", "X-LibreRamen-Client": self.client_name,
            "Content-Type": "application/json", "Accept": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                return json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            try:
                payload = json.loads(e.read() or b"null")
            except ValueError:
                payload = None
            msg = payload.get("error") if isinstance(payload, dict) else None
            raise ApiError(msg or f"HTTP {e.code}", e.code, payload) from None
        except urllib.error.URLError as e:
            raise ApiError(f"cannot connect to LibreRamen at {self.url}: {e.reason}") from None

    # ---- endpoints
    def status(self):
        return self.request("GET", "/status")

    def info(self):
        return self.request("GET", "/info")

    def spectra(self, limit: int | None = None):
        return self.request("GET", "/spectra" + (f"?limit={int(limit)}" if limit is not None else ""))["spectra"]

    def spectrum(self, id: int):
        return self.request("GET", f"/spectra/{int(id)}")

    def measure(self, wait: bool = True, timeout: float | None = None, **settings):
        """One spectrum: exposure_s, averages, background ('each'|'none'), prebleach_s, sample, channel.
        Returns the job ({"state": "done"|"failed"|"stopped"|"running", "spectra": [ids], ...})."""
        if timeout is None:
            t = settings.get("exposure_s", 1) * settings.get("averages", 1) * 2 + settings.get("prebleach_s", 0) + 30
            timeout = 2 * t + 120
        return self.request("POST", "/measure", dict(settings, wait=wait), timeout=timeout if wait else None)

    def series(self, **settings):
        """Start a time series (interval_s, count or duration_s, ...); returns the job at once."""
        return self.request("POST", "/series", settings)

    def series_progress(self):
        return self.request("GET", "/series/progress")

    def job(self, id: int):
        return self.request("GET", f"/jobs/{int(id)}")

    def stop(self):
        return self.request("POST", "/stop", {})

    def process(self, id: int | None = None, x=None, y=None, settings: dict | None = None):
        return self.request("POST", "/process", _data(id, x, y, settings))

    def peaks(self, id: int | None = None, x=None, y=None, settings: dict | None = None, **options):
        """options: snr, min_fwhm, shoulders, xmin, xmax."""
        return self.request("POST", "/peaks", dict(_data(id, x, y, settings), **options))["peaks"]

    def standards(self):
        return self.request("GET", "/standards")["standards"]

    def log(self, since: int = 0):
        return self.request("GET", f"/log?since={int(since)}")


def _data(id, x, y, settings):
    body = {"id": int(id)} if id is not None else {"x": [float(v) for v in x], "y": [float(v) for v in y]}
    if settings is not None:
        body["settings"] = settings
    return body


if __name__ == "__main__":      # python -m libreramen.client  -> status
    print(json.dumps(Client().status(), indent=2))
