/* Temporary diagnostic branch only: all processes and files belong to this probe. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static int probe(const wchar_t *bin, const char *name, BOOL use_job, BOOL new_group) {
    wchar_t path[MAX_PATH], log_path[MAX_PATH], command[4096];
    GetTempPathW(MAX_PATH, path);
    GetTempFileNameW(path, L"cbp", 0, log_path);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE log = CreateFileW(log_path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, NULL);
    HANDLE handles[] = {nul, log};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &bytes) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), NULL, NULL)) return 2;
    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attrs;
    HANDLE job = NULL;
    if (use_job) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                             &limits, sizeof(limits))) return 2;
    }
    const wchar_t *arguments = wcsstr(bin, L"cmd.exe")
        ? L"/d /s /c \"echo cbm-owned-startup-ready\""
        : L"-NoProfile -NonInteractive -Command \"Write-Output 'cbm-owned-startup-ready'\"";
    swprintf(command, 4096, L"\"%ls\" %ls", bin, arguments);
    PROCESS_INFORMATION child = {0};
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    if (new_group) flags |= CREATE_NEW_PROCESS_GROUP;
    ULONGLONG started = GetTickCount64();
    BOOL created = CreateProcessW(bin, command, NULL, NULL, TRUE, flags, NULL, NULL,
                                   &si.StartupInfo, &child);
    DWORD create_error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    HeapFree(GetProcessHeap(), 0, attrs);
    CloseHandle(nul);
    if (!created) {
        printf("%s create_failed=%lu\n", name, create_error);
        if (job) CloseHandle(job);
        CloseHandle(log);
        DeleteFileW(log_path);
        return 1;
    }
    if (job && !AssignProcessToJobObject(job, child.hProcess)) {
        printf("%s assignment_failed=%lu\n", name, GetLastError());
        TerminateProcess(child.hProcess, 1);
    } else {
        ResumeThread(child.hThread);
    }
    ULONGLONG first_output = 0;
    BOOL terminal = FALSE;
    while (GetTickCount64() - started < 30000) {
        LARGE_INTEGER size;
        if (!first_output && GetFileSizeEx(log, &size) && size.QuadPart > 0)
            first_output = GetTickCount64() - started;
        if (WaitForSingleObject(child.hProcess, 10) == WAIT_OBJECT_0) {
            terminal = TRUE;
            break;
        }
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(child.hProcess, &exit_code);
    printf("%s job=%d group=%d first_output_ms=%llu elapsed_ms=%llu terminal=%d exit=%lu\n",
           name, use_job, new_group, first_output, GetTickCount64() - started, terminal, exit_code);
    if (!terminal) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
    }
    SetFilePointer(log, 0, NULL, FILE_BEGIN);
    char output[1024] = {0};
    DWORD count = 0;
    ReadFile(log, output, sizeof(output) - 1, &count, NULL);
    printf("%s output=%s\n", name, output);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (job) CloseHandle(job);
    CloseHandle(log);
    DeleteFileW(log_path);
    fflush(stdout);
    return 0;
}

int main(void) {
    wchar_t system[MAX_PATH], ps[1024], cmd[1024], pwsh[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    swprintf(ps, 1024, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system);
    swprintf(cmd, 1024, L"%ls\\cmd.exe", system);
    printf("pointer_bits=%zu ps=%ls\n", sizeof(void *) * 8, ps);
    int status = probe(cmd, "cmd-job", TRUE, TRUE);
    status |= probe(ps, "ps-no-job", FALSE, TRUE);
    status |= probe(ps, "ps-job", TRUE, TRUE);
    status |= probe(ps, "ps-job-no-group", TRUE, FALSE);
    DWORD found = SearchPathW(NULL, L"pwsh.exe", NULL, MAX_PATH, pwsh, NULL);
    if (found && found < MAX_PATH) status |= probe(pwsh, "pwsh-job", TRUE, TRUE);
    else printf("pwsh unavailable error=%lu\n", GetLastError());
    return status;
}
