# BIN Directory

The `bin` directory is where `recli` looks for programs to execute.
When given a command `foo`, it looks for an program named `bin/foo`.
If no such program exists, it looks for `bin/DEFAULT`.  If that does
not exist, it does nothing and executes no program.

The `recli` command-line arguments are passed to the program as
command-line options.  The name of the program is _not_ passed to it.

For example, if the command `foo bar` is entered, the `bin/foo`
program is executed, with command-line option `bar`.  This is exactly
the same as typing the following on the `sh` command-line:

    $ ./bin/foo bar

or with recli:

   recli> foo bar

The purpose of using `recli` as a wrapper is to ensure that only a
limited number of programs can be executed, and that the arguments to
those programs have limited values.

## Special Command-line options

`recli` assumes that every program takes a command-line option
`--config NAME`, where `NAME` is the name of a configuration option.
For now, only `--config syntax` is supported.  `recli` uses these
options to get additional information from each program.

### --config syntax

The `--config syntax`` option should print out the syntax accepted by
the program, not including the program name.  For example, for the
above syntax `foo bar`, the syntax should be obtained by:

    $ ./bin/foo --config syntax
    bar

`recli` will parse the output.  It remember that the `foo` program
accepts the command-line option `bar`.  It will determine that `foo
bar` is a valid syntax.

# Predefined Programs

There are a number of predefined programs.  These serve to give a
common "look and feel" to the `recli` CLI.  They also enable the
`plugins`.

## rehash

The `rehash` program runs all of the program in the `bin` directory,
and passes the `--config syntax` option to them.  It collates the
output, and saves it into the `cache/syntax.txt` file.

The syntax cache file is used by `recli` to simply the process of
loading the syntax, so that it does not have to re-run all of the
programs.

See the `syntax/README.md` file for more information.

## show

The `show` command is a wrapper which looks for programs in the
`plugins` directory.  The idea is that `recli` "proper" does not know
about the plugins.  Instead, it just blindly executes programs from
the `bin` directory.

By placing programs in he `plugins` directory, the `bin` directory
does not have to be modified when plugins are added.  In addition, the
plugins do not each have to begin their hierarchy with a `show`
directory.  The `plugins/README.md` file explains these concepts in
more detail.

The `show` command walks over its command-line options, trying to
execute programs from the `plugins` directory.  When given a command
`show foo bar`, it tries to run programs in the following order:

    plugins/foo/show bar
    plugins/foo/bar/show

The result is that plugins can be created hierarchically, with simple
names.

## add

This program is a symlink to `show`, and behaves identically to it.

The intention is to allow commands of the format `add foo`.

## mod

This program is a symlink to `show`, and behaves identically to it.

The intention is to allow commands of the format `mod foo`.

## del

This program is a symlink to `show`, and behaves identically to it.

The intention is to allow commands of the format `del foo`.

## DEFAULT

This program is executed by `recli` when no matching program has been
found.  It recurses through the `plugins` directory, as done above
with the `show` program.

The main difference between it and `show` is that it tries to run the
command as-is.  When given a command `foo bar baz`, it tries to run
programs in the following order:

    plugins/foo bar baz
    plugins/foo/bar baz
    plugins/foo/bar/baz

A future version will probably also look for:

    plugins/foo/bar/baz/DEFAULT
    plugins/foo/bar/DEFAULT baz
    plugins/foo/DEFAULT bar baz
