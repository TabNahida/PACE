#!/usr/bin/env python3
"""Run external interop peers against PACE.

    python3 tests/interop/run_interop.py --build-dir build/ci                # every suite whose peer exists
    python3 tests/interop/run_interop.py --build-dir build/ci --require h2spec,curl-h2
    python3 tests/interop/run_interop.py --build-dir build/ci --list

The runner starts pace_interop_server (built with the tests), reads its endpoint map and runs each
suite. A suite is skipped, not failed, when its peer tool or its PACE endpoint is missing, unless it
is listed in --require. Pinned peer versions and where CI gets them are in tests/interop/peers.json.
Results go to stdout and, with --output, to a JSON report.

Later phases add suites here as PACE gains its own TLS, QUIC and HTTP engines (BoGo, quic-interop-runner,
tlsfuzzer, h3spec against the native HTTP/3 server...).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CERTS = ROOT / "tests" / "certs"
EXE = ".exe" if os.name == "nt" else ""


class Skip(Exception):
    pass


def tool(name, environment=None):
    path = os.environ.get(environment) if environment else None
    path = path or shutil.which(name)
    if not path:
        raise Skip(f"{name} is not installed")
    return path


def run(command, timeout=60, input_bytes=None):
    result = subprocess.run(command, capture_output=True, timeout=timeout, input=input_bytes)
    return result.returncode, result.stdout.decode(errors="replace"), result.stderr.decode(errors="replace")


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def curl_supports(curl, feature):
    _, out, _ = run([curl, "--version"])
    return feature.lower() in out.lower()


# -- suites -------------------------------------------------------------------------------------------------------


def curl_request(ports, endpoint, url_scheme, extra):
    curl = tool("curl")
    if endpoint not in ports:
        raise Skip(f"pace_interop_server has no {endpoint} endpoint in this build")
    base = f"{url_scheme}://localhost:{ports[endpoint]}"
    common = [curl, "--silent", "--show-error", "--max-time", "20", "--cacert", str(CERTS / "ca.pem"), *extra]
    code, out, err = run([*common, base + "/"])
    expect(code == 0 and out == "hello from PACE\n", f"GET / failed (exit {code}): {err.strip() or out!r}")
    payload = "interop " * 4096
    code, out, err = run([*common, "--data-binary", "@-", base + "/echo"], input_bytes=payload.encode())
    expect(code == 0 and out == payload, f"POST /echo failed (exit {code}): {err.strip() or len(out)}")
    code, out, err = run([*common, "--output", os.devnull, "--write-out", "%{http_code} %{http_version}",
                          base + "/status/204"])
    expect(code == 0 and out.startswith("204"), f"GET /status/204 failed (exit {code}): {err.strip() or out!r}")
    return {"http_version": out.split()[-1]}


def suite_curl_http1(ports):
    return curl_request(ports, "http1", "http", ["--http1.1"])


def suite_curl_https1(ports):
    return curl_request(ports, "https1", "https", ["--http1.1"])


def suite_curl_h2c(ports):
    if not curl_supports(tool("curl"), "HTTP2"):
        raise Skip("curl was built without HTTP/2")
    return curl_request(ports, "h2c", "http", ["--http2-prior-knowledge"])


def suite_curl_h2(ports):
    if not curl_supports(tool("curl"), "HTTP2"):
        raise Skip("curl was built without HTTP/2")
    result = curl_request(ports, "h2", "https", ["--http2"])
    expect(result["http_version"] == "2", f"ALPN did not select h2: {result}")
    return result


def suite_curl_h3(ports):
    if not curl_supports(tool("curl"), "HTTP3"):
        raise Skip("curl was built without HTTP/3")
    return curl_request(ports, "h3", "https", ["--http3-only"])


def suite_openssl_tls(ports):
    openssl = tool("openssl", "PACE_INTEROP_OPENSSL")
    if "tls_echo" not in ports:
        raise Skip("pace_interop_server has no TLS endpoint in this build")
    results = {}
    for version in ("-tls1_2", "-tls1_3"):
        message = f"ping over {version}\n"
        client = subprocess.Popen([openssl, "s_client", "-connect", f"127.0.0.1:{ports['tls_echo']}", version,
                                   "-servername", "localhost", "-CAfile", str(CERTS / "ca.pem"),
                                   "-verify_return_error", "-quiet"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # s_client closes as soon as stdin ends, so keep stdin open until the echo arrives.
        reader = threading.Thread(target=lambda: setattr(client, "echo", client.stdout.readline()), daemon=True)
        reader.start()
        client.stdin.write(message)
        client.stdin.flush()
        reader.join(timeout=20)
        echo = getattr(client, "echo", "")
        client.stdin.close()
        try:
            client.wait(timeout=10)
        except subprocess.TimeoutExpired:
            client.kill()
        error = client.stderr.read()
        expect(echo == message, f"{version}: expected echo {message!r}, got {echo!r}: {error.strip()[-400:]}")
        results[version.lstrip("-")] = "ok"
    return results


def suite_h2spec(ports):
    h2spec = tool("h2spec", "PACE_INTEROP_H2SPEC")
    if "h2" not in ports:
        raise Skip("pace_interop_server has no TLS HTTP/2 endpoint in this build")
    report = Path(os.environ.get("PACE_INTEROP_OUTPUT_DIR", ".")) / "h2spec-junit.xml"
    code, out, err = run([h2spec, "--tls", "--insecure", "--host", "127.0.0.1", "--port", str(ports["h2"]),
                          "--timeout", "5", "--junit-report", str(report)], timeout=600)
    summary = [line for line in out.splitlines() if "tests," in line]
    # h2spec exits non-zero when any test fails. The 0.6.1 HTTP/2 server is built on nghttp2; a known
    # failure list is kept beside this script so regressions stand out while known gaps stay visible.
    known = ROOT / "tests" / "interop" / "h2spec-known-failures.txt"
    failures = [line.strip() for line in out.splitlines() if line.strip().startswith("× ")]
    expected = set(known.read_text().splitlines()) if known.is_file() else set()
    unexpected = [line for line in failures if line[2:].strip() not in expected]
    expect(not unexpected, "unexpected h2spec failures:\n" + "\n".join(unexpected[:40]))
    return {"exit": code, "summary": summary[-1] if summary else out.strip()[-200:], "known_failures": len(failures)}


SUITES = {
    "curl-http1": suite_curl_http1,
    "curl-https1": suite_curl_https1,
    "curl-h2c": suite_curl_h2c,
    "curl-h2": suite_curl_h2,
    "curl-h3": suite_curl_h3,
    "openssl-tls": suite_openssl_tls,
    "h2spec": suite_h2spec,
}


# -- driver -------------------------------------------------------------------------------------------------------


def find_server(build_dir):
    matches = sorted(build_dir.glob(f"**/pace_interop_server{EXE}"), key=lambda path: path.stat().st_mtime)
    if not matches:
        sys.exit(f"pace_interop_server was not found under {build_dir}; build it with --tests=y")
    return matches[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--suites", default=",".join(SUITES))
    parser.add_argument("--require", default="", help="comma-separated suites that must run, not be skipped")
    parser.add_argument("--informational", default="",
                        help="comma-separated suites whose failures are reported but do not fail the run")
    parser.add_argument("--output", type=Path, help="write a JSON report here")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.list:
        print("\n".join(SUITES))
        return 0
    selected = [name for name in args.suites.split(",") if name]
    required = {name for name in args.require.split(",") if name}
    informational = {name for name in args.informational.split(",") if name}
    unknown = (set(selected) | required | informational) - set(SUITES)
    if unknown:
        parser.error(f"unknown suites: {', '.join(sorted(unknown))}")

    server = subprocess.Popen([str(find_server(args.build_dir)), str(CERTS)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    report = {"server": None, "suites": {}}
    failed = False
    try:
        line = server.stdout.readline()
        if not line:
            sys.exit(f"pace_interop_server exited early: {server.stderr.read()}")
        ports = json.loads(line)
        report["server"] = ports
        print(f"PACE endpoints: {ports}", flush=True)
        for name in selected:
            started = time.monotonic()
            try:
                details = SUITES[name](ports)
                status = {"status": "passed", "details": details}
            except Skip as reason:
                status = {"status": "skipped", "reason": str(reason)}
                if name in required:
                    status["status"] = "failed"
                    failed = True
            except (AssertionError, subprocess.TimeoutExpired, OSError) as error:
                status = {"status": "failed", "reason": str(error)}
                if name in informational:
                    status["status"] = "failed (informational)"
                else:
                    failed = True
            status["seconds"] = round(time.monotonic() - started, 2)
            report["suites"][name] = status
            print(f"{status['status']:>7}  {name}  {status.get('reason', status.get('details', ''))}", flush=True)
    finally:
        server.stdin.close()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
