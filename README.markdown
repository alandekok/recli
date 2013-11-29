# Recli

A minimal, zero-config, BSD licensed, CLI based on linenoise, a readline replacement.

The name comes from "re-cli", or a re-imagination of the traditional CLI.

## Why?

Command-line interfaces (CLIs) are widely used on Unix systems.  The
problem is that every one is different.  This project creates a
standardized CLI binary based on
[linenoise](https://github.com/antirez/linenoise/), which is a minimal,
zero-config, BSD licensed, readline replacement.

This project follows on the minimal nature of linenoise.

Recli is based on the typical "words separated by spaces" paradigm.
Strings are allowed, via either `"this is a string"`, or `'this is a
string'`.  Other than the quote character, both strings are treated
identically.

At its base, Recli simple parses command lines with editing, and then
chops the command-line into space-separated words.  It does not do
anything else with the input unless you tell it to.

## Functionality

 * Customizable syntax is documented in [SYNTAX.markdown](SYNTAX.markdown).  Recli can load a pre-defined syntax from a simple text file.  When a syntax has been defined, the only input which will be accepted is input which matches the syntax.  All other input will produce a syntax error.

 * Permissions are documented in [PERMISSION.markdown](PERMISSION.markdown).  Recli can load a Cisco IOS-style permissions file.  This file contains a list of which commands are allowed and which ones are forbidden.  It can be used in combination with the syntax, or by itself.

 * Customizable help text is documented in [HELP.markdown](HELP.markdown).  Recli can load contextual help text, and display it when the user types `?`, or `help`.  If no help text is defined, the closest matching syntax is printed.

 * Recli supports tab completion for syntaxes.  Pressing TAB will result in it switching between the various options.  The tab completion is based on the Linenoise functionality, and may not match exactly what you expect from readline.

 * Recli has predefined data types, which are easily extensible.  It includes support for INTEGER, IPADDR, STRING, and a number of other common data types.  See [DATATYPES.markdown](DATATYPES.markdown).

 * Partial commands can be entered.  When that happens, the prompt changes to indicate that more text is expected.  The final part of the command can be entered by itself.  This is useful when you need to entry a number of similar, but long commands.  Just enter the common prefix once, and then the unique trailing portions.

 * Configuration files can be placed in a subdirectory.  See [init/README.markdown](init/README.markdown)

## To Do List

 * Handle multiple permissions files

 * allow loading of syntaxes / permissions from static buffers instead of files.

 * if the binary isn't named "recli", look in /etc/recli/$argv0.conf for syntax, etc.

 * Add "fifo" mode.  "recli --master fifo " listens on a fifo for commands, and runs them.
  recli --child fifo" reads syntax, etc. from the fifo.  It then writes commands to the fifo,
  and reads results.  The syntax for the fifo should (of course) be in recli format.  Something
  like "COMMAND ..." and "RESULT ..." for back and forth communication.  All commands should be
  blocking on the child.

  The above two features will allow recli to be used as a login shell, while there's a "master"
  daemon on the same machine.

 * Add TCP mode, so the commands get sent over a TCP connection.  We do NOT want to add SSL support.
  That should be done via "stunnel", maybe via "inetd" mode?

 * more regression tests for syntaxes and permissions
