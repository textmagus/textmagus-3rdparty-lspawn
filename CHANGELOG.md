# Changelog

## 1.4 (25 Feb 2016)

* Exit immediately if the child process fails to start.
* Do not "busy wait" for spawned stdout or stderr on OSX.
* Only change environment of child processes on OSX if an environment was
  specified.
* Fix memory access error by disconnecting GTK listeners when `lua_close()` has
  been called.

## 1.3 (26 Sep 2015)

* Fixed small memory leak on OSX and the terminal.
* Ensure stdout and stderr callbacks are run before process exit.
* The environment of child processes can be specified.

## 1.2 (01 Apr 2015)

* Leading '*' in `proc:read()` argument is optional, like in Lua 5.3.
* `proc:kill()` can send signals to spawned processes.

## 1.1 (07 Jan 2015)

* Fixed compiler warnings and compile as C99.
* Added `proc:close()` method for sending EOF to spawned process.

## 1.0 (01 Oct 2014)

Initial release.
