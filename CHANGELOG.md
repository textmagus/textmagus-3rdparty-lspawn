# Changelog

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
