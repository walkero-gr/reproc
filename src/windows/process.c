#include "process.h"
#include "util.h"

#include <assert.h>
#include <malloc.h>
#include <windows.h>

typedef struct process {
  HANDLE stdin;
  HANDLE stdout;
  HANDLE stderr;
  HANDLE child_stdin;
  HANDLE child_stdout;
  HANDLE child_stderr;
  PROCESS_INFORMATION info;
} process;

// Create each process in a new process group so we can send separate CTRL-BREAK
// signals to each of them
static const DWORD CREATION_FLAGS = CREATE_NEW_PROCESS_GROUP;

PROCESS_ERROR process_init(process *process)
{
  assert(process);

  process->stdin = NULL;
  process->stdout = NULL;
  process->stderr = NULL;
  process->child_stdin = NULL;
  process->child_stdout = NULL;
  process->child_stderr = NULL;

  ZeroMemory(&process->info, sizeof(PROCESS_INFORMATION));
  process->info.hThread = NULL;
  process->info.hProcess = NULL;
  process->info.dwProcessId = 0;

  SetLastError(0);

  pipe_init(&process->child_stdin, &process->stdin, &process->stdin) &&
      pipe_init(&process->stdout, &process->child_stdout, &process->stdout) &&
      pipe_init(&process->stderr, &process->child_stderr, &process->stderr);

  PROCESS_ERROR error = system_error_to_process_error(GetLastError());

  return error;
}

PROCESS_ERROR process_start(process *process, int argc, char *argv[])
{
  assert(process);

  assert(argc > 0);
  assert(argv);
  assert(argv[argc]);

  // Make sure process was initialized completely
  assert(process->stdin);
  assert(process->stdout);
  assert(process->stderr);
  assert(process->child_stdin);
  assert(process->child_stdout);
  assert(process->child_stderr);

  // Make sure process_start is only called once for each process_init call
  assert(!process->info.hThread);
  assert(!process->info.hProcess);

  for (int i = 0; i < argc; i++) {
    assert(argv[i]);
  }

  STARTUPINFOW startup_info;
  ZeroMemory(&startup_info, sizeof(STARTUPINFOW));
  startup_info.cb = sizeof(STARTUPINFOW);
  startup_info.dwFlags |= STARTF_USESTDHANDLES;

  // Assign child pipe endpoints to child process stdin/stdout/stderr
  startup_info.hStdInput = process->child_stdin;
  startup_info.hStdOutput = process->child_stdout;
  startup_info.hStdError = process->child_stderr;

  // Join argv to whitespace delimited string as required by CreateProcess
  char *command_line_string = string_join(argv, argc);
  // Convert utf-8 to utf-16 as required by CreateProcessW
  wchar_t *command_line_wstring = string_to_wstring(command_line_string);

  SetLastError(0);

  CreateProcessW(NULL, command_line_wstring, NULL, NULL, TRUE, CREATION_FLAGS,
                 NULL, NULL, &startup_info, &process->info);

  free(command_line_string);
  free(command_line_wstring);

  PROCESS_ERROR error = system_error_to_process_error(GetLastError());

  CloseHandle(process->child_stdin);
  CloseHandle(process->child_stdout);
  CloseHandle(process->child_stderr);
  CloseHandle(process->info.hThread);

  // CreateProcessW error has priority over CloseHandle errors
  error = error != PROCESS_SUCCESS
              ? error
              : system_error_to_process_error(GetLastError());

  return error;
}

PROCESS_ERROR process_write_stdin(process *process, const void *buffer,
                                  uint32_t to_write, uint32_t *actual)
{
  assert(process);
  assert(process->stdin);
  assert(buffer);

  return pipe_write(process->stdin, buffer, to_write, actual);
}

PROCESS_ERROR process_read_stdout(process *process, void *buffer,
                                  uint32_t to_read, uint32_t *actual)
{
  assert(process);
  assert(process->stdout);
  assert(buffer);

  return pipe_read(process->stdout, buffer, to_read, actual);
}

PROCESS_ERROR process_read_stderr(process *process, void *buffer,
                                  uint32_t to_read, uint32_t *actual)
{
  assert(process);
  assert(process->stderr);
  assert(buffer);

  return pipe_read(process->stderr, buffer, to_read, actual);
}

PROCESS_ERROR process_wait(process *process, uint32_t milliseconds)
{
  assert(process);
  assert(process->info.hProcess);

  SetLastError(0);

  DWORD wait_result = WaitForSingleObject(process->info.hProcess, milliseconds);

  switch (wait_result) {
  case WAIT_TIMEOUT:
    return PROCESS_WAIT_TIMEOUT;
  case WAIT_FAILED:
    return system_error_to_process_error(GetLastError());
  }

  return PROCESS_SUCCESS;
}

PROCESS_ERROR process_terminate(process *process, uint32_t milliseconds)
{
  assert(process);
  assert(process->info.dwProcessId);

  SetLastError(0);

  // Process group of process started with CREATE_NEW_PROCESS_GROUP is equal to
  // the process id
  if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process->info.dwProcessId)) {
    return system_error_to_process_error(GetLastError());
  }

  return process_wait(process, milliseconds);
}

PROCESS_ERROR process_kill(process *process, uint32_t milliseconds)
{
  assert(process);
  assert(process->info.hProcess);

  SetLastError(0);

  if (!TerminateProcess(process->info.hProcess, 0)) {
    return system_error_to_process_error(GetLastError());
  }

  return process_wait(process, milliseconds);
}

PROCESS_ERROR process_free(process *process)
{
  assert(process);

  SetLastError(0);

  // NULL checks so free works even if process was only partially initialized
  // (can happen if error occurs during initialization or if start is not
  // called)
  if (process->stdin) { CloseHandle(process->stdin); };
  if (process->stdout) { CloseHandle(process->stdout); };
  if (process->stderr) { CloseHandle(process->stderr); };
  if (process->info.hProcess) { CloseHandle(process->info.hProcess); };

  PROCESS_ERROR error = system_error_to_process_error(GetLastError());

  return error;
}

int64_t process_system_error(void) { return GetLastError(); }
