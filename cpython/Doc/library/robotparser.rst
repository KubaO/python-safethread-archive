
:mod:`robotparser` ---  Parser for robots.txt
=============================================

.. module:: robotparser
   :synopsis: Loads a robots.txt file and answers questions about fetchability of other URLs.
.. sectionauthor:: Skip Montanaro <skip@pobox.com>


.. index::
   single: WWW
   single: World Wide Web
   single: URL
   single: robots.txt

This module provides a single class, :class:`RobotFileParser`, which answers
questions about whether or not a particular user agent can fetch a URL on the
Web site that published the :file:`robots.txt` file.  For more details on the
structure of :file:`robots.txt` files, see http://www.robotstxt.org/orig.html.


.. class:: RobotFileParser()

   This class provides a set of methods to read, parse and answer questions about a
   single :file:`robots.txt` file.


   .. method:: RobotFileParser.set_url(url)

      Sets the URL referring to a :file:`robots.txt` file.


   .. method:: RobotFileParser.read()

      Reads the :file:`robots.txt` URL and feeds it to the parser.


   .. method:: RobotFileParser.parse(lines)

      Parses the lines argument.


   .. method:: RobotFileParser.can_fetch(useragent, url)

      Returns ``True`` if the *useragent* is allowed to fetch the *url* according to
      the rules contained in the parsed :file:`robots.txt` file.


   .. method:: RobotFileParser.mtime()

      Returns the time the ``robots.txt`` file was last fetched.  This is useful for
      long-running web spiders that need to check for new ``robots.txt`` files
      periodically.


   .. method:: RobotFileParser.modified()

      Sets the time the ``robots.txt`` file was last fetched to the current time.

The following example demonstrates basic use of the RobotFileParser class. ::

   >>> import robotparser
   >>> rp = robotparser.RobotFileParser()
   >>> rp.set_url("http://www.musi-cal.com/robots.txt")
   >>> rp.read()
   >>> rp.can_fetch("*", "http://www.musi-cal.com/cgi-bin/search?city=San+Francisco")
   False
   >>> rp.can_fetch("*", "http://www.musi-cal.com/")
   True

