#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]){
    if(argc<2){
        printf("Usage: %s <num_messages>\n", argv[0]);
        return 1;
    }

    int num_messages = atoi(argv[1]);

    FILE* fin = fopen("long.txt", "r");
    if(fin == NULL){
        perror("Error opening file");
        return 1;
    }

    char word[100];
    int iterations = 0;
    while(fscanf(fin, " %[^,],", word) && iterations < num_messages){
        printf("%s\n", word);
        iterations++;
    }

    fclose(fin);

    return 0;
}
