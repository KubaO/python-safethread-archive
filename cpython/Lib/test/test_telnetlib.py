import socket
import threading
import telnetlib
import time

from unittest import TestCase
from test import test_support

HOST = test_support.HOST

def server(evt, serv):
    serv.listen(5)
    evt.set()
    try:
        conn, addr = serv.accept()
    except socket.timeout:
        pass
    finally:
        serv.close()
        evt.set()

class GeneralTests(TestCase):

    def setUp(self):
        self.evt = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.port = test_support.bind_port(self.sock)
        threading.Thread(target=server, args=(self.evt,self.sock)).start()
        self.evt.wait()
        self.evt.clear()
        time.sleep(.1)

    def tearDown(self):
        self.evt.wait()

    def testBasic(self):
        # connects
        telnet = telnetlib.Telnet(HOST, self.port)
        telnet.sock.close()

    def testTimeoutDefault(self):
        # default
        telnet = telnetlib.Telnet(HOST, self.port)
        self.assertTrue(telnet.sock.gettimeout() is None)
        telnet.sock.close()

    def testTimeoutValue(self):
        # a value
        telnet = telnetlib.Telnet(HOST, self.port, timeout=30)
        self.assertEqual(telnet.sock.gettimeout(), 30)
        telnet.sock.close()

    def testTimeoutDifferentOrder(self):
        telnet = telnetlib.Telnet(timeout=30)
        telnet.open(HOST, self.port)
        self.assertEqual(telnet.sock.gettimeout(), 30)
        telnet.sock.close()

    def testTimeoutNone(self):
        # None, having other default
        previous = socket.getdefaulttimeout()
        socket.setdefaulttimeout(30)
        try:
            telnet = telnetlib.Telnet(HOST, self.port, timeout=None)
        finally:
            socket.setdefaulttimeout(previous)
        self.assertEqual(telnet.sock.gettimeout(), 30)
        telnet.sock.close()



def test_main(verbose=None):
    test_support.run_unittest(GeneralTests)

if __name__ == '__main__':
    test_main()
