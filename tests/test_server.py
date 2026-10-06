import hashlib
import os
import signal
import socket
import subprocess
import tempfile
import time
import unittest

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "http-server")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_server(root, *args):
    port = free_port()
    proc = subprocess.Popen([BIN, "-p", str(port), "-w", "2", *args, root],
                            stdout=subprocess.PIPE, text=True)
    proc.stdout.readline()  # "listening on ..."
    return proc, port


def stop_server(proc):
    proc.send_signal(signal.SIGTERM)
    out, _ = proc.communicate(timeout=5)
    return proc.returncode, out


class Client:
    def __init__(self, port, rcvbuf=None):
        self.sock = socket.socket()
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(5)
        self.sock.connect(("127.0.0.1", port))
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data

    def response(self, head=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        raw, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = raw.decode().split("\r\n")
        status = int(lines[0].split()[1])
        headers = {k.lower(): v for k, v in (l.split(": ", 1) for l in lines[1:])}
        size = 0 if head else int(headers["content-length"])
        while len(self.buf) < size:
            self._fill()
        body, self.buf = self.buf[:size], self.buf[size:]
        return status, headers, body

    def get(self, target="/", head=False):
        self.send(f"{'HEAD' if head else 'GET'} {target} HTTP/1.1\r\nHost: t\r\n\r\n".encode())
        return self.response(head)

    def closed(self):
        try:
            return self.sock.recv(1) == b""
        except ConnectionResetError:  # server closed with unread input
            return True
        except socket.timeout:
            return False


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        base = cls.tmp.name
        cls.root = os.path.join(base, "www")
        os.makedirs(os.path.join(cls.root, "docs"))
        cls.big = os.urandom(4 * 1024 * 1024)
        files = {
            "index.html": b"<h1>home</h1>",
            "docs/index.html": b"<h1>docs</h1>",
            "hello world.txt": b"spaces",
            "big.bin": cls.big,
        }
        for name, data in files.items():
            with open(os.path.join(cls.root, name), "wb") as f:
                f.write(data)
        with open(os.path.join(base, "secret.txt"), "w") as f:
            f.write("secret")
        os.symlink(os.path.join(base, "secret.txt"), os.path.join(cls.root, "link.txt"))
        os.symlink(base, os.path.join(cls.root, "up"))
        cls.proc, cls.port = start_server(cls.root)

    @classmethod
    def tearDownClass(cls):
        code, out = stop_server(cls.proc)
        cls.tmp.cleanup()
        assert code == 0, code
        assert "requests=" in out, out

    def client(self, **kw):
        c = Client(self.port, **kw)
        self.addCleanup(c.sock.close)
        return c

    def test_get(self):
        status, headers, body = self.client().get("/")
        self.assertEqual((status, body), (200, b"<h1>home</h1>"))
        self.assertEqual(headers["content-type"], "text/html; charset=utf-8")
        self.assertEqual(self.client().get("/docs/")[2], b"<h1>docs</h1>")
        self.assertEqual(self.client().get("/hello%20world.txt")[2], b"spaces")
        self.assertEqual(self.client().get("/?x=1")[0], 200)

    def test_head(self):
        c = self.client()
        status, headers, body = c.get("/", head=True)
        self.assertEqual((status, headers["content-length"], body), (200, "13", b""))
        self.assertEqual(c.get("/")[0], 200)  # connection still framed correctly

    def test_not_found_keeps_connection(self):
        c = self.client()
        self.assertEqual(c.get("/missing")[0], 404)
        self.assertEqual(c.get("/docs")[0], 404)  # directory without trailing slash
        self.assertEqual(c.get("/")[0], 200)

    def test_head_404_has_no_body(self):
        c = self.client()
        self.assertEqual(c.get("/missing", head=True)[0], 404)
        self.assertEqual(c.get("/")[0], 200)

    def test_method_not_allowed(self):
        c = self.client()
        c.send(b"POST / HTTP/1.1\r\nHost: t\r\n\r\n")
        status, headers, _ = c.response()
        self.assertEqual((status, headers["allow"]), (405, "GET, HEAD"))
        self.assertTrue(c.closed())

    def test_bad_requests_close(self):
        for raw, want in [(b"garbage\r\n\r\n", 400),
                          (b"GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
                          (b"GET / HTTP/3.0\r\n\r\n", 505),
                          (b"GET / HTTP/1.1\r\n" + b"X: " + b"a" * 9000 + b"\r\n", 431)]:
            c = self.client()
            c.send(raw)
            self.assertEqual(c.response()[0], want, raw[:30])
            self.assertTrue(c.closed())

    def test_bad_escape_is_400_but_framing_is_intact(self):
        c = self.client()
        self.assertEqual(c.get("/%zz")[0], 400)
        self.assertEqual(c.get("/")[0], 200)

    def test_keep_alive_reuse(self):
        c = self.client()
        for _ in range(5):
            self.assertEqual(c.get("/")[2], b"<h1>home</h1>")

    def test_http10_closes_unless_asked(self):
        c = self.client()
        c.send(b"GET / HTTP/1.0\r\n\r\n")
        self.assertEqual(c.response()[0], 200)
        self.assertTrue(c.closed())
        c = self.client()
        c.send(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        self.assertEqual(c.response()[0], 200)
        self.assertEqual(c.get("/")[0], 200)

    def test_pipelining(self):
        c = self.client()
        c.send(b"GET / HTTP/1.1\r\n\r\nGET /docs/ HTTP/1.1\r\n\r\nGET /nope HTTP/1.1\r\n\r\n"
               b"HEAD / HTTP/1.1\r\n\r\n")
        self.assertEqual(c.response()[2], b"<h1>home</h1>")
        self.assertEqual(c.response()[2], b"<h1>docs</h1>")
        self.assertEqual(c.response()[0], 404)
        self.assertEqual(c.response(head=True)[0], 200)

    def test_pipelined_large_responses(self):
        c = self.client(rcvbuf=4096)
        c.send(b"GET /big.bin HTTP/1.1\r\n\r\n" * 2 + b"GET / HTTP/1.1\r\n\r\n")
        time.sleep(0.3)
        self.assertEqual(c.response()[2], self.big)
        self.assertEqual(c.response()[2], self.big)
        self.assertEqual(c.response()[2], b"<h1>home</h1>")

    def test_fragmented_request(self):
        c = self.client()
        for byte in b"GET / HTTP/1.1\r\nHost: t\r\n\r\n":
            c.send(bytes([byte]))
            time.sleep(0.002)
        self.assertEqual(c.response()[0], 200)

    def test_large_file_with_slow_reader(self):
        c = self.client(rcvbuf=4096)
        c.send(b"GET /big.bin HTTP/1.1\r\n\r\n")
        time.sleep(0.5)  # let the server hit EAGAIN on sendfile
        status, headers, body = c.response()
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).digest(), hashlib.sha256(self.big).digest())

    def test_cannot_escape_root(self):
        for target in ["/../secret.txt", "/%2e%2e/secret.txt", "/docs/../../secret.txt",
                       "//secret.txt", "/link.txt", "/up/secret.txt", "/%2fetc/passwd"]:
            status, _, body = self.client().get(target)
            self.assertEqual(status, 404, target)
            self.assertNotIn(b"secret", body)

    def test_many_connections(self):
        clients = [self.client() for _ in range(300)]
        for c in clients:
            c.send(b"GET / HTTP/1.1\r\n\r\n")
        for c in clients:
            self.assertEqual(c.response()[0], 200)


class TimeoutTest(unittest.TestCase):
    def test_idle_and_stalled_connections_are_closed(self):
        with tempfile.TemporaryDirectory() as root:
            proc, port = start_server(root, "-t", "1")
            idle = Client(port)
            stalled = Client(port)
            self.addCleanup(idle.sock.close)
            self.addCleanup(stalled.sock.close)
            stalled.send(b"GET / HTTP/1.1\r\nHost:")  # never finishes
            start = time.time()
            self.assertTrue(idle.closed())
            self.assertTrue(stalled.closed())
            self.assertLess(time.time() - start, 4)
            code, out = stop_server(proc)
            self.assertEqual(code, 0)
            self.assertIn("timeouts=2", out)


if __name__ == "__main__":
    unittest.main()
