#include "ImNodal/Test_ImNodal.h"
#include <string>

int main(int argc, char** argv) {
    if (argc > 1) {
        printf("Exec ImNodal test : %s\n", argv[1]);
        return Test_ImNodal(argv[1]) ? 0 : 1;
    }

    // return Test_ImNodal("") ? 0 : 1;
    
    return 0;
}