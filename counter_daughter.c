#include "counter.h"

int main(int argc, char* argv[]) {
    char role = argv[1][0];
    switch (role) {
        case '1':
            // Копия 1
            copy1_function();
            break;
        case '2':
            // Копия 2
            copy2_function();
            break;
    }

    return 0;
}