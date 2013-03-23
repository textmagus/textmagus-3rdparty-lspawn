// Copyright 2012-2013 Mitchell mitchell<att>foicica.com. See LICENSE.

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>
#if !_WIN32
#include <signal.h>
#else
#include <windows.h>
#define stdin _stdin
#define stdout _stdout
#define stderr _stderr
#define kill TerminateProcess
#define SIGKILL 0
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

typedef struct {
  lua_State *L;
  GPid pid;
  int stdin;
  int stdout;
  int stderr;
  int stdout_callback_ref;
  int stderr_callback_ref;
  int exit_callback_ref;
  int stdin_ref;
} spawn_state;

/**
 * Returns a Lua reference to the Lua function at the given stack position.
 * @param L The Lua state.
 * @param narg Position of the Lua function on the stack.
 * @return Lua reference or LUA_REFNIL
 */
static int l_callback_ref(lua_State *L, int narg) {
  luaL_argcheck(L, lua_isnone(L, narg) || lua_type(L, narg) == LUA_TFUNCTION ||
                   lua_isnil(L, narg), narg, "function or nil expected");
  lua_pushvalue(L, narg);
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

/**
 * Helper for reading stdout or stderr and passing it to the appropriate Lua
 * callback function.
 * @param ch GIOChannel.
 * @param cond GIOCondition.
 * @param L The Lua state.
 * @param ref The Lua reference for callback function.
 */
static gboolean std_(GIOChannel *ch, GIOCondition cond, lua_State *L, int ref) {
  if (!(cond & G_IO_IN)) return FALSE;
  char buf[1024];
  gsize len = 0;
  int status = g_io_channel_read_chars(ch, buf, 1024, &len, NULL);
  if (status == G_IO_STATUS_NORMAL && len > 0) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_isfunction(L, -1)) {
      lua_pushlstring(L, buf, len);
      lua_pcall(L, 1, 0, 0);
    } else lua_pop(L, 1); // non-function
  }
  return !(cond & G_IO_HUP);
}

/**
 * Callback for when child stdout has data to read.
 * @param source GIOChannel.
 * @param cond GIOCondition.
 * @param data spawn_state.
 */
static gboolean s_stdout(GIOChannel *source, GIOCondition cond, gpointer data) {
  spawn_state *S = (spawn_state *)data;
  return std_(source, cond, S->L, S->stdout_callback_ref);
}

/**
 * Callback for when child stderr has data to read.
 * @param source GIOChannel.
 * @param cond GIOCondition.
 * @param data spawn_state.
 */
static gboolean s_stderr(GIOChannel *source, GIOCondition cond, gpointer data) {
  spawn_state *S = (spawn_state *)data;
  return std_(source, cond, S->L, S->stderr_callback_ref);
}

/**
 * Adds a callback for the given file descriptor when data is available to read.
 * @param fd File descriptor returned by `g_spawn_async_with_pipes`.
 * @param S Spawn state.
 * @param out `TRUE` for stdout; stderr otherwise.
 */
static void add_io_callback(int fd, spawn_state *S, int out) {
#if !_WIN32
  GIOChannel *channel = g_io_channel_unix_new(fd);
#else
  GIOChannel *channel = g_io_channel_win32_new_fd(fd);
#endif
  g_io_channel_set_encoding(channel, NULL, NULL);
  g_io_channel_set_buffered(channel, FALSE);
  g_io_add_watch(channel, G_IO_IN | G_IO_HUP, out ? s_stdout : s_stderr, S);
  g_io_channel_unref(channel);
}

/**
 * Callback for when child process exits.
 * @param pid Child pid.
 * @param status Exit status.
 * @param data spawn_state.
 */
static void s_exit(GPid pid, int status, gpointer data) {
  spawn_state *S = (spawn_state *)data;
  lua_State *L = S->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, S->exit_callback_ref);
  if (lua_isfunction(L, -1)) {
    lua_pushinteger(L, status);
    lua_pcall(L, 1, 0, 0);
  } else lua_pop(L, 1); // non-function

  // Release resources.
  close(S->stdin);
  close(S->stdout);
  close(S->stderr);
  luaL_unref(L, LUA_REGISTRYINDEX, S->stdout_callback_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, S->stderr_callback_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, S->exit_callback_ref);
  lua_rawgeti(L, LUA_REGISTRYINDEX, S->stdin_ref);
  lua_pushnil(L);
  lua_setupvalue(L, -2, 1);
  lua_pop(L, 1); // cclosure
  luaL_unref(L, LUA_REGISTRYINDEX, S->stdin_ref);
  g_spawn_close_pid(S->pid);
  free(S);
}

/**
 * Function for writing a given string to stdin of spawned process.
 * A `nil` parameter kills the process.
 * @param L The Lua state.
 */
static int spawn_input(lua_State *L) {
  if (!lua_isnil(L, lua_upvalueindex(1))) {
    spawn_state *S = (spawn_state *)lua_touserdata(L, lua_upvalueindex(1));
    if (!lua_isnil(L, 1))
      write(S->stdin, luaL_checkstring(L, 1), lua_rawlen(L, 1));
    else
      kill(S->pid, SIGKILL);
  } else luaL_error(L, "invalid stdin (process finished)");
  return 0;
}

/**
 * Spawns a process.
 * The Lua parameters given are string working directory, string or table of
 * args for argv, environment table, stdin, stdout callback function, stderr
 * callback function, and exit callback function.
 * Pushes a closure onto the stack that accepts stdin.
 * @param L The Lua state.
 */
static int spawn(lua_State *L) {
  const char *working_dir = lua_tostring(L, 1);

  int i, n2, type2 = lua_type(L, 2);
  luaL_argcheck(L, type2 == LUA_TSTRING || type2 == LUA_TTABLE, 2,
                "string or table of args expected");
  n2 = (type2 == LUA_TTABLE) ? lua_rawlen(L, 2) : 1;
  char *argv[n2 + 1];
  if (type2 == LUA_TTABLE) {
    for (i = 0; i < n2; i++) {
      lua_rawgeti(L, 2, i + 1);
      luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2,
                    "string arg expected in args table");
      argv[i] = (char *)lua_tostring(L, -1);
      lua_pop(L, 1); // arg
    }
  } else argv[0] = (char *)lua_tostring(L, 2);
  argv[n2] = NULL;

  int j, n3, type3 = lua_type(L, 3);
  if (!lua_isnone(L, 3) && !lua_isnil(L, 3)) {
    luaL_argcheck(L, lua_istable(L, 3), 3, "table of envs expected");
    n3 = lua_rawlen(L, 3);
  } else n3 = 0;
  char *envp[n3 + 1];
  for (j = 0; j < n3; j++) {
    lua_rawgeti(L, 3, j + 1);
    luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2,
                  "string env expected in envs table");
    envp[j] = (char *)lua_tostring(L, -1);
    lua_pop(L, 1); // env
  }
  envp[n3] = NULL;

  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
  GPid pid;
  int stdin, stdout, stderr;
  if (g_spawn_async_with_pipes(working_dir, argv, (n3 > 0) ? envp : NULL, flags,
                               NULL, NULL, &pid, &stdin, &stdout, &stderr,
                               NULL)) {
    luaL_argcheck(L, lua_isnone(L, 4) || lua_type(L, 4) == LUA_TSTRING ||
                     lua_isnil(L, 4), 4, "string stdin or nil expected");
    if (!lua_isnone(L, 4) && !lua_isnil(L, 4))
      write(stdin, lua_tostring(L, 4), lua_rawlen(L, 4));
    spawn_state *S = (spawn_state *)malloc(sizeof(spawn_state));
    S->L = L;
    S->pid = pid;
    S->stdin = stdin;
    S->stdout = stdout;
    S->stderr = stderr;
    S->stdout_callback_ref = l_callback_ref(L, 5);
    S->stderr_callback_ref = l_callback_ref(L, 6);
    S->exit_callback_ref = l_callback_ref(L, 7);
    lua_pushlightuserdata(L, (void *)S);
    lua_pushcclosure(L, spawn_input, 1);
    lua_pushvalue(L, -1); // duplicate for return since luaL_ref pops
    S->stdin_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    add_io_callback(stdout, S, TRUE);
    add_io_callback(stderr, S, FALSE);
    g_child_watch_add(pid, s_exit, S);
    return 1;
  } else luaL_error(L, "could not spawn process");

  return 0;
}

int luaopen_spawn (lua_State *L) { return (lua_pushcfunction(L, spawn), 1); }
int luaopen_os_spawn(lua_State *L) { return luaopen_spawn(L); }
