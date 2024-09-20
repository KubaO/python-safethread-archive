#  Which objects can be shared between threads

You probably know how to append to a list or modify a dict.  When confronted with threading though you may start to wonder, what happens if two threads modify a list or a dict simultaneously?  There's two common answers to this:

1. Corruption.  This is the default in C, and to a lesser degree Java.  It doesn't limit you much and can be faster, but it requires some deep voodoo to know when you're using it right.

2. Locks.  All the operations internally lock the object.  This makes them individually correct, but they'll be wrong again if you try to compose them into larger operations.  It also adds a significant performance penalty.

   My priority is making it easy to write correct programs, and neither of those options are good enough.  Instead, I take a third option as my default:

3. Make sharing impossible, avoiding the question.  list and dict cannot be shared between threads.  Any attempt to pass a list or dict into something like a Queue will raise a !TypeError.  This ensures any thread interaction is explicit, rather than implicit.  See also: [The Zen of Python](http://www.python.org/dev/peps/pep-0020/)

Of course if you couldn't share _any_ objects then you'd just have processes, which are quite awkward to use.  Instead, I only make mutable objects like list and dict unshareable, while immutable int and str objects can still be shared.  Further, mutable objects that provide an explicit API for use between threads are also shareable.

A [Monitor](Monitors.wiki.md) is a major example of the latter.  It lets you store unshareable objects safely inside, while the Monitor itself is shareable and passed between threads.

Another example is a [`shareddict`](SharedDict.wiki.md).  It's a dict that can only have shareable keys and values, and all the operations are atomic.  It's used for module and class dictionaries, which typically aren't modified after startup.