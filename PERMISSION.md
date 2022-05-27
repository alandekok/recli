# Permissions

The CLI can read a file which contains a list of permitted commands.
The format is similar to the common Cisco style.

## Basic Syntax

The file format for the permission definitions is straightforward.  At the
simplest level, it is just a copy of commands that the user is permitted
to type:

    foo bar baz

Will allow the user to type "foo bar baz".  Anything else is a permissions
error.

Multiple lines can be used:

    foo bar baz
    bar bad
    hello there

Again, any of these commands are allowed.

These examples are not very useful, as the default is to permit all
commands.  The additional capabilities listed below make the
permissions much more useful.

## Comments and Blank Lines

Comments can be used, and blank lines are ignored:

    # allow foo ...
    foo bar baz
    
    # and bar, too
    bar bad
    
    # and hello!
    hello there

## Forbidden Commands

Commands can be forbidden by prefixing them with a "!" character:

    # allow foo bar
    foo bar
    # disallow all other commands related to "foo"
    !foo

## Priority of Permissions

The above example also introduces the concept of priority.  The
permissions are checked from the start of the file to the end.  The
first matching permission is used.  If a permission does not match, the
subsequent one is checked.

You should generally list permitted commands first, and then deny all
other commands.

## Wildcards

Wildcards are allowed, though they are not necessary.  The following
two permissions are equivalent:

    # Allow "foo bar", and disallow other "foo"
    foo bar
    !foo

or with wildcards:

    # Allow "foo bar", and disallow other "foo"
    foo bar
    !foo *

Wildcards are mainly used for allowing any "middle" command, or for
denying a subset of commands:

    # Allow "foo * baz", disallowing other "foo" commands
    foo * baz
    !foo

Allow a few commands, and disallow all others:

      foo bar
      bar a
      !*

## Caveats

There are no sanity checks on the input permissions, other than that
it is correctly formatted.  If you specify conflicting, ambiguous, or
duplicate permissions, then the parser will enforce them all.  You
will not be warned that this is happening.
