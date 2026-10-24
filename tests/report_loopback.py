"""报告测试的本机 HTTP 夹具与独立 ZIP 验证；仅使用 Python 标准库。"""

import argparse
import email.parser
import email.policy
import http.server
import json
from pathlib import Path
import socket
import threading
import zipfile


def verify_archives(root):
    count = 0
    for manifest_path in Path(root).rglob("archive-manifest.json"):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        with zipfile.ZipFile(manifest["archive"]) as archive:
            assert archive.testzip() is None, "ZIP CRC mismatch"
            assert set(archive.namelist()) == set(manifest["included"]), "ZIP attachment selection mismatch"
            assert not set(manifest["excluded"]) & set(archive.namelist()), "Excluded attachment exported"
            document = json.loads(archive.read("report.json"))
            assert document["diagnostics"]["extension_version"] == 2
            assert document["diagnostics"]["permissions"]["network_upload"] == "not_authorized"
            assert document["left_out"] == manifest["excluded"]
            if manifest["future_extension"]:
                extension = document["future_extension"]
                assert extension["access_token"] == "<redacted>"
                assert extension["nested"] == [True, 42, "中文"]
            for name in archive.namelist():
                if name.endswith((".txt", ".ini", ".log", ".json")):
                    text = archive.read(name).decode("utf-8")
                    for secret in ("SYNTHETIC123", "query-value", "opaque-secret", "private-header"):
                        assert secret not in text, f"Synthetic secret in {name}"
        count += 1
    assert count > 0, "No flow archives found"
    print(f"Independent ZIP/JSON/privacy verification passed: {count} archives")


def serve(ready_file, log_file, expected_file):
    log_lock = threading.Lock()
    expected_payload = Path(expected_file).read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.connection.settimeout(5)
            length = int(self.headers.get("Content-Length", "0"))
            valid = False
            error = ""
            try:
                assert 0 < length <= 32 * 1024 * 1024, "Invalid multipart length"
                body = self.rfile.read(length)
                assert len(body) == length, "Incomplete request (cancellation)"
                mime = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                    ("Content-Type: " + self.headers.get("Content-Type", "") + "\r\nMIME-Version: 1.0\r\n\r\n").encode("ascii") + body
                )
                assert mime.is_multipart(), "Not multipart/form-data"
                parts = {part.get_param("name", header="content-disposition"): part for part in mime.iter_parts()}
                assert parts["reqtype"].get_payload(decode=True) == b"fileupload"
                assert parts["time"].get_payload(decode=True) == b"72h"
                file = parts["fileToUpload"]
                assert file.get_filename() == "renodx-dlss5-report.zip"
                assert file.get_content_type() == "application/zip"
                payload = file.get_payload(decode=True)
                assert payload == expected_payload, "Multipart file bytes differ from the saved ZIP"
                valid = True
            except (AssertionError, KeyError, OSError, ValueError) as exc:
                error = str(exc)
            with log_lock:
                with Path(log_file).open("a", encoding="utf-8") as log:
                    log.write(json.dumps({"path": self.path, "multipart_valid": valid, "error": error}) + "\n")
            if self.path == "/reset":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            status = 200
            response = b"https://example.invalid/synthetic-report.zip\n"
            if not valid:
                status, response = 400, b"invalid multipart"
            elif self.path in ("/fail", "/failure"):
                status, response = 503, b"synthetic failure"
            elif self.path == "/proxy":
                status, response = 407, b"synthetic proxy authentication required"
            elif self.path == "/invalid":
                response = b"not a report URL"
            elif self.path == "/redirect":
                status = 307
                response = b"redirect must not be followed"
            elif self.path == "/oversize-response":
                response = b"https://example.invalid/report.zip\n" + b"x" * (17 * 1024)
            try:
                self.send_response(status)
                if self.path == "/redirect":
                    self.send_header("Location", f"http://127.0.0.1:{self.server.server_port}/redirect-target")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    Path(ready_file).write_text(json.dumps({"base_url": f"http://127.0.0.1:{server.server_port}"}), encoding="utf-8")
    server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    server_parser = commands.add_parser("serve")
    server_parser.add_argument("--ready-file", required=True)
    server_parser.add_argument("--log-file", required=True)
    server_parser.add_argument("--expected-file", required=True)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("root")
    args = parser.parse_args()
    if args.command == "serve":
        serve(args.ready_file, args.log_file, args.expected_file)
    else:
        verify_archives(args.root)
