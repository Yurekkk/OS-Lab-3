#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32

    #include <windows.h>
    #include <memoryapi.h>

    #define get_current_pid() GetCurrentProcessId()

#else // POSIX

    #include <unistd.h>

    #define get_current_pid() getpid()

#endif

#define LOG_FILE "counter.log"
#define TIME_STR_SIZE 32
#define counter_t unsigned long long

typedef struct {
    counter_t value;
} SharedCounter;

HANDLE SharedCounter_hMap = NULL;



int main() {
    char start_msg[] = "Process launched.";
    log_msg(start_msg);

    SharedCounter* counter = get_counter_ptr();

    BOOL is_main_process;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        is_main_process = FALSE;  // объект уже существовал
    } else {
        is_main_process = TRUE;   // мы создали его, значит процесс главный
    }

    if (is_main_process)
        set_counter(counter, 0);


}



char* get_time_str() {
    time_t now;
    struct tm tm_info;
    char* time_str = malloc(TIME_STR_SIZE * sizeof(char));

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
    fprintf(f, "[%s] (PID: %lu) MSG: %s\n", time_str, (unsigned long) get_current_pid(), msg);
    free(time_str);
    fclose(f);
    
    ReleaseMutex(hMutex);   // отпускаем мьютекс
    CloseHandle(hMutex);
}

SharedCounter* get_counter_ptr() {
    // Создает или открывает общий объект для всех процессов
    SharedCounter_hMap = CreateFileMapping(
        INVALID_HANDLE_VALUE,   // не привязан к файлу на диске
        NULL,                   // атрибуты безопасности (по умолчанию)
        PAGE_READWRITE,         // права
        0,                      // старшие 32 бита размера, без понятия, так было в документации
        sizeof(SharedCounter),  // младшие 32 бита размера
        "SharedCounter"         // имя объекта разделяемой памяти
    );

    // Сопоставляет представление сопоставления файлов в адресное пространство вызывающего процесса.
    SharedCounter* counter = (SharedCounter*) MapViewOfFile(
        SharedCounter_hMap,
        FILE_MAP_ALL_ACCESS,    // права
        0, 0,                   // старшие и младшие 32 бита
        sizeof(SharedCounter)   // сколько байт отобразить
    );

    return counter;
}

void set_counter(SharedCounter* counter, counter_t new_val) {
    // Мью́текс (англ. mutex, от mutual exclusion — «взаимное исключение») — 
    // примитив синхронизации, обеспечивающий взаимное исключение исполнения 
    // критических участков кода.

    HANDLE hMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "CounterMutex"  // имя объекта мьютекса
    );

    WaitForSingleObject(hMutex, INFINITE);  // ждем, пока мьютекс освободится
    counter->value = new_val;               // безопасно меняем общую переменную
    ReleaseMutex(hMutex);                   // отпускаем мьютекс
    CloseHandle(hMutex);
}

counter_t get_counter(SharedCounter* counter) {
    HANDLE hMutex = CreateMutex(
        NULL,           // атрибуты безопасности (по умолчанию)
        FALSE,          // вызывающий поток не получает права владения мьютексом изначально
        "CounterMutex"  // имя объекта мьютекса
    );

    WaitForSingleObject(hMutex, INFINITE);  // ждем, пока мьютекс освободится
    counter_t to_return = counter->value;
    ReleaseMutex(hMutex);                   // отпускаем мьютекс
    CloseHandle(hMutex);

    return to_return;
}
