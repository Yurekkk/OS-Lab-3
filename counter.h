#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>

#ifdef _WIN32
    #include <windows.h>
    #include <memoryapi.h>
#else // POSIX
    #include <unistd.h>
    #include <sys/types.h>
#endif

#ifdef _WIN32
    #define get_current_pid()   GetCurrentProcessId()
    #define sleep_ms(ms)        Sleep(ms)
#else // POSIX
    #define get_current_pid()   getpid()
    #define sleep_ms(ms)        usleep(ms * 1000)
#endif

#define LOG_FILE "counter.log"
#define TIME_STR_SIZE 32
#define INCREMENT_DELAY 300         // in ms
#define LOG_COUNTER_DELAY 1000      // in ms
#define LAUNCH_COPIES_DELAY 3000    // in ms
#define COPY2_DELAY 2000            // in ms
#define counter_t unsigned long long

typedef struct {
    counter_t counter;
    long leader_pid;
} SharedData;

typedef struct {
#ifdef _WIN32
    HANDLE hProcess;
#else // POSIX
    unsigned int pid;
#endif
} app_info;

volatile BOOL quit_flag = FALSE;
HANDLE SharedData_hMap = NULL;
SharedData* data;

double get_curr_time();
char* get_time_str();
void log_msg(char* msg);
void log_counter_val();
char* trimspaces(char *str);

SharedData* get_data_ptr();
HANDLE lockDataMutex();
void unlockDataMutex(HANDLE hMutex);

app_info* launch_daughter_process(int argc);
void close_process_handle(app_info* app_info);
BOOL process_is_completed(app_info* app_info);
BOOL process_is_alive(long pid);
void await_app(app_info* app_info);

void main_counter_function();
DWORD WINAPI terminal_thread(LPVOID arg);
void copy1_function();
void copy2_function();



double get_curr_time() {
#ifdef _WIN32
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return  (double) now.QuadPart / freq.QuadPart * 1000.0;
#else // POSIX
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1e3 + now.tv_nsec / 1e6;
#endif
}

char* get_time_str() {
    time_t now;
    struct tm tm_info;
    char* time_str = (char*) malloc(TIME_STR_SIZE * sizeof(char));

    time(&now);

    // Потокобезопасное получение структуры даты и времени
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif

    strftime(time_str, TIME_STR_SIZE, "%Y-%m-%d %H:%M:%S", &tm_info);

    return time_str;
}

void log_msg(char* msg) {
    HANDLE hMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "LogMutex"      // имя объекта мьютекса
    );

    WaitForSingleObject(hMutex, INFINITE);  // ждем, пока мьютекс освободится
    
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) {
        perror("Couldn't open the file!");
        return;
    }

    char* time_str = get_time_str();
    fprintf(f, "[%s] (PID: %lu)\tMSG: %s\n", time_str, (unsigned long) get_current_pid(), msg);
    free(time_str);
    fclose(f);
    
    ReleaseMutex(hMutex);   // отпускаем мьютекс
    CloseHandle(hMutex);
}

void log_counter_val() {
    HANDLE hMutex = lockDataMutex();
    counter_t val = data->counter;
    unlockDataMutex(hMutex);
    
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Counter value is %d.", val);
    log_msg(buffer);
}

char* trimspaces(char *str) {
    // Функция, убирающая пробелы на концах строки

    char *end;

    // Trim leading spaces
    while(isspace((unsigned char)*str)) str++;

    if(*str == 0)  // All spaces?
    return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;

    // Write new null terminator character
    end[1] = '\0';

    return str;
}



SharedData* get_data_ptr() {
    // Создает или открывает (если уже есть) общий объект для всех процессов
    SharedData_hMap = CreateFileMapping(
        INVALID_HANDLE_VALUE,   // не привязан к файлу на диске
        NULL,                   // атрибуты безопасности (по умолчанию)
        PAGE_READWRITE,         // права
        0,                      // старшие 32 бита размера, честно, без понятия, так было в документации
        sizeof(SharedData),  // младшие 32 бита размера
        "SharedData"         // имя объекта разделяемой памяти
    );

    // Сопоставляет представление сопоставления файлов в адресное пространство вызывающего процесса.
    data = (SharedData*) MapViewOfFile(
        SharedData_hMap,
        FILE_MAP_ALL_ACCESS,    // права
        0, 0,                   // старшие и младшие 32 бита
        sizeof(SharedData)   // сколько байт отобразить
    );

    return data;
}

HANDLE lockDataMutex() {
    // Мью́текс (англ. mutex, от mutual exclusion — «взаимное исключение») — 
    // примитив синхронизации, обеспечивающий взаимное исключение исполнения 
    // критических участков кода.
    // Стандарт C не обязывает атомики работать межпроцессно.

    HANDLE hMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "DataMutex"     // имя объекта мьютекса
    );
    WaitForSingleObject(hMutex, INFINITE);  // ждем, пока мьютекс освободится
    return hMutex;
}

void unlockDataMutex(HANDLE hMutex) {
    ReleaseMutex(hMutex);   // отпускаем мьютекс
    CloseHandle(hMutex);
}



app_info* launch_daughter_process(int argc) {
#ifdef _WIN32

    // Всякая разная информация
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);

    char buffer[100];
    snprintf(buffer, sizeof(buffer), "counter_daughter.exe %d", argc);

    // Запускаем программу
    WINBOOL success = CreateProcessA(
        NULL,
        buffer,     // lpCommandLine
        NULL,       // lpProcessAttributes,
        NULL,       // lpThreadAttributes,
        TRUE,       // bInheritHandles,
        0,          // dwCreationFlags,
        NULL,       // lpEnvironment,
        NULL,       // lpCurrentDirectory,
        &si,        // lpStartupInfo,
        &pi         // lpProcessInformation
    );

    if (!success) {
        printf("Copy process did not open.\n");
        return NULL;
    }

    // Поток не нужен, закрываем его дескриптор
    CloseHandle(pi.hThread);

    // Возвращаем указатель на процесс
    app_info* info = (app_info*) malloc(sizeof(app_info));
    info->hProcess = pi.hProcess;
    return info;

#else // POSIX

    // Создаем дочерний процесс
    pid_t pid = fork();

    if (pid == 0) {
        // Дочерний процесс
        char* const argv[] = {"counter_daughter", argc_char};
        execv("counter_daughter", argv);

        // Если execv успешен — дочерний 
        // процесс больше не выполняет код.

        // Если execv неуспешен
        _exit(127);
    } else if (pid > 0) {
        // Родительский процесс
        app_info* info = (app_info*) malloc(sizeof(app_info));
        info->pid = pid;
        return info;
    } else {
        // fork не сработал
        return NULL;
    }

#endif
}

void close_process_handle(app_info* app_info) {
#ifdef _WIN32
    CloseHandle(app_info->hProcess);
    // Выполнение подпроцесса не останавливается после закрытия хэндла
#endif
    free(app_info);
}

BOOL process_is_completed(app_info* app_info) {
#ifdef _WIN32

    DWORD exitCode;
    return (GetExitCodeProcess(app_info->hProcess, &exitCode) && 
        exitCode != STILL_ACTIVE);

#else // POSIX

    int status;
    pid_t result = waitpid(app_info->pid, &status, WNOHANG);
    if (result == 0) {
        return FALSE;
    } else if (result == pid) {
        return TRUE;
    } else {
        // ошибка
        return -1;
    }

#endif
}

BOOL process_is_alive(long pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == NULL) return FALSE;

    DWORD exit_code = 0;
    BOOL success = GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);

    if (!success) return FALSE;
    return (exit_code == STILL_ACTIVE);
}

void await_app(app_info* app_info) {
#ifdef _WIN32
    // Ожидаем завершения процесса
    const unsigned long awaitTime = INFINITE;
    WaitForSingleObject(app_info->hProcess, awaitTime);
#else // POSIX
    // Ожидаем завершения процесса
    int status;
    pid_t result = waitpid(app_info->pid, &status, 0);
#endif
}



void main_counter_function() {
    char start_msg[] = "Main process launched.";
    log_msg(start_msg);

    CreateThread(NULL, 0, terminal_thread, NULL, 0, NULL);

    data = get_data_ptr();

    // Проверяем, создан ли новый объект
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        // Если да, то инициализируем данные
        HANDLE hMutex = lockDataMutex();
        data->counter = 0;
        data->leader_pid = get_current_pid();
        unlockDataMutex(hMutex);
    } 

    app_info* copy_1_info;
    app_info* copy_2_info;
    BOOL copies_previously_launched = FALSE;
    long current_pid = get_current_pid();

    double now = get_curr_time();

    time_t prev_incr_time = now;
    time_t prev_log_counter_time = now;
    time_t prev_copy_launch_time = now;

    // Основной цикл
    while (!quit_flag) {
        // Каждый раз пытаемся стать новым лидером, если старый умер
        HANDLE hMutex = lockDataMutex();
        if (data->leader_pid == -1 || !process_is_alive(data->leader_pid))
            data->leader_pid = current_pid;
        BOOL is_leader = (current_pid == data->leader_pid);
        unlockDataMutex(hMutex);

        now = get_curr_time();

        if (now - prev_incr_time >= INCREMENT_DELAY) {
            // Инкрементировать счетчик
            now = get_curr_time();
            prev_incr_time = now;

            HANDLE hMutex = lockDataMutex();
            data->counter++;
            unlockDataMutex(hMutex);
        }

        if (now - prev_log_counter_time >= LOG_COUNTER_DELAY) {
            // Записать значение счетчика в лог
            now = get_curr_time();
            prev_log_counter_time = now;
            if (is_leader)
                log_counter_val();
        }

        if (now - prev_copy_launch_time >= LAUNCH_COPIES_DELAY) {
            // Запустить копии (или вывести сообщение, если прошлые 
            // копии еще не завершили работу)

            now = get_curr_time();
            prev_copy_launch_time = now;

            if (is_leader) {
                if (copies_previously_launched && (
                    !process_is_completed(copy_1_info) ||
                    !process_is_completed(copy_2_info))) {
                    char msg[] = "Previously launched copies have not completed yet.";
                    log_msg(msg);
                }
                else {
                    if (copies_previously_launched) {
                        // Закрываем предыдущие копии
                        close_process_handle(copy_1_info);
                        close_process_handle(copy_2_info);
                    }

                    copy_1_info = launch_daughter_process(1);
                    copy_2_info = launch_daughter_process(2);
                    copies_previously_launched = TRUE;
                }
            }
        }
    }

    HANDLE hMutex = lockDataMutex();
    data->leader_pid = -1;
    unlockDataMutex(hMutex);

    await_app(copy_1_info);
    await_app(copy_2_info);
    close_process_handle(copy_1_info);
    close_process_handle(copy_2_info);

    UnmapViewOfFile(data);
    CloseHandle(SharedData_hMap);

    char exit_msg[] = "Main process completed.";
    log_msg(exit_msg);
}

DWORD WINAPI terminal_thread(LPVOID arg) {
    // Отдельный поток ждет ввода в командную строку и 
    // изменяет значение счетчика при вводе

    data = get_data_ptr();
    char buffer[64];
    char *ptr;
    printf("Enter a number to change the value of the counter\n"\
           "or leave a blank line to terminate the process.\n");

    while (!quit_flag) {
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            quit_flag = TRUE;
            break;
        }

        // Убираем \n и \r\n
        buffer[strcspn(buffer, "\r\n")] = '\0';
        // Убираем пробелы на концах
        char* trimmed = trimspaces(buffer);

        if (trimmed[0] == '\0') {
            // Если строка была пустой
            quit_flag = TRUE;
            break;
        }

        // Проверяем, состоит ли строка только из цифр
        BOOL is_only_digits = TRUE;
        int i = 0;
        if (trimmed[0] == '-') i++;
        for (; trimmed[i] != '\0'; i++) {
            if (!isdigit((unsigned char)trimmed[i])) {
                is_only_digits = FALSE;
                break;
            }
        }

        if (!is_only_digits) {
            printf("Invalid input.\n");
            continue;
        }
        else {
            counter_t num = strtoull(trimmed, &ptr, 10);
            HANDLE hMutex = lockDataMutex();
            data->counter = num;
            unlockDataMutex(hMutex);
            printf("Value is set.\n");
        }
    }
}

void copy1_function() {
    char start_msg[] = "Copy 1 process launched.";
    log_msg(start_msg);

    data = get_data_ptr();
    HANDLE hMutex = lockDataMutex();
    data->counter += 10;
    unlockDataMutex(hMutex);
    // log_counter_val();

    char exit_msg[] = "Copy 1 process completed.";
    log_msg(exit_msg);
}

void copy2_function() {
    char start_msg[] = "Copy 2 process launched.";
    log_msg(start_msg);

    data = get_data_ptr();
    HANDLE hMutex = lockDataMutex();
    data->counter *= 2;
    unlockDataMutex(hMutex);
    // log_counter_val();

    sleep_ms(COPY2_DELAY);

    hMutex = lockDataMutex();
    data->counter /= 2;
    unlockDataMutex(hMutex);
    // log_counter_val();

    char exit_msg[] = "Copy 2 process completed.";
    log_msg(exit_msg);
}

