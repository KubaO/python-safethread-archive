"""Utilities to get a password and/or the current user name.

getpass(prompt) - prompt for a password, with echo turned off
getuser() - get the user name from the environment or password database

On Windows, the msvcrt module will be used.
On the Mac EasyDialogs.AskPassword is used, if available.

"""

# Authors: Piers Lauder (original)
#          Guido van Rossum (Windows support and cleanup)

import sys

__all__ = ["getpass","getuser"]

def unix_getpass(prompt='Password: ', stream=None):
    """Prompt for a password, with echo turned off.
    The prompt is written on stream, by default stdout.

    Restore terminal settings at end.
    """
    if stream is None:
        stream = sys.stdout

    try:
        fd = sys.stdin.fileno()
    except:
        return default_getpass(prompt)

    old = termios.tcgetattr(fd)     # a copy to save
    new = old[:]

    new[3] = new[3] & ~termios.ECHO # 3 == 'lflags'
    try:
        termios.tcsetattr(fd, termios.TCSADRAIN, new)
        passwd = _raw_input(prompt, stream)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

    stream.write('\n')
    return passwd


def win_getpass(prompt='Password: ', stream=None):
    """Prompt for password with echo off, using Windows getch()."""
    if sys.stdin is not sys.__stdin__:
        return default_getpass(prompt, stream)
    import msvcrt
    for c in prompt:
        msvcrt.putwch(c)
    pw = ""
    while 1:
        c = msvcrt.getwch()
        if c == '\r' or c == '\n':
            break
        if c == '\003':
            raise KeyboardInterrupt
        if c == '\b':
            pw = pw[:-1]
        else:
            pw = pw + c
    msvcrt.putwch('\r')
    msvcrt.putwch('\n')
    return pw


def default_getpass(prompt='Password: ', stream=None):
    print("Warning: Problem with getpass. Passwords may be echoed.", file=sys.stderr)
    return _raw_input(prompt, stream)


def _raw_input(prompt="", stream=None):
    # This doesn't save the string in the GNU readline history.
    if stream is None:
        stream = sys.stdout
    prompt = str(prompt)
    if prompt:
        stream.write(prompt)
        stream.flush()
    line = sys.stdin.readline()
    if not line:
        raise EOFError
    if line[-1] == '\n':
        line = line[:-1]
    return line


def getuser():
    """Get the username from the environment or password database.

    First try various environment variables, then the password
    database.  This works on Windows as long as USERNAME is set.

    """

    import os

    for name in ('LOGNAME', 'USER', 'LNAME', 'USERNAME'):
        user = os.environ.get(name)
        if user:
            return user

    # If this fails, the exception will "explain" why
    import pwd
    return pwd.getpwuid(os.getuid())[0]

# Bind the name getpass to the appropriate function
try:
    import termios
    # it's possible there is an incompatible termios from the
    # McMillan Installer, make sure we have a UNIX-compatible termios
    termios.tcgetattr, termios.tcsetattr
except (ImportError, AttributeError):
    try:
        import msvcrt
    except ImportError:
        try:
            from EasyDialogs import AskPassword
        except ImportError:
            getpass = default_getpass
        else:
            getpass = AskPassword
    else:
        getpass = win_getpass
else:
    getpass = unix_getpass
