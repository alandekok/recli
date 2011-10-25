# Help text

The CLI will read specially formatted files, and use them as help
text.  To keep things simple, the format of the help files is
Markdown.

For a syntax like this:

    one fish

We can have the following help text:

    # one
    
    Help for "one" command
    
    ## one fish
    
    Help for "one fish" command.

The parse is simple.  Any line starting with a hash is a Markdown
header, with title text.  The title is interpreted as a command which
can produce help.  The text until the next title is help for that
command.

To keep it simple, the titles should be simple lists of words, without
alternation or optional syntax.  For example, help for a more complex
syntax like this:

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
