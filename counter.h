/*
Задание "Позволяет пользователю через интерфейс командной 
строки установить любое значение счетчика"
- Реализовано через отдельный поток, слушающий терминал

Задание "Пользователь может запустить любое количество 
программ. В этом случае только одна из программ должна 
писать в лог текущее значение счетчика и порождать копии"
- Реализовано через leader_pid в SharedData

Data race в windows устраняется с помощью mutex;
в linux для файла устраняется с помощью flock, 
для shared data - с помощью семафоров.
Atomic не обязательно работает межпроцессно

Закрытие процесса после нажатия на enter в терминале
происходит с помощью volatile BOOL флажка
*/



#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L // Гарантирует доступ к sem_open, shm_open и прочему
#endif

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    #include <memoryapi.h>
#else // POSIX
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/file.h>
    #include <sys/wait.h> 
    #include <sys/mman.h>
    #include <pthread.h>
    #include <semaphore.h>
    #include <errno.h> 
    #include <ctype.h> 
    #include <signal.h> 
#endif



#ifdef _WIN32
    #define get_current_pid()   GetCurrentProcessId()
    #define sleep_ms(ms)        Sleep(ms)
#else // POSIX
    #define get_current_pid()   getpid()
    typedef void* HANDLE;
    typedef int BOOL;
    #define TRUE 1
    #define FALSE 0
    static inline void sleep_ms(unsigned long ms) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
#endif

#define SHARED_DATA_MAGIC_NUMBER 2465502718961455901
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
    // Черт знает, как проверять на линуксе, инициализирована ли структура
    // или нет и является ли процесс первым запущенным или нет
    // Обычный флажок не пойдет, потому что там может быть мусорное TRUE
    // Поэтому структуру считаем инициализированной тогда, когда магическое
    // число совпадает с заданным (вероятность случайного совпадения
    // мусорного значения и заданного = 1/2^64)
    unsigned long long magic_number;
} SharedData;

typedef struct {
#ifdef _WIN32
    HANDLE hProcess;
#else // POSIX
    unsigned int pid;
#endif
} app_info;



volatile BOOL quit_flag = FALSE;
SharedData* data;

#ifdef _WIN32
    HANDLE SharedData_hMap = NULL;
    HANDLE hDataMutex = NULL;
#else // POSIX
    int shm_fd = -1;
    sem_t* shm_sem = NULL;
#endif



double get_curr_time();
char* get_time_str();
void log_msg(char* msg);
void log_counter_val();
char* trimspaces(char *str);

SharedData* get_data_ptr();
void initDataMaybe();
void initSync();
void lockData();
void unlockData();
void cleanupDataSync();

app_info* launch_daughter_process(int argc);
void close_process_handle(app_info* app_info);
BOOL process_is_completed(app_info* app_info);
BOOL process_is_alive(long pid);
void await_app(app_info* app_info);
void launch_daughter_thread(void* (*func)(void*));

void main_counter_function();
void* terminal_func(void* arg);
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
#else // POSIX
    localtime_r(&now, &tm_info);
#endif

    strftime(time_str, TIME_STR_SIZE, "%Y-%m-%d %H:%M:%S", &tm_info);

    return time_str;
}

void log_msg(char* msg) {
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) {
        perror("Couldn't open the file!");
        return;
    }

#ifdef _WIN32
    HANDLE hLogMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "LogMutex"      // имя объекта мьютекса
    );
    if (!hLogMutex) {
        perror("Mutex failed!");
        fclose(f);
        return;
    }

    // ждем, пока мьютекс освободится
    DWORD waitResult = WaitForSingleObject(hLogMutex, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        // Ошибка ожидания
        CloseHandle(hLogMutex);
        fclose(f);
        return;
    }
#else // POSIX
    if (flock(fileno(f), LOCK_EX) != 0) {
        perror("flock failed!");
        fclose(f);
        return;
    }
#endif

    char* time_str = get_time_str();
    fprintf(f, "[%s] (PID: %lu)\tMSG: %s\n", time_str, (unsigned long) get_current_pid(), msg);
    free(time_str);

#ifdef _WIN32
    ReleaseMutex(hLogMutex);   // отпускаем мьютекс
    CloseHandle(hLogMutex);
#else // POSIX
    flock(fileno(f), LOCK_UN);
#endif

    fclose(f);
}

void log_counter_val() {
    lockData();
    counter_t val = data->counter;
    unlockData();
    
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "Counter value is %llu.", val);
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
#ifdef _WIN32

    // Создает или открывает (если уже есть) общий объект для всех процессов
    SharedData_hMap = CreateFileMapping(
        INVALID_HANDLE_VALUE,   // не привязан к файлу на диске
        NULL,                   // атрибуты безопасности (по умолчанию)
        PAGE_READWRITE,         // права
        0,                      // старшие 32 бита размера, честно, без понятия, так было в документации
        sizeof(SharedData),  // младшие 32 бита размера
        "SharedData"         // имя объекта разделяемой памяти
    );

    if (!SharedData_hMap) {
        perror("CreateFileMapping failed");
        return NULL;
    }

    // Сопоставляет представление сопоставления файлов в адресное пространство вызывающего процесса.
    data = (SharedData*) MapViewOfFile(
        SharedData_hMap,
        FILE_MAP_ALL_ACCESS,    // права
        0, 0,                   // старшие и младшие 32 бита
        sizeof(SharedData)   // сколько байт отобразить
    );

    return data;

#else // POSIX
    
    // Создаём или открываем объект разделяемой памяти
    shm_fd = shm_open("/SharedData", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        return NULL;
    }

    // Устанавливаем размер
    if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
        perror("ftruncate failed");
        close(shm_fd);
        return NULL;
    }

    // Отображаем в память
    data = (SharedData*) mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap failed");
        close(shm_fd);
        return NULL;
    }

    return data;

#endif
}

void initDataMaybe() {
    // Инициализируем данные, если они не инициализированы
    lockData();
    if (data->magic_number != SHARED_DATA_MAGIC_NUMBER) {
        data->counter = 0;
        data->leader_pid = get_current_pid();
        data->magic_number = SHARED_DATA_MAGIC_NUMBER;
    } 
    unlockData();
}

void initSync() {
#ifdef _WIN32

    // Мью́текс (англ. mutex, от mutual exclusion — «взаимное исключение») — 
    // примитив синхронизации, обеспечивающий взаимное исключение исполнения 
    // критических участков кода.
    // Стандарт C не обязывает атомики работать межпроцессно.

    hDataMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "DataMutex"     // имя объекта мьютекса
    );
    if (!hDataMutex) {
        perror("CreateMutex failed");
        return;
    }

#else // POSIX

    // Создаём или открываем именованный семафор
    shm_sem = sem_open("/DataSem", O_CREAT, 0666, 1);
    if (shm_sem == SEM_FAILED) {
        perror("sem_open failed");
        return;
    }

#endif
}

void lockData() {
#ifdef _WIN32

    // ждем, пока мьютекс освободится
    DWORD waitResult = WaitForSingleObject(hDataMutex, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        // Ошибка ожидания
        CloseHandle(hDataMutex);
        return;
    }

#else // POSIX

    // Ждём (захватываем)
    if (sem_wait(shm_sem) == -1) {
        perror("sem_wait failed");
        return;
    }

#endif
}

void unlockData() {
#ifdef _WIN32
    ReleaseMutex(hDataMutex);   // отпускаем мьютекс
#else // POSIX
    if (sem_post(shm_sem) == -1) {
        perror("sem_post failed");
    }
#endif
}

void cleanupDataSync() {
#ifdef _WIN32
    if (hDataMutex) {
        CloseHandle(hDataMutex);
        hDataMutex = NULL;
    }
    if (data) {
        UnmapViewOfFile(data);
        data = NULL;
    }
    if (SharedData_hMap) {
        CloseHandle(SharedData_hMap);
        SharedData_hMap = NULL;
    }
#else
    if (shm_sem != SEM_FAILED) {
        sem_close(shm_sem);     // закрываем дескриптор семафора
        shm_sem = SEM_FAILED;
    }
    if (data && data != MAP_FAILED) {
        munmap(data, sizeof(SharedData));   // отключаем память
        data = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);  // закрываем файловый дескриптор
        shm_fd = -1;
    }
#endif
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
        char arg_str[16];
        snprintf(arg_str, sizeof(arg_str), "%d", argc);
        char* const argv[] = {"./counter_daughter", arg_str, NULL};
        execv("./counter_daughter", argv);

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
    } else if (result == app_info->pid) {
        return TRUE;
    } else {
        // ошибка
        return -1;
    }

#endif
}

BOOL process_is_alive(long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == NULL) return FALSE;

    DWORD exit_code = 0;
    BOOL success = GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);

    if (!success) return FALSE;
    return (exit_code == STILL_ACTIVE);

#else // POSIX

    // kill(pid, 0) проверяет существование процесса без отправки сигнала
    if (kill(pid, 0) == 0) {
        return TRUE;   // процесс существует
    } else {
        if (errno == ESRCH) {
            return FALSE;  // процесс не найден
        }
        // EPERM — процесс есть, но нет прав → считаем, что жив
        return TRUE;
    }

#endif
}

void await_app(app_info* app_info) {
#ifdef _WIN32
    const unsigned long awaitTime = INFINITE;
    WaitForSingleObject(app_info->hProcess, awaitTime);
#else // POSIX
    int status;
    pid_t result = waitpid(app_info->pid, &status, 0);
#endif
}

void launch_daughter_thread(void* (*func)(void*)) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, func, NULL, 0, NULL);
    if (!h) {
        perror("CreateThread failed");
        return;
    }
    CloseHandle(h);
#else // POSIX
    // Создаем detached поток
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, func, NULL) != 0) {
        perror("pthread_create failed");
    }
    pthread_attr_destroy(&attr);
#endif
}



void main_counter_function() {
    char start_msg[] = "Main process launched.";
    log_msg(start_msg);

    launch_daughter_thread(terminal_func);
    data = get_data_ptr();
    initSync();
    initDataMaybe();

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
        lockData();
        if (data->leader_pid == -1 || !process_is_alive(data->leader_pid))
            data->leader_pid = current_pid;
        BOOL is_leader = (current_pid == data->leader_pid);
        unlockData();

        now = get_curr_time();

        if (now - prev_incr_time >= INCREMENT_DELAY) {
            // Инкрементировать счетчик
            now = get_curr_time();
            prev_incr_time = now;

            lockData();
            data->counter++;
            unlockData();
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

    lockData();
    data->leader_pid = -1;
    unlockData();

    await_app(copy_1_info);
    await_app(copy_2_info);
    close_process_handle(copy_1_info);
    close_process_handle(copy_2_info);

    char exit_msg[] = "Main process completed.";
    log_msg(exit_msg);

    cleanupDataSync();

    printf("Process terminated.\n");
}

void* terminal_func(void* arg) {
    // Отдельный поток ждет ввода в командную строку и 
    // изменяет значение счетчика при вводе

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
            printf("Terminating process...\n");
            quit_flag = TRUE;
            break;
        }

        // Проверяем, состоит ли строка только из цифр
        BOOL is_only_digits = TRUE;
        for (int i = 0; trimmed[i] != '\0'; i++) {
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
            lockData();
            data->counter = num;
            unlockData();
            printf("Value is set.\n");
        }
    }
}

void copy1_function() {
    char start_msg[] = "Copy 1 process launched.";
    log_msg(start_msg);

    data = get_data_ptr();
    initSync();

    lockData();
    data->counter += 10;
    unlockData();
    // log_counter_val();

    char exit_msg[] = "Copy 1 process completed.";
    log_msg(exit_msg);

    cleanupDataSync();
}

void copy2_function() {
    char start_msg[] = "Copy 2 process launched.";
    log_msg(start_msg);

    data = get_data_ptr();
    initSync();

    lockData();
    data->counter *= 2;
    unlockData();
    // log_counter_val();

    sleep_ms(COPY2_DELAY);

    lockData();
    data->counter /= 2;
    unlockData();
    // log_counter_val();

    char exit_msg[] = "Copy 2 process completed.";
    log_msg(exit_msg);

    cleanupDataSync();
}

