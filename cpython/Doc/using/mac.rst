
.. _using-on-mac:

***************************
Using Python on a Macintosh
***************************

:Author: Bob Savage <bobsavage@mac.com>


Python on a Macintosh running Mac OS X is in principle very similar to Python on
any other Unix platform, but there are a number of additional features such as
the IDE and the Package Manager that are worth pointing out.

The Mac-specific modules are documented in :ref:`mac-specific-services`.

Python on Mac OS 9 or earlier can be quite different from Python on Unix or
Windows, but is beyond the scope of this manual, as that platform is no longer
supported, starting with Python 2.4. See http://www.cwi.nl/~jack/macpython for
installers for the latest 2.3 release for Mac OS 9 and related documentation.


.. _getting-osx:

Getting and Installing MacPython
================================

Mac OS X 10.4 comes with Python 2.3 pre-installed by Apple. However, you are
encouraged to install the most recent version of Python from the Python website
(http://www.python.org). A "universal binary" build of Python 2.5, which runs
natively on the Mac's new Intel and legacy PPC CPU's, is available there.

What you get after installing is a number of things:

* A :file:`MacPython 2.5` folder in your :file:`Applications` folder. In here
  you find IDLE, the development environment that is a standard part of official
  Python distributions; PythonLauncher, which handles double-clicking Python
  scripts from the Finder; and the "Build Applet" tool, which allows you to
  package Python scripts as standalone applications on your system.

* A framework :file:`/Library/Frameworks/Python.framework`, which includes the
  Python executable and libraries. The installer adds this location to your shell
  path. To uninstall MacPython, you can simply remove these three things. A
  symlink to the Python executable is placed in /usr/local/bin/.

The Apple-provided build of Python is installed in
:file:`/System/Library/Frameworks/Python.framework` and :file:`/usr/bin/python`,
respectively. You should never modify or delete these, as they are
Apple-controlled and are used by Apple- or third-party software.

IDLE includes a help menu that allows you to access Python documentation. If you
are completely new to Python you should start reading the tutorial introduction
in that document.

If you are familiar with Python on other Unix platforms you should read the
section on running Python scripts from the Unix shell.


How to run a Python script
--------------------------

Your best way to get started with Python on Mac OS X is through the IDLE
integrated development environment, see section :ref:`ide` and use the Help menu
when the IDE is running.

If you want to run Python scripts from the Terminal window command line or from
the Finder you first need an editor to create your script. Mac OS X comes with a
number of standard Unix command line editors, :program:`vim` and
:program:`emacs` among them. If you want a more Mac-like editor,
:program:`BBEdit` or :program:`TextWrangler` from Bare Bones Software (see
http://www.barebones.com/products/bbedit/index.shtml) are good choices, as is
:program:`TextMate` (see http://macromates.com/). Other editors include
:program:`Gvim` (http://macvim.org) and :program:`Aquamacs`
(http://aquamacs.org).

To run your script from the Terminal window you must make sure that
:file:`/usr/local/bin` is in your shell search path.

To run your script from the Finder you have two options:

* Drag it to :program:`PythonLauncher`

* Select :program:`PythonLauncher` as the default application to open your
  script (or any .py script) through the finder Info window and double-click it.
  :program:`PythonLauncher` has various preferences to control how your script is
  launched. Option-dragging allows you to change these for one invocation, or use
  its Preferences menu to change things globally.


.. _osx-gui-scripts:

Running scripts with a GUI
--------------------------

With older versions of Python, there is one Mac OS X quirk that you need to be
aware of: programs that talk to the Aqua window manager (in other words,
anything that has a GUI) need to be run in a special way. Use :program:`pythonw`
instead of :program:`python` to start such scripts.

With Python 2.5, you can use either :program:`python` or :program:`pythonw`.


Configuration
-------------

Python on OS X honors all standard Unix environment variables such as
:envvar:`PYTHONPATH`, but setting these variables for programs started from the
Finder is non-standard as the Finder does not read your :file:`.profile` or
:file:`.cshrc` at startup. You need to create a file :file:`~
/.MacOSX/environment.plist`. See Apple's Technical Document QA1067 for details.

For more information on installation Python packages in MacPython, see section
:ref:`mac-package-manager`.


.. _ide:

The IDE
=======

MacPython ships with the standard IDLE development environment. A good
introduction to using IDLE can be found at http://hkn.eecs.berkeley.edu/
dyoo/python/idle_intro/index.html.


.. _mac-package-manager:

Installing Additional Python Packages
=====================================

There are several methods to install additional Python packages:

* http://pythonmac.org/packages/ contains selected compiled packages for Python
  2.5, 2.4, and 2.3.

* Packages can be installed via the standard Python distutils mode (``python
  setup.py install``).

* Many packages can also be installed via the :program:`setuptools` extension.


GUI Programming on the Mac
==========================

There are several options for building GUI applications on the Mac with Python.

*PyObjC* is a Python binding to Apple's Objective-C/Cocoa framework, which is
the foundation of most modern Mac development. Information on PyObjC is
available from http://pyobjc.sourceforge.net.

The standard Python GUI toolkit is :mod:`Tkinter`, based on the cross-platform
Tk toolkit (http://www.tcl.tk). An Aqua-native version of Tk is bundled with OS
X by Apple, and the latest version can be downloaded and installed from
http://www.activestate.com; it can also be built from source.

*wxPython* is another popular cross-platform GUI toolkit that runs natively on
Mac OS X. Packages and documentation are available from http://www.wxpython.org.

*PyQt* is another popular cross-platform GUI toolkit that runs natively on Mac
OS X. More information can be found at
http://www.riverbankcomputing.co.uk/pyqt/.


Distributing Python Applications on the Mac
===========================================

The "Build Applet" tool that is placed in the MacPython 2.5 folder is fine for
packaging small Python scripts on your own machine to run as a standard Mac
application. This tool, however, is not robust enough to distribute Python
applications to other users.

The standard tool for deploying standalone Python applications on the Mac is
:program:`py2app`. More information on installing and using py2app can be found
at http://undefined.org/python/#py2app.


Application Scripting
=====================

Python can also be used to script other Mac applications via Apple's Open
Scripting Architecture (OSA); see http://appscript.sourceforge.net. Appscript is
a high-level, user-friendly Apple event bridge that allows you to control
scriptable Mac OS X applications using ordinary Python scripts. Appscript makes
Python a serious alternative to Apple's own *AppleScript* language for
automating your Mac. A related package, *PyOSA*, is an OSA language component
for the Python scripting language, allowing Python code to be executed by any
OSA-enabled application (Script Editor, Mail, iTunes, etc.). PyOSA makes Python
a full peer to AppleScript.


Other Resources
===============

The MacPython mailing list is an excellent support resource for Python users and
developers on the Mac:

http://www.python.org/community/sigs/current/pythonmac-sig/

Another useful resource is the MacPython wiki:

http://wiki.python.org/moin/MacPython

