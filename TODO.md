## To Do List

 * don't allow "help" or other reserved words as commands

 * Handle multiple permissions files

 * allow loading of syntaxes / permissions from static buffers instead of files.

 * add deamon mode

    * recli --daemon /path/to/fifo (or TCP socket)

    * recli --client /path/to/fifo (or TCP socket)

 * Add TCP mode, so the commands get sent over a TCP connection.  We do NOT want to add SSL support.
  That should be done via "stunnel", maybe via "inetd" mode?

 * more regression tests for syntaxes and permissions

 * add a REST layer, so that it can convert CLI commands to REST commands.  This allows any web site REST API to be poked via a simple CLI.

### Client-Server spec

Via the following commands

    -> get commands
    <- command blah
    <- command blah
    <- done
    -> get help
    <- help blah
    <- help blah
    <- done
    -> get permissions
    <- permission blah
    <- permission blah
    <- done
    -> get prompt
    <- prompt blah
    <- done

Once it's boot-strapped, it prints the prompt, and waits for user input.  When it wants to run something, it sends it to the master, which responds:

    -> run blah blah
    <- success
    <- text blah
    <- text blah
    <- error blah
    <- done

Or instead of "success", "fail".  A command can potentially produce text on both stdout and stderr, so we need to distinguish the two streams.

The above two features will allow recli to be used as a login shell, while there's a "master" daemon on the same machine.

