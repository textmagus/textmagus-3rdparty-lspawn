// Copyright 2012-2015 Mitchell mitchell.att.foicica.com. See LICENSE.

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if GTK
#include <glib.h>
#endif
#if !_WIN32
#if (!GTK || __APPLE__)
#include <errno.h>
#include <sys/select.h>
#endif
#include <sys/wait.h>
#include <signal.h>
#else
#include <fcntl.h>
#include <windows.h>
#endif

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#if _WIN32
#define waitpid(pid, ...) WaitForSingleObject(pid, INFINITE)
#define kill(pid, _) TerminateProcess(pid, 1)
#define g_io_channel_unix_new g_io_channel_win32_new_fd
#define close CloseHandle
#define FD(handle) _open_osfhandle((intptr_t)handle, _O_RDONLY)
#if !GTK
// The following macro is only for quieting compiler warnings. Spawning in Win32
// console is not supported.
#define read(fd, buf, len) read((int)fd, buf, len)
#endif
#endif
#define l_setcfunction(l, n, name, f) \
  (lua_pushcfunction(l, f), lua_setfield(l, (n > 0) ? n : n - 1, name))
#define l_reffunction(l, n) \
  (luaL_argcheck(l, lua_isfunction(l, n) || lua_isnoneornil(l, n), n, \
                 "function or nil expected"), \
   lua_pushvalue(l, n), luaL_ref(l, LUA_REGISTRYINDEX))
#if LUA_VERSION_NUM < 502
#define LUA_OK 0
#define lua_rawlen lua_objlen
#endif

typedef struct {
  lua_State *L;
  int ref;
#if !_WIN32
  int pid, fstdin, fstdout, fstderr;
#else
  HANDLE pid, fstdin, fstdout, fstderr;
#endif
#if (GTK && !__APPLE__)
  GIOChannel *cstdout, *cstderr;
#endif
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

/** p:read() Lua function. */
static int lp_read(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  luaL_argcheck(L, p->pid, 1, "process terminated");
  const char *arg = luaL_optstring(L, 2, "*l"), c = *(arg + 1);
  luaL_argcheck(L, (arg[0] == '*' && (c == 'l' || c == 'L' || c == 'a')) ||
                   lua_isnumber(L, 2), 2, "invalid option");
#if (GTK && !__APPLE__)
  char *buf;
  size_t len;
  GError *error = NULL;
  GIOStatus status;
  if (!g_io_channel_get_buffered(p->cstdout))
    g_io_channel_set_buffered(p->cstdout, TRUE); // needed for functions below
  if (!lua_isnumber(L, 2)) {
    if (c == 'l' || c == 'L') {
      GString *s = g_string_new(NULL);
      status = g_io_channel_read_line_string(p->cstdout, s, NULL, &error);
      len = s->len, buf = g_string_free(s, FALSE);
    } else if (c == 'a') {
      status = g_io_channel_read_to_end(p->cstdout, &buf, &len, &error);
      if (status == G_IO_STATUS_EOF) status = G_IO_STATUS_NORMAL;
    }
  } else {
    size_t bytes = (size_t)lua_tointeger(L, 2);
    buf = malloc(bytes);
    status = g_io_channel_read_chars(p->cstdout, buf, bytes, &len, &error);
  }
  if ((g_io_channel_get_buffer_condition(p->cstdout) & G_IO_IN) == 0)
    g_io_channel_set_buffered(p->cstdout, FALSE); // needed for stdout callback
  lua_pushlstring(L, buf, len - ((buf[len - 1] == '\n' && c == 'l') ? 1 : 0));
  free(buf);
  if (status != G_IO_STATUS_NORMAL) {
    lua_pushnil(L);
    if (status == G_IO_STATUS_EOF) return 1;
    lua_pushinteger(L, error->code), lua_pushstring(L, error->message);
    return 3;
  } else return 1;
#else
  int len = 0, n = 0;
  if (!lua_isnumber(L, 2)) {
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    char ch;
    while ((n = read(p->fstdout, &ch, 1)) > 0) {
      if (ch != '\n' || c == 'L' || c == 'a') luaL_addchar(&buf, ch), len++;
      if (ch == '\n' && c != 'a') break;
    }
    if (n < 0 && len == 0) len = n;
    luaL_pushresult(&buf);
  } else {
    size_t bytes = (size_t)lua_tointeger(L, 2);
    char *buf = malloc(bytes);
    if ((len = read(p->fstdout, buf, bytes)) > 0) lua_pushlstring(L, buf, len);
    free(buf);
  }
  if (len <= 0) {
    lua_pushnil(L);
    if (len == 0) return 1;
    lua_pushinteger(L, errno), lua_pushstring(L, strerror(errno));
    return 3;
  } else return 1;
#endif
}

/** p:write() Lua function. */
static int lp_write(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  luaL_argcheck(L, p->pid, 1, "process terminated");
  for (int i = 2; i <= lua_gettop(L); i++) {
    size_t len;
    const char *s = luaL_checklstring(L, i, &len);
#if !_WIN32
    len = write(p->fstdin, s, len); // assign result to fix compiler warning
#else
    DWORD len_written;
    WriteFile(p->fstdin, s, len, &len_written, NULL);
#endif
  }
  return 0;
}

/** p:close() Lua function. */
static int lp_close(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  luaL_argcheck(L, p->pid, 1, "process terminated");
  return (close(p->fstdin), 0);
}

/** p:kill() Lua function. */
static int lp_kill(lua_State *L) {
  PStream *p = (PStream *)luaL_checkudata(L, 1, "ta_spawn");
  if (p->pid) kill(p->pid, SIGKILL);
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

#if (GTK && !__APPLE__)
/** Signal that channel output is available for reading. */
static int ch_read(GIOChannel *source, GIOCondition cond, void *data) {
  PStream *p = (PStream *)data;
  if (!p->pid || !(cond & G_IO_IN)) return FALSE;
  char buf[BUFSIZ];
  size_t len = 0;
  do {
    int status = g_io_channel_read_chars(source, buf, BUFSIZ, &len, NULL);
    int r = (source == p->cstdout) ? p->stdout_cb : p->stderr_cb;
    if (status == G_IO_STATUS_NORMAL && len > 0 && r > 0) {
      lua_rawgeti(p->L, LUA_REGISTRYINDEX, r);
      lua_pushlstring(p->L, buf, len);
      if (lua_pcall(p->L, 1, 0, 0) != LUA_OK)
        fprintf(stderr, "Lua: %s\n", lua_tostring(p->L, -1)), lua_pop(p->L, 1);
    }
  } while (len == BUFSIZ);
  return p->pid && !(cond & G_IO_HUP);
}

/**
 * Creates a new channel that monitors a file descriptor for output.
 * @param fd File descriptor returned by `g_spawn_async_with_pipes()` or
 *   `_open_osfhandle()`.
 * @param p PStream to notify when output is available for reading.
 * @param watch Whether or not to watch for output to send to a Lua callback.
 */
static GIOChannel *new_channel(int fd, PStream *p, int watch) {
  GIOChannel *channel = g_io_channel_unix_new(fd);
  g_io_channel_set_encoding(channel, NULL, NULL);
  g_io_channel_set_buffered(channel, FALSE);
  if (watch) {
    g_io_add_watch(channel, G_IO_IN | G_IO_HUP, ch_read, p);
    g_io_channel_unref(channel);
  }
  return channel;
}

/** Signal that the child process finished. */
static void p_exit(GPid pid, int status, void *data) {
  PStream *p = (PStream *)data;
  if (p->exit_cb != LUA_REFNIL) {
    lua_rawgeti(p->L, LUA_REGISTRYINDEX, p->exit_cb);
    lua_pushinteger(p->L, status);
    if (lua_pcall(p->L, 1, 0, 0) != LUA_OK)
      fprintf(stderr, "Lua: %s\n", lua_tostring(p->L, -1)), lua_pop(p->L, 1);
  }
#if _WIN32
  close(p->pid);
#endif
  close(p->fstdin), close(p->fstdout), close(p->fstderr);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->stdout_cb);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->stderr_cb);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->exit_cb);
  luaL_unref(p->L, LUA_REGISTRYINDEX, p->ref); // allow proc to be collected
  p->pid = 0;
}
#elif !_WIN32
/**
 * Pushes onto the stack an fd_set of all spawned processes for use with
 * `select()` and `lspawn_readfds()` and returns the `nfds` to pass to
 * `select()`.
 */
int lspawn_pushfds(lua_State *L) {
  int nfds = 1;
  fd_set *fds = (fd_set *)lua_newuserdata(L, sizeof(fd_set));
  FD_ZERO(fds);
  lua_getfield(L, LUA_REGISTRYINDEX, "spawn_procs");
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    PStream *p = (PStream *)lua_touserdata(L, -2);
    FD_SET(p->fstdout, fds);
    FD_SET(p->fstderr, fds);
    if (p->fstdout >= nfds) nfds = p->fstdout + 1;
    if (p->fstderr >= nfds) nfds = p->fstderr + 1;
    lua_pop(L, 1); // value
  }
  lua_pop(L, 1); // spawn_procs
  return nfds;
}

/** Signal that a fd has output to read. */
static void fd_read(int fd, PStream *p) {
  char buf[BUFSIZ];
  ssize_t len;
  do {
    len = read(fd, buf, BUFSIZ);
    int r = (fd == p->fstdout) ? p->stdout_cb : p->stderr_cb;
    if (len > 0 && r > 0) {
      lua_rawgeti(p->L, LUA_REGISTRYINDEX, r);
      lua_pushlstring(p->L, buf, len);
      if (lua_pcall(p->L, 1, 0, 0) != LUA_OK)
        fprintf(stderr, "Lua: %s\n", lua_tostring(p->L, -1)), lua_pop(p->L, 1);
    }
  } while (len == BUFSIZ);
}

/**
 * Reads any output from the fds in the fd_set at the top of the stack and
 * returns the number of fds read from.
 * Also signals any registered child processes that have finished and cleans up
 * after them.
 */
int lspawn_readfds(lua_State *L) {
  int n = 0;
  fd_set *fds = (fd_set *)lua_touserdata(L, -1);
  lua_getfield(L, LUA_REGISTRYINDEX, "spawn_procs");
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    PStream *p = (PStream *)lua_touserdata(L, -2);
    // Read output if any is available.
    if (FD_ISSET(p->fstdout, fds)) fd_read(p->fstdout, p), n++;
    if (FD_ISSET(p->fstderr, fds)) fd_read(p->fstderr, p), n++;
    // Check process status.
    int status;
    if (waitpid(p->pid, &status, WNOHANG) > 0) {
      fd_read(p->fstdout, p), fd_read(p->fstderr, p); // read anything left
      if (p->exit_cb != LUA_REFNIL) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->exit_cb);
        lua_pushinteger(L, status);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
          fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1)), lua_pop(L, 1);
      }
      close(p->fstdin), close(p->fstdout), close(p->fstderr);
      luaL_unref(L, LUA_REGISTRYINDEX, p->stdout_cb);
      luaL_unref(L, LUA_REGISTRYINDEX, p->stderr_cb);
      luaL_unref(L, LUA_REGISTRYINDEX, p->exit_cb);
      p->pid = 0;
      lua_pushnil(L), lua_replace(L, -2), lua_settable(L, -3); // t[proc] = nil
      lua_pushnil(L); // push nil for next call to lua_next()
    } else lua_pop(L, 1); // value
  }
  lua_pop(L, 1); // spawn_procs
  return n;
}

#if (GTK && __APPLE__)
static int monitoring_fds = 0;
/**
 * Monitors spawned fds when GTK is idle.
 * This is necessary because at the moment, using GLib on Mac OSX to spawn
 * and monitor file descriptors will abort when attempting to poll those fds.
 */
static int monitor_fds(void *data) {
  lua_State *L = (lua_State *)data;
  struct timeval timeout = {0, 0};
  int nfds = lspawn_pushfds(L);
  fd_set *fds = (fd_set *)lua_touserdata(L, -1);
  if (select(nfds, fds, NULL, NULL, &timeout) > 0) lspawn_readfds(L);
  lua_pop(L, 1); // fd_set
  if (nfds == 1) monitoring_fds = 0;
  return nfds > 1;
}
#endif
#endif

/** spawn() Lua function. */
static int spawn(lua_State *L) {
#if !_WIN32
#if (GTK && !__APPLE__)
  char **argv = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(luaL_checkstring(L, 1), NULL, &argv, &error)) {
    lua_pushfstring(L, "invalid argv: %s", error->message);
    luaL_argerror(L, 1, lua_tostring(L, -1));
  }
#else
  lua_newtable(L);
  const char *param = luaL_checkstring(L, 1), *c = param;
  while (*c) {
    while (*c == ' ') c++;
    param = c;
    if (*c == '"') {
      param = ++c;
      while (*c && *c != '"') c++;
    } else while (*c && *c != ' ') c++;
    lua_pushlstring(L, param, c - param);
    lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    if (*c == '"') c++;
  }
  int argc = lua_rawlen(L, -1);
  char **argv = malloc((argc + 1) * sizeof(char *));
  for (int i = 0; i < argc; i++) {
    lua_rawgeti(L, -1, i + 1);
    size_t len = lua_rawlen(L, -1);
    char *param = malloc(len + 1);
    strcpy(param, lua_tostring(L, -1)), param[len] = '\0';
    argv[i] = param;
    lua_pop(L, 1); // param
  }
  argv[argc] = NULL;
  lua_pop(L, 1); // argv
#endif
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
  p->L = L, p->ref = 0;
  p->stdout_cb = l_reffunction(L, 3), p->stderr_cb = l_reffunction(L, 4);
  p->exit_cb = l_reffunction(L, 5);
  if (luaL_newmetatable(L, "ta_spawn")) {
    l_setcfunction(L, -1, "status", lp_status);
    l_setcfunction(L, -1, "wait", lp_wait);
    l_setcfunction(L, -1, "read", lp_read);
    l_setcfunction(L, -1, "write", lp_write);
    l_setcfunction(L, -1, "close", lp_close);
    l_setcfunction(L, -1, "kill", lp_kill);
    l_setcfunction(L, -1, "__tostring", lp_tostring);
    lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
  }
  lua_setmetatable(L, -2);

#if !_WIN32
#if (GTK && !__APPLE__)
  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
  if (g_spawn_async_with_pipes(lua_tostring(L, 2), argv, NULL, flags, NULL,
                               NULL, &p->pid, &p->fstdin, &p->fstdout,
                               &p->fstderr, &error)) {
    p->cstdout = new_channel(p->fstdout, p, p->stdout_cb > 0);
    p->cstderr = new_channel(p->fstderr, p, p->stderr_cb > 0);
    g_child_watch_add(p->pid, p_exit, p);
    lua_pushnil(L);
  } else {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", lua_tostring(L, 1), error->message);
  }

  g_strfreev(argv);
#else
  // Adapted from Chris Emerson and GLib.
  // Attempt to create pipes for stdin, stdout, and stderr and fork process.
  int pstdin[2] = {-1, -1}, pstdout[2] = {-1, -1}, pstderr[2] = {-1, -1}, pid;
  if (pipe(pstdin) == 0 && pipe(pstdout) == 0 && pipe(pstderr) == 0 &&
      (pid = fork()) >= 0) {
    if (pid > 0) {
      // Parent process: register child for monitoring its fds and pid.
      close(pstdin[0]), close(pstdout[1]), close(pstderr[1]);
      p->pid = pid;
      p->fstdin = pstdin[1], p->fstdout = pstdout[0], p->fstderr = pstderr[0];
      lua_getfield(L, LUA_REGISTRYINDEX, "spawn_procs");
      // spawn_procs is of the form: t[proc] = true
      lua_pushvalue(L, -2), lua_pushboolean(L, 1), lua_settable(L, -3);
      lua_pop(L, 1); // spawn_procs
      lua_pushnil(L);
#if (GTK && __APPLE__)
      // On GTK-OSX, manually monitoring spawned fds prevents the fd polling
      // aborts caused by GLib.
      if (!monitoring_fds) g_idle_add(monitor_fds, L), monitoring_fds = 1;
#endif
    } else if (pid == 0) {
      // Child process: redirect stdin, stdout, and stderr, chdir, and exec.
      close(pstdin[1]), close(pstdout[0]), close(pstderr[0]);
      close(0), close(1), close(2);
      dup2(pstdin[0], 0), dup2(pstdout[1], 1), dup2(pstderr[1], 2);
      close(pstdin[0]), close(pstdout[1]), close(pstderr[1]);
      const char *cwd = lua_tostring(L, 2);
      if (cwd && chdir(cwd) < 0) {
        fprintf(stderr, "Failed to change directory '%s' (%s)", cwd,
                strerror(errno));
        exit(EXIT_FAILURE);
      }
      execvp(argv[0], argv); // does not return on success
      fprintf(stderr, "Failed to execute child process \"%s\" (%s)", argv[0],
              strerror(errno));
    }
  } else {
    if (pstdin[0] >= 0) close(pstdin[0]), close(pstdin[1]);
    if (pstdout[0] >= 0) close(pstdout[0]), close(pstdout[1]);
    if (pstderr[0] >= 0) close(pstderr[0]), close(pstderr[1]);
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", lua_tostring(L, 1), strerror(errno));
  }
  for (int i = 0; i < argc; i++) free(argv[i]);
  free(argv);
#endif
#else
#if GTK
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
    p->cstdout = new_channel(FD(proc_stdout), p, p->stdout_cb > 0);
    p->cstderr = new_channel(FD(proc_stderr), p, p->stderr_cb > 0);
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
#else
  luaL_error(L, "not implemented in this environment");
#endif
#endif
  if (lua_isuserdata(L, -2))
    p->ref = (lua_pushvalue(L, -2), luaL_ref(L, LUA_REGISTRYINDEX));

  return 2;
}

int luaopen_spawn(lua_State *L) {
#if LUA_VERSION_NUM < 502
  lua_register(L, "spawn", spawn);
#endif
#if (!GTK || __APPLE__)
  // Need to keep track of running processes for monitoring fds and pids.
  lua_newtable(L), lua_setfield(L, LUA_REGISTRYINDEX, "spawn_procs");
#endif
  return (lua_pushcfunction(L, spawn), 1);
}
