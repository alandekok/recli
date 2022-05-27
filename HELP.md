# Help text

The CLI will read specially formatted files, and use them as help
text.  To keep things simple, the format of the help files is
Markdown.

## Long Form Help

For a syntax like this:

    one fish

We can have the following long form help text:

    # one

    Help for "one" command
    
    ## one fish
    
    Help for "one fish" command.

When the user types `help one`, the text 'Help for the "one"
command' is printed.  When user types `help one fish`, the text
'Help for the "one fish" command' is printed.  Help is
context-sensitive.  Typing `one<ENTER>`, followed by `help fish`
is identical to `help one fish`.

The parser is simple.  Any line starting with a hash is a Markdown
header, with title text.  The title is interpreted as a command which
can produce help.  The text until the next title is help for that
command.

To keep the help file manageable, the titles should be simple lists of
words, without alternation or optional syntax.  For example, help for
a more complex syntax like this:

    hello (a|b)

is simple:

    # hello
    
    Say "hello" to someone.  Choose "a" or "b"
    
    ## hello a
    
    Say "hello" to "a"
    
    ## hello b
    
    Say "hello" to "b"

We have used multiple hashes to get multi-level headers in Markdown.
These are useful for Markdown formatting, but are not used by the CLI
help file parser.  One '#' is treated just the same as two or three
'#'.

## Short Form Help

The `?` character can also be used to show help.  It is different from
the `help` command, in that it either prints syntax, or
contex-specific help.

In the above example, the syntax is printed when the user types `?`.
Additional help can be shown by adding another line after the heading,
which has four spaces at the beginning.  That line will become the
"short" help, which is printed when the user types `one ?`.

    # one

        Enter one thing.

    The "one" command does a number of things.
    It is a very useful command.
    
    ## one fish

        Enter one fish.
    
    Help for "one fish" command.

It may not be obvious in an HTML formatted page, but the "Enter a
fish" text is indented by four spaces.  This means that the text is
help for the `?` character, and is not part of the text for the `help`
command.

With this help file, when the user types `one fish ?`, the following
output will be printed:

    fish: Enter a fish

If there are multiple options for the current command, each option is
printed on a separate line, followed by its short form help.
