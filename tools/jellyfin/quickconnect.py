#!/usr/bin/env python3
"""Authorize Stereonaut against a Jellyfin server with Quick Connect.

Prints a code that has to be confirmed in the Jellyfin web UI (user menu ->
Quick Connect), then stores the access token in
$XDG_CONFIG_HOME/stereonaut/jellyfin.json with mode 0600. The token is never
printed and never belongs into the repository.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

CONFIG_DIR = os.path.join(
    os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
    "stereonaut")
CONFIG_PATH = os.path.join(CONFIG_DIR, "jellyfin.json")
TIMEOUT_SECONDS = 300


def request(server, method, path, device_id, body=None, token=None):
    header = ('MediaBrowser Client="Stereonaut", Device="Stereonaut", '
              f'DeviceId="{device_id}", Version="0.1"')
    if token:
        header += f', Token="{token}"'
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(server + path, data=data, method=method)
    req.add_header("Authorization", header)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=15) as response:
        payload = response.read()
    return json.loads(payload) if payload else {}


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} https://server", file=sys.stderr)
        return 2
    server = sys.argv[1].rstrip("/")
    device_id = "stereonaut-" + uuid.uuid4().hex[:16]
    state = request(server, "POST", "/QuickConnect/Initiate", device_id)
    print(f"Quick-Connect-Code: {state['Code']}", flush=True)
    print("Im Jellyfin-Web bestaetigen (Benutzermenue -> Quick Connect).",
          flush=True)
    deadline = time.time() + TIMEOUT_SECONDS
    secret = state["Secret"]
    while not state.get("Authenticated"):
        if time.time() > deadline:
            print("Zeitueberschreitung", file=sys.stderr)
            return 1
        time.sleep(2)
        state = request(server, "GET",
                        "/QuickConnect/Connect?secret=" + secret, device_id)
    result = request(server, "POST", "/Users/AuthenticateWithQuickConnect",
                     device_id, {"Secret": secret})
    os.makedirs(CONFIG_DIR, mode=0o700, exist_ok=True)
    descriptor = os.open(CONFIG_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                         0o600)
    with os.fdopen(descriptor, "w") as handle:
        json.dump({"server": server, "device_id": device_id,
                   "user_id": result["User"]["Id"],
                   "token": result["AccessToken"]}, handle)
    print("Angemeldet als", result["User"].get("Name", "?"),
          "- Token gespeichert in", CONFIG_PATH)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except urllib.error.HTTPError as error:
        print(f"HTTP {error.code} {error.reason} bei {error.url}",
              file=sys.stderr)
        sys.exit(1)
