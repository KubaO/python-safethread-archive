# Thread-spawning and joining operation

## Introduction

python-safethread uses `with branch() as children:` to spawn threads.  This incorporates exception handling, cancellation, and deadlock detection.

```python
from __future__ import shared_module
from socket import socket
from threadtools import branch

def main():
    server_socket = socket(...)
    server_socket.bind(...)
    run_server(server_socket)

def run_server(server_socket):
    with server_socket:  # Close the server socket when we exit
        with branch() as clients:
            clients.add(handle_client, *server_socket.accept())

def handle_client(client_socket, addr):
    with client_socket:  # Close the client socket when we exit
        ... utilize client_socket ...
        data = client_socket.read()
```

You'll end up with a stack something like this:

```
main - run_server - socket.accept        # Main thread
         |- handle_client - socket.read  # First client thread
         \- handle_client - socket.read  # Second client thread
```

If any one of the threads fails, the rest will be cancelled, and the main thread will wait in the `with branch() as clients:` line until the children shave exited.

Generally, only IO operations (accessing a file or socket) or a [condition](Monitors.wiki.md) will react to being cancelled, raising a Cancelled exception (caught by the `with branch() as clients:`.)  CPU-bound operations will ignore it, running to completion (and thus allowing them to leave things in a sane state.)