# The Deadlock Fallacy

One of the great hurdles with designing a concurrent language is how to deal with deadlocks.  There's a desire to make deadlocks impossible, but I believe this is a mistake, for reasons that will become apparent.

The first of two major approaches is to use message passing (in any of many variations).  You isolate all your state in a series of objects, then split function calls into two categories.  For small operations you use "immediate" calls, which are allowed to modify a single object's state (and lock the object while doing so), but are prohibited from using "eventual" calls.  This second category, "eventual" calls, are allowed to communicate between objects, but are not allowed to lock any objects while doing so.

This split between "immediate" and "eventual" calls makes deadlocks impossible, but it only does so by making large operations impossible.  You can't modify several objects as a single operation, and instead must break them down into smaller operations (risking race conditions), or reinvent locking (and deadlocks).

The second major approach is to use transactions.  Again though, they make large operations impossible.  Their primary limitation is that _all_ I/O is the equivalent of an "eventual" call, so any sort of communicating operation must be written such that it cannot include I/O.

Is this operation size limitation really a problem?  I believe so.  Programs are written as a hierarchy of encapsulation.  Your `main` function calls several other functions, they call several other functions, and so on, all the while using small (or large) primitive operations that may themselves encapsulate several other operations.  Trying to force such a program into a rigid dichotomy is doomed to failure.

An example of where they can fail is redirecting your stdout stream over the network.  stdout is typically done using "immediate" calls (as it refers to local files), and this lets you add logging at arbitrary points if using the message passing approach, but changing it to go over the network changes your call method to "eventual" (breaking your API), and making the operation too large for many of those users.

The result is we've got two approaches that both impose severe size limitations, and a real world that's intolerant of such size limitations.  The conclusion is you need to make practical compromises.  My choice is to use the state isolation of message passing, but allow "immediates" within "eventuals", and make sure to automatically detect any deadlocks that do occur.  To this I add a minimal amount of transactions, only on objects that are specifically transactable, and I don't prohibit I/O within them; if the programmer adds logging, it's their responsibility to handle any duplication.

Ultimately, good style and a robust language will produce correct programs, not a language that tries to make it impossible to go wrong.