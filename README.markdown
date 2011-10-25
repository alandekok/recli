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
Strings are allowed, via either "this is a string", or 'this is a
string'.  Other than the quote character, both strings are treated
identically.

At its base, Recli simple parses command lines with editing, and then
chops the command-line into space-separated words.  It does not do
anything else with the input unless you tell it to.

## Functionality

 * Customizable syntax is documented in SYNTAX.markdown.  Recli can load a pre-defined syntax from a simple text file.  When a syntax has been defined, the only input which will be accepted is input which matches the syntax.  All other input will produce a syntax error.

 * Permissions are documented in PERMISSION.markdown.  Recli can load a Cisco IOS-style permissions file.  This file contains a list of which commands are allowed and which ones are forbidden.  It can be used in combination with the syntax, or by itself.

 * Customizable help text is documented in HELP.markdown.  Recli can load contextual help text, and display it when the user types '?'.  If no help text is defined, the closest matching syntax is printed.

 * Recli supports tab completion for syntaxes.  Pressing <TAB> will result in it switching between the various options.  The tab completion is based on the Linenoise functionality, and may not match exactly what you expect from readline.

 * Recli has predefined data types, which are easily extensible.  It includes support for INTEGER, IPADDR, STRING, and a number of other common data types.  See DATATYPES.markdown.

 * When '-r dir' is passed as a command-line option, Recli will look for a program to run, and pass the command-line arguments to it.  It treats each word in the input as a directory or file.  It looks for an executable in the given directory, and runs it with the passed parameters.  Entering "foo bar baz" will cause it to look for an executable "dir/foo/bar/baz".  If the file "bar" is an executable, then it is executed, with command-line argument "baz".

## To Do List

 * Handle multiple permissions files

 * allow laoding of syntaxes / permissions from static buffers instead of files.

 * more regression tests for syntaxes and permissions
