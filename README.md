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

 * Customizable syntax is documented in [SYNTAX.md](SYNTAX.md).  Recli can load a pre-defined syntax from a simple text file.  When a syntax has been defined, the only input which will be accepted is input which matches the syntax.  All other input will produce a syntax error.

 * Permissions are documented in [PERMISSION.md](PERMISSION.md).  Recli can load a Cisco IOS-style permissions file.  This file contains a list of which commands are allowed and which ones are forbidden.  It can be used in combination with the syntax, or by itself.

 * Customizable help text is documented in [HELP.md](HELP.md).  Recli can load contextual help text, and display it when the user types `?`, or `help`.  If no help text is defined, the closest matching syntax is printed.

 * Recli supports tab completion for syntaxes.  Pressing TAB will result in it switching between the various options.  The tab completion is based on the Linenoise functionality, and may not match exactly what you expect from readline.

 * Recli has predefined data types, which are easily extensible.  It includes support for INTEGER, IPADDR, STRING, and a number of other common data types.  See [DATATYPES.md](DATATYPES.md).

 * Partial commands can be entered.  When that happens, the prompt changes to indicate that more text is expected.  The final part of the command can be entered by itself.  This is useful when you need to entry a number of similar, but long commands.  Just enter the common prefix once, and then the unique trailing portions.

 * Configuration files can be placed in a subdirectory.  A full example is provided in the `config` directory; see [config/README.md](config/README.md) for more details.

## Usage

```
make
./recli
```

