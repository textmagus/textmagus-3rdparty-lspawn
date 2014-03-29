// Copyright 2012-2014 Mitchell mitchell<att>foicica.com. See LICENSE.

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#if _WIN32
#include <windows.h>
#include <fcntl.h>
#define waitpid(pid, ...) WaitForSingleObject(pid, INFINITE)
#define write(fd, s, len) WriteFile(fd, s, len, NULL, NULL)
#define kill(pid, _) TerminateProcess(pid, 0)
#define g_io_channel_unix_new g_io_channel_win32_new_fd
#define close CloseHandle
#define FD(handle) _open_osfhandle((intptr_t)handle, _O_RDONLY)
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#define l_setcfunction(l, n, name, f) \
  (lua_pushcfunction(l, f), lua_setfield(l, (n > 0) ? n : n - 1, name))
#define l_reffunction(l, n) \
  (luaL_argcheck(l, lua_isfunction(l, n) || lua_isnoneornil(l, n), n, \
                 "function or nil expected"), \
   lua_pushvalue(l, n), luaL_ref(l, LUA_REGISTRYINDEX))

typedef struct {
  lua_State *L;
#if !_WIN32
  int pid, fstdin, fstdout, fstderr;
#else
  HANDLE pid, fstdin, fstdout, fstderr;
#endif
  GIOChannel *cstdout, *cstderr;
  int stdout_cb, stderr_cb, exit_cb;
} PStream;

/** p:status() Lua function. */
static int lp_status(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  lua_pushstring(L, p->pid ? "running" : "terminated");
  return 1;
}

/** p:wait() Lua function. */
static int lp_wait(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  luaL_argcheck(L, p->pid, 1, "process terminated");
  waitpid(p->pid, NULL, 0);
  return 0;
}

/** p:write() Lua function. */
static int lp_write(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  luaL_argcheck(L, p->pid, 1, "process terminated");
  size_t len;
  int i;
  for (i = 2; i <= lua_gettop(L); i++) {
    const char *s = luaL_checklstring(L, i, &len);
    write(p->fstdin, s, len);
  }
  return 0;
}

/** p:kill() Lua function. */
static int lp_kill(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  if (p->pid) kill(p->pid, SIGKILL), p->pid = 0;
  return 0;
}

/** tostring(p) Lua function. */
static int lp_tostring(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  if (p->pid)
    lua_pushfstring(L, "process (pid=%d)", p->pid);
  else
    lua_pushstring(L, "process (terminated)");
  return 1;
}

/** Signal that channel output is available for reading. */
static int ch_read(GIOChannel *source, GIOCondition cond, void *data) {
  PStream *p = (PStream *)data;
  if (!(cond & G_IO_IN)) return FALSE;
  char buf[BUFSIZ];
  size_t len = 0;
  do {
    int status = g_io_channel_read_chars(source, buf, BUFSIZ, &len, NULL);
    int r = (source == p->cstdout) ? p->stdout_cb : p->stderr_cb;
    if (status == G_IO_STATUS_NORMAL && len > 0 && r > 0) {
      lua_rawgeti(p->L, LUA_REGISTRYINDEX, r);
      lua_pushlstring(p->L, buf, len);
      lua_pcall(p->L, 1, 0, 0);
    }
  } while (len == BUFSIZ);
  return !(cond & G_IO_HUP);
}

/**
 * Creates a new channel that monitors a file descriptor for output.
 * @param fd File descriptor returned by `g_spawn_async_with_pipes()` or
 *   `_open_osfhandle()`.
 * @param p PStream to notify when output is available for reading.
 */
static GIOChannel *new_channel(int fd, PStream *p) {
  GIOChannel *channel = g_io_channel_unix_new(fd);
  g_io_channel_set_encoding(channel, NULL, NULL);
  g_io_channel_set_buffered(channel, FALSE);
  g_io_add_watch(channel, G_IO_IN | G_IO_HUP, ch_read, p);
  g_io_channel_unref(channel);
  return channel;
}

/** Signal that the child process finished. */
static void p_exit(GPid pid, int status, void *data) {
  PStream *p = (PStream *)data;
  if (p->exit_cb != LUA_REFNIL) {
    lua_rawgeti(p->L, LUA_REGISTRYINDEX, p->exit_cb);
    lua_pushinteger(p->L, status);
    lua_pcall(p->L, 1, 0, 0);
  }
#if _WIN32
  close(p->pid);
#endif
  close(p->fstdin), close(p->fstdout), close(p->fstderr);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->stdout_cb);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->stderr_cb);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->exit_cb);
  p->pid = 0;
}

/** spawn() Lua function. */
static int spawn(lua_State *L) {
#if !_WIN32
  char **argv = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(luaL_checkstring(L, 1), NULL, &argv, &error)) {
    lua_pushfstring(L, "invalid argv: %s", error->message);
    luaL_argerror(L, 1, lua_tostring(L, -1));
  }
#else
  lua_pushstring(L, getenv("COMSPEC"));
  lua_pushstring(L, " /c ");
  lua_pushvalue(L, 1);
  lua_concat(L, 3);
  lua_replace(L, 1); // cmd = os.getenv('COMSPEC')..' /c '..cmd
  wchar_t argv[2048] = {L'\0'}, cwd[MAX_PATH] = {L'\0'};
  MultiByteToWideChar(GetACP(), 0, lua_tostring(L, 1), -1, (LPWSTR)&argv,
                      sizeof(argv));
  MultiByteToWideChar(GetACP(), 0, lua_tostring(L, 2), -1, (LPWSTR)&cwd,
                      MAX_PATH);
#endif
  lua_settop(L, 5); // ensure 5 values so userdata to be pushed is 6th

  PStream *p = (PStream *)lua_newuserdata(L, sizeof(PStream));
  p->L = L;
  if (luaL_newmetatable(L, "ta_spawn")) {
    l_setcfunction(L, -1, "status", lp_status);
    l_setcfunction(L, -1, "wait", lp_wait);
    l_setcfunction(L, -1, "write", lp_write);
    l_setcfunction(L, -1, "kill", lp_kill);
    l_setcfunction(L, -1, "__tostring", lp_tostring);
    lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
  }
  lua_setmetatable(L, -2);

#if !_WIN32
  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
  if (g_spawn_async_with_pipes(lua_tostring(L, 2), argv, NULL, flags, NULL,
                               NULL, &p->pid, &p->fstdin, &p->fstdout,
                               &p->fstderr, &error)) {
    p->cstdout = new_channel(p->fstdout, p), p->stdout_cb = l_reffunction(L, 3);
    p->cstderr = new_channel(p->fstderr, p), p->stderr_cb = l_reffunction(L, 4);
    p->exit_cb = l_reffunction(L, 5);
    g_child_watch_add(p->pid, p_exit, p);
    lua_pushnil(L);
  } else {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", lua_tostring(L, 1), error->message);
  }

  g_strfreev(argv);
#else
  // Adapted from SciTE.
  SECURITY_DESCRIPTOR sd;
  InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), 0, 0};
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = &sd;
  sa.bInheritHandle = TRUE;

  // Redirect stdin.
  HANDLE stdin_read = NULL, proc_stdin = NULL;
  CreatePipe(&stdin_read, &proc_stdin, &sa, 0);
  SetHandleInformation(proc_stdin, HANDLE_FLAG_INHERIT, 0);
  // Redirect stdout.
  HANDLE proc_stdout = NULL, stdout_write = NULL;
  CreatePipe(&proc_stdout, &stdout_write, &sa, 0);
  SetHandleInformation(proc_stdout, HANDLE_FLAG_INHERIT, 0);
  // Redirect stderr.
  HANDLE proc_stderr = NULL, stderr_write = NULL;
  CreatePipe(&proc_stderr, &stderr_write, &sa, 0);
  SetHandleInformation(proc_stderr, HANDLE_FLAG_INHERIT, 0);

  // Spawn with pipes and no window.
  STARTUPINFOW startup_info = {
    sizeof(STARTUPINFOW), NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0,
    STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES, SW_HIDE, 0, 0, stdin_read,
    stdout_write, stderr_write
  };
  PROCESS_INFORMATION proc_info = {0, 0, 0, 0};
  if (CreateProcessW(NULL, argv, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP,
                     NULL, *cwd ? cwd : NULL, &startup_info, &proc_info)) {
    p->pid = proc_info.hProcess;
    p->fstdin = proc_stdin, p->fstdout = proc_stdout, p->fstderr = proc_stderr;
    p->cstdout = new_channel(FD(proc_stdout), p);
    p->stdout_cb = l_reffunction(L, 3);
    p->cstderr = new_channel(FD(proc_stderr), p);
    p->stderr_cb = l_reffunction(L, 4);
    p->exit_cb = l_reffunction(L, 5);
    g_child_watch_add(p->pid, p_exit, p);
    // Close unneeded handles.
    CloseHandle(proc_info.hThread);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write), CloseHandle(stderr_write);
    lua_pushnil(L);
  } else {
    char *message = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&message,
                   0, NULL);
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", lua_tostring(L, 1), message);
    LocalFree(message);
  }
#endif

  return 2;
}

int luaopen_spawn(lua_State *L) { return (lua_pushcfunction(L, spawn), 1); }
