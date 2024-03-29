# Configuration Directory

When recli is started with the `-d dir` option, it looks for a
number of configuration files in that directory:

* `banner.txt`
  
  The contents of this file are printed as a "login banner" when recli first starts.

  When this file does not exist, no banner text is printed.

* `ENV`

  The contents of this file are set as environment variables for any program that is executed.  See also `run`.

  When this file does not exist, the variables from the current environment are used.  Be aware that for SUID programs, this is likely to be wrong, and to create security problems.

  The format of the `ENV` file is "VARIABLE=VALUE", as printed by `printenv`.  Blank lines are ignored.  Comments are not allowed.

  As a special case, some variables can be passed through from the
  users environment.  This is most useful for things like
  `SSH_AUTH_SOCK`.  You can set

    SSH_AUTH_SOCK=$SSH_AUTH_SOCK

  and the variable will be passed through from the environment.

* `help.md`
  
  The help text is loaded from this file.  See HELP.md

  When this file does not exist, help is not available.

* `run`
  
  An executable program which is called after the input passes both syntax checks and permission checks.  The input is passed to it as a series of arguments, exactly as entered by the user.  This includes quotation marks surrounding STRING parameters.

  When this file does not exist, recli does nothing with the input.

* `syntax.txt`
  
  The syntax is loaded from this file.  See SYNTAX.md

  When this file does not exist, any input is allowed.

* `permission/`
  
  The permissions are loaded from this directory.

  When this directory not exist, or the relevant files are empty, any input is allowd.

* `permission/$USER.txt`
  
  The permissions for the $USER (i.e. user name) are loaded from this file.  If it does not exist, the `DEFAULT.txt` file is used instead

* `permission/DEFAULT.txt`
  
  The default set of permissions to apply when there is no per-user permission file.  This can be used to deny access to everyone other than authorized users.
  
  If this file contains only `!*`, i.e. no permissions at all, then recli will refuse to accept any commands users where the `DEFAULT.txt` permissions are applied.  The recli command will start, determine that the user has no permission to run any command, and immediately exit.
