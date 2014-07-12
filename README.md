# lspawn

lspawn is a Lua module for spawning processes and asynchronously reading and
writing to them. lspawn is built to leverage either [GTK][] or POSIX standards
(but not both at the same time). Using GTK, lspawn runs on Windows, Mac OSX,
Linux, and BSD. Using POSIX, lspawn runs on Mac OSX, Linux, and BSD.

lspawn also allows Lua functions to be called when a process has data to read
from stdout and/or stderr. In addition, it can call a Lua function when a
process exits. See the *spawn.luadoc* and *proc.luadoc* files for complete API
documentation.

[GTK]: http://www.gtk.org

## Compiling

Compile lspawn for GTK or POSIX by running `make GLIB=1` or `make`,
respectively. This will build *spawn.so*, which can be `require()`ed by Lua.

## Usage

Using lspawn with GTK is easy, as long as you are running your application in a
`gtk_main()` loop. When Lua calls `spawn()`, lspawn automatically registers the
given callback functions (if any) with GTK and GTK calls those functions as
appropriate. (The [Textadept][] text editor is a Lua-scriptable GTK application
that makes use of this.)

Using lspawn with POSIX standards is a bit more difficult. In order to mimic
asynchronous I/O, the main idea is to use the POSIX `select()` function to
notify your application that a process has some data to read. lspawn provides
two C functions that work in tandem with `select()` in order to interact with
processes spawned via Lua's `spawn()`:

* `lspawn_pushfds()`: Pushes onto the Lua stack an `fd\_set` of all spawned
  processes and returns the corresponding `nfds` to pass to `select()`. (If your
  application is monitoring more processes on its own, ensure `nfds` is updated
  appropriately before calling `select()`.)
* `lspawn_readfds()`: Reads any output from the fds in the `fd\_set` at the top
  of the Lua stack and returns the number of fds read from. Also checks for
  child processes that have finished in the meantime.

The general sequence of events is:

1. Call `lspawn_pushfds()` to get an `fd\_set` of processes spawned by lspawn.
2. Have your application set any additional fds to monitor and update `nfds` if
   necessary.
3. Call `select()` with the `fd\_set` pushed by `lspawn_pushfds()`.
4. Call `lspawn_readfds()` to have lspawn call the appropriate Lua callback
   functions for processes have have output to read or for processes that have
   exited.
5. Pop the `fd\_set` pushed by `lspawn_pushfds()` to free memory.

Here's an example for an application that also monitors stdin along with the
processes spawned by lspawn:

    while (1) {
      /* ... */
      int nfds = lspawn_pushfds(L);
      fd_set *fds = (fd_set *)lua_touserdata(L, -1);
      FD_SET(0, fds); // monitor stdin; no need to update nfds
      if (select(nfds, fds, NULL, NULL, NULL) > 0) {
        if (FD_ISSET(0, fds)) {
          /* read stdin */
        }
        if (lspawn_readfds(L) > 0) {
          /* lspawn read data; do something more if necessary */
        }
      }
      lua_pop(L, 1); // fd_set; will be gc'ed by Lua
      /* ... */
    }

The terminal version of [Textadept][] does something similar in order to respond
to user keypresses while monitoring spawned processes. The implementation is
in *src/textadept.c* for reference (search for the "textadept\_waitkey"
function).

[Textadept]: http://foicica.com/textadept
