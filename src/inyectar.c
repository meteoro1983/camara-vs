/*
 * inyectar.c - carga camara_vs.dll en un WE9 / WE9LE que ya esta corriendo (sin Kitserver,
 * o para probar sin reiniciar el partido).
 *   inyectar.exe [proceso.exe] [ruta\camara_vs.dll]
 * Por defecto busca WE9LEK.exe o we9.exe y la DLL junto a este exe.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD buscar_pid(const char *nombre) {
    PROCESSENTRY32 e = {sizeof e};
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;
    if (Process32First(s, &e))
        do { if (!_stricmp(e.szExeFile, nombre)) { pid = e.th32ProcessID; break; } } while (Process32Next(s, &e));
    CloseHandle(s);
    return pid;
}

int main(int argc, char **argv) {
    static const char *juegos[] = {"WE9LEK.exe", "we9.exe", "PES5.exe"};
    char dll[MAX_PATH], *p;
    DWORD pid = 0, codigo = 0;
    HANDLE h, hilo;
    void *remoto;
    int i;
    if (argc > 1) pid = buscar_pid(argv[1]);
    for (i = 0; !pid && argc <= 1 && i < 3; i++) pid = buscar_pid(juegos[i]);
    if (!pid) { printf("no encuentro el juego corriendo\n"); return 1; }
    if (argc > 2) GetFullPathNameA(argv[2], MAX_PATH, dll, NULL);
    else {
        GetModuleFileNameA(NULL, dll, MAX_PATH);
        if ((p = strrchr(dll, '\\')) != NULL) strcpy(p + 1, "camara_vs.dll");
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) { printf("no encuentro %s\n", dll); return 1; }
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) { printf("no se pudo abrir el proceso %lu (error %lu)\n", pid, GetLastError()); return 1; }
    remoto = VirtualAllocEx(h, NULL, MAX_PATH, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(h, remoto, dll, strlen(dll) + 1, NULL);
    hilo = CreateRemoteThread(h, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"), remoto, 0, NULL);
    if (!hilo) { printf("no se pudo crear el hilo remoto (error %lu)\n", GetLastError()); return 1; }
    WaitForSingleObject(hilo, 10000);
    GetExitCodeThread(hilo, &codigo);
    printf(codigo ? "cargada %s en el proceso %lu\n" : "fallo la carga de %s en %lu\n", dll, pid);
    return codigo ? 0 : 1;
}
